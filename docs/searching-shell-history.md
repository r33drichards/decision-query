# Tutorial: searching shell history in plain English

Your shell history is a few tens of thousands of lines of text you can only grep.
`grep kubectl` finds `kubectl`; it does not find `helm`, `k9s` or `kubectx` unless
you already thought of them. This tutorial points decision-query at an
[atuin](https://atuin.sh) history database so you can ask questions instead:
*"is this command Kubernetes-related?"*, *"does this line contain an API key?"*

Everything runs locally. No text leaves the machine.

Worked example at the end: finding credentials you have pasted into a terminal,
and sorting them by which vendor issued them.

## What you need

- macOS on Apple silicon, or Linux with an NVIDIA GPU (a CPU-only build works
  everywhere, about five times slower)
- A C++20 compiler, CMake 3.24+, ICU, nlohmann-json
- Python, to fetch the model once
- ~1 GB of disk for the checkpoint

The commands below use [nix](https://nixos.org) to supply the build dependencies,
so nothing has to be installed system-wide. If you already have cmake and ICU,
drop the `nix-shell -p … --run` wrapper and run the inner command directly.

## 1. Clone and build

```bash
git clone https://github.com/r33drichards/decision-query.git
cd decision-query
git submodule update --init --recursive   # laya.cpp, ggml
nix-shell -p cmake icu nlohmann_json sqlite ninja --run 'make loadable-release'
```

That produces `dist/release/decision_query.dylib` (`.so` on Linux). The first build takes a
few minutes because it compiles ggml from source.

The GPU backend is selected automatically: Metal on Apple silicon, CUDA when a
CUDA toolkit is found, otherwise CPU. See [Accuracy](#accuracy-and-speed) before
relying on the Metal numbers.

## 2. Fetch the model

`make model` needs `huggingface_hub`. Give it a virtualenv and point the Makefile
at that interpreter:

```bash
python3 -m venv /tmp/hf && /tmp/hf/bin/pip install huggingface_hub
HF_HUB_DISABLE_XET=1 make model python=/tmp/hf/bin/python
```

This downloads a pinned Laya checkpoint (~840 MB) into `models/laya`.

Do not reach for `nix-shell -p 'python3.withPackages(...)'` here. It puts the
right interpreter on `PATH`, but inside the shell Python resolves `sys.prefix` to
the *base* interpreter, so the environment's `site-packages` never lands on
`sys.path` and the import fails anyway. `nix shell --expr` behaves the same way.

`HF_HUB_DISABLE_XET=1` is worth keeping. Without it the `hf_xet` transfer backend
can abort partway through `model.safetensors`, leave a `.incomplete` file, and
still exit `0` — so a failed download looks like a successful one. If inference
later complains the model is missing, check that `models/laya/model.safetensors`
is ~840 MB and that `models/laya/.cache` holds no `.incomplete` files.

## 3. Copy your history database

Work on a copy. The live database is open by your shell and has a hot
write-ahead log, so a plain `cp` can miss recent commands or capture a torn page.
`.backup` folds the WAL in and leaves the original untouched:

```bash
sqlite3 ~/.local/share/atuin/history.db ".backup '$HOME/atuin-copy.db'"
```

You now have one self-contained file, with no `-wal`/`-shm` beside it, that is
safe to query or mutate.

The table is `history(id, timestamp, duration, exit, command, cwd, session,
hostname, deleted_at)`. Two things to know: **`timestamp` is in nanoseconds**,
and rows atuin has soft-deleted carry a `deleted_at`.

## 4. Open a shell and load the extension

```bash
nix-shell -p sqlite --run "sqlite3 $HOME/atuin-copy.db"
```

Use this `sqlite3`, not the one at `/usr/bin/sqlite3` — the macOS system build has
extension loading compiled out, so `.load` fails there outright.

At the `sqlite>` prompt:

```sql
.load ./dist/release/decision_query
select dq_load('./models/laya');
```

`dq_load` returns the backend it selected: `MTL0` for Metal, `CUDA0` for CUDA,
`CPU` otherwise. One model stays resident per process.

Dot-commands must start at column 0. Indenting `.load` inside a script produces a
confusing `near ".": syntax error`.

Turn on readable output while you are exploring:

```sql
.mode box
.timer on
```

## 5. Check it works

```sql
select noul('kubectl get pods', 'Is this command related to Kubernetes?');
```

`noul` returns the probability that the statement holds, 0.0 to 1.0. Expect
something close to `1.0`, in about 0.1 s on a GPU backend. If that works, the
extension, the model and the database are all wired up.

## 6. Ask a question of your history

Now the real thing. This scans the last 30 days:

```sql
select
  -- P(this command contains a credential), one model pass per row
  round(noul(command,
    'Does this command contain an API key, token, password, or secret credential?'), 3) as p,

  count(*) as n,                     -- how often you ran this exact command
  substr(command, 1, 56) as command  -- keep the table readable

from history

-- atuin timestamps are NANOSECONDS, hence the * 1000000000
where timestamp >= (unixepoch('now','-30 days') * 1000000000)
  and deleted_at is null

-- Score each DISTINCT command once rather than once per repetition.
-- This is the single most important line for performance.
group by command

order by p desc
limit 20;
```

**`group by command` is the trick.** The cost of this query is one model forward
pass per row, so collapsing repeats is a direct multiplier: on a typical history
the last 30 days are ~230 rows but only ~113 distinct commands, which halves the
work for identical results.

The same idea scales further. Classifying the ~700 distinct *command heads*
(`kubectl`, `helm`, `ssh`, …) covers a 40,000-row history in about a minute,
where scoring every row would take an hour.

## 7. Two questions in one pass

Knowing a line holds a secret is more useful when you also know *whose* secret it
is. `decide()` evaluates several questions about the same row in a single forward
pass — so asking "is this a credential?" **and** "which vendor issued it?" costs
the same as asking either one alone.

```sql
create temp table judged as
select
  command,
  count(*) as n,
  decide(command, json_object(
    -- a yes/no probability
    'secret', json_object(
      'type','noul',
      'instructions','Does this command contain an API key, token, password, or secret credential?'),
    -- and a category, from a fixed list
    'provider', json_object(
      'type','choice',
      'instructions','Which service or vendor issued the credential in this command?',
      'criteria', json_array('anthropic','openai','aws','azure','github',
                             'modal','deno','database','litellm','other'))
  )) as answer
from history
where deleted_at is null
  and timestamp >= (unixepoch('now','-30 days')  * 1000000000)
group by command;

-- decide() returns JSON, so pull the fields out with json_extract
select round(json_extract(answer,'$.secret.noul'), 3) as p,
       json_extract(answer,'$.provider.choice')       as provider,
       n,
       substr(command, 1, 46) as command
from judged
where json_extract(answer,'$.secret.noul') >= 0.95
order by p desc, n desc;
```

view the results 

```
.mode box
.headers on
select round(json_extract(answer,'$.secret.noul'),3) as p,
       json_extract(answer,'$.provider.choice')      as provider,
       n,
       command
from judged
order by p desc
limit 20;
```

Results from actual run 

```
limit 20;sqlite> sqlite>    ...>    ...>    ...>    ...>    ...>    ...> 
┌───────┬──────────┬────┬──────────────────────────────────────────────────────────────┐
│   p   │ provider │ n  │                           command                            │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.931 │ openai   │ 1  │ ! First copy your one-time code: A83E-55D1                   │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.768 │ openai   │ 1  │ CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1 claude --dangerously- │
│       │          │    │ skip-permissions                                             │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.721 │ openai   │ 33 │ CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1 claude --dangerously- │
│       │          │    │ skip-permissions                                             │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.719 │ aws      │ 1  │ ssh root@100.100.41.14                                       │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.703 │ openai   │ 1  │ $PY atari_live.py --game MsPacman --model $CK/atari-dag2f-rl │
│       │          │    │ cd                                                           │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.702 │ openai   │ 5  │ clear                                                        │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.625 │ openai   │ 20 │ cd                                                           │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.611 │ openai   │ 1  │ $PY atari_live.py --game Galaxian  --model $CK/atari-dag2f-r │
│       │          │    │ lcd                                                          │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.609 │ github   │ 1  │ gh auth refresh -s workflow && gh auth token | pbcopy        │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.579 │ openai   │ 1  │ ls                                                           │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.568 │ openai   │ 1  │ $PY atari_live.py --game Freeway  --model $CK/atari-dag2f-rl │
│       │          │    │ cd                                                           │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.562 │ openai   │ 20 │ CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1 claude --dangerously- │
│       │          │    │ skip-permissions --worktree                                  │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.55  │ openai   │ 1  │ $PY atari_live.py --game frogger  --model ~/laya-vision-smol │
│       │          │    │ vlm/checkpoints/atari-8g-1f                                  │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.493 │ openai   │ 1  │ ../venv/bin/python examples/atari_live.py --game Freeway --m │
│       │          │    │ odel thaitea/laya-vision-smolvlm-256m                        │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.479 │ openai   │ 1  │ $PY atari_live.py --game Frogger  --model ~/laya-vision-smol │
│       │          │    │ vlm/checkpoints/atari-8g-1f                                  │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.47  │ other    │ 1  │ pwd                                                          │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.452 │ openai   │ 1  │ test                                                         │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.413 │ openai   │ 1  │ $PY atari_live.py --game Qbert  --model ~/laya-vision-smolvl │
│       │          │    │ m/checkpoints/atari-8g-1f                                    │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.402 │ openai   │ 3  │ PI_EXPERIMENTAL=1 PI_SERVER_DIR=/tmp/pisrv ~/pi/pi-test.sh c │
│       │          │    │ lient                                                        │
├───────┼──────────┼────┼──────────────────────────────────────────────────────────────┤
│ 0.396 │ openai   │ 1  │ $PY atari_live.py --game Galaxian  --model ~/laya-vision-smo │
│       │          │    │ lvlm/checkpoints/atari-8g-1f                                 │
└───────┴──────────┴────┴──────────────────────────────────────────────────────────────┘
sqlite> 
```

so secrets detection is not great in baseline laya, and neither was categorization. 

`decide()` also returns per-category probabilities, so you can see how confident the
choice was rather than just taking the label:

```sql
select json_extract(answer,'$.provider.probabilities') from judged limit 1;
-- {"anthropic":0.9986,"openai":0.0009,"aws":0.0003,…}
```

Add a category for anything you actually use. A vendor that has no category lands
in `other`, which is why the 56-occurrence key above is filed there rather than
under its real issuer.

## Accuracy and speed

**Measured on an Apple M3**, 20 decisions, model load excluded:

| Backend | Per decision | Notes |
|---|---|---|
| Metal | 0.103 s | default on Apple silicon |
| CPU FP32 | 0.533 s | exact, ~5x slower |

The first Metal run spends a few minutes compiling shaders and prints a wall of
`ggml_metal_library_compile_all` lines. That is cached; later runs load in about a
second. Do not mistake it for a hang.

**Metal trades exactness for speed.** ggml's Metal backend ignores the FP32
accumulation laya.cpp requests, so probabilities drift by up to ~0.001 against the
tolerance the project holds ports to. Selected categories are unaffected. For
filtering and ranking this is irrelevant; when the digits matter, use:

```sql
select dq_load('./models/laya', json_object('metal', 0));
```

**Read rankings, not absolute scores.** On a small or homogeneous window the
scores compress upward — in a 30-day window with no real secrets in it, `clear`
scored 0.70 and `cd` 0.62 purely because nothing outranked them. Treat ~0.5 as a
boundary you calibrate per corpus, not a universal threshold.

**Expect roughly half the top hits to be wrong.** The model responds to
credential-*shaped context* — `ssh user@host`, `-i ~/.ssh/key.pem`, a `:tag` after
a docker image — not to whether a token is really secret. On one real history,
45 of the 87 commands scoring >= 0.9 held an actual credential. That is a useful
filter over tens of thousands of lines, not an oracle; every hit needs an eyeball.

**Shape-based prefilters cut the wrong things.** It is tempting to prefilter by
Shannon entropy before paying for inference. On a real history that dropped a
production API key, because the key was formatted as a UUID and a "looks like a
UUID" rule discarded it. Position in the line (`-H "X-API-Key: …"`) identified it
when shape could not. If you prefilter, measure what the filter removes.

## If you find real credentials

Rotate them first. Then remember the history database itself still holds them, and
atuin **syncs** — so rotating the key does not remove it from the record, or from
wherever that record has replicated:

```bash
atuin search --delete --search-mode full-text 'sk-ant-'
```

Take care running the query itself on a shared screen: the output prints matching
commands in full, secrets included.

## Where to go next

- `score(text, question, json_array(...))` — an ordinal score, e.g. how
  destructive a command is
- `choice(text, question, json_array(...))` — a category on its own
- `decide(text, questions)` — several questions in one pass, as above
- [Load options](../README.md#load-options) — backend selection and model variants
- [Precision](../laya.cpp/docs/precision.md) — the accuracy contract and where
  Metal departs from it
