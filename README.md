# decision-query

Typed decisions as SQL functions, for SQLite and PostgreSQL. Ask a yes/no question,
pick from named options, or place a row on a rubric, and get calibrated probabilities
back -- so a table of text can be classified, scored or filtered without leaving the
database.

A decision comes from whichever backend you load. A local checkpoint runs in-process
through [laya.cpp](https://github.com/r33drichards/laya.cpp) and sends nothing anywhere;
an HTTP endpoint forwards to any service speaking the System One request shape. The SQL
is identical either way.

```sql
-- SQLite
.load ./dist/debug/decision_query
select id from tickets where noul(body, 'Does the customer request a refund?') > 0.5;

-- PostgreSQL
CREATE EXTENSION decision_query;
SET decision_query.model_dir = '/srv/models/laya';
SELECT id FROM tickets WHERE noul(body, 'Does the customer request a refund?') > 0.5;
```

Both modules provide `dq_load`, `dq_backend`, `dq_version`, `noul`,
`choice`, `score` and `decide()`, which returns the full answers object as JSON.

## Layout

| Directory | Contents |
|---|---|
| [`engine/`](engine/) | Shared C++ engine: one resident checkpoint per process, serialized calls, lazy loading. `decision_engine.hpp` plus the `decision-query-engine` CMake target that builds laya.cpp and ggml as static position-independent archives. |
| [`laya.cpp/`](laya.cpp/) | Git submodule with the native runtime, tokenizers and the `laya-cli` tool. |
| [`sqlite/`](sqlite/README.md) | SQLite loadable module, static library, Python wheel and tests. |
| [`postgres/`](postgres/README.md) | PostgreSQL extension built from the [pg_extension](https://github.com/mkindahl/pg_extension) CMake template, with pg_regress tests. |

The top-level `CMakeLists.txt` builds everything into one tree, so ggml and the laya
runtime compile once. Each module directory also configures on its own.

## Build

Requirements: a C++20 compiler, CMake 3.24+, ICU and nlohmann-json; the SQLite extension
headers for `sqlite/`; PostgreSQL server development files for `postgres/`; optionally
the CUDA toolkit. On Debian-like systems:

```sh
sudo apt-get install cmake ninja-build libicu-dev nlohmann-json3-dev libsqlite3-dev \
  postgresql-16 postgresql-server-dev-16
git clone --recurse-submodules https://github.com/r33drichards/decision-query.git
cd decision-query
make loadable static        # SQLite: dist/debug/decision_query.so, libdecision_query.a, decision_query.h
make postgres               # PostgreSQL: build/postgres/decision_query.so and decision_query.control
sudo make postgres-install  # into the directories reported by pg_config
```

The CUDA backend is enabled automatically when CMake finds a CUDA compiler. Force a
choice with `CMAKE_FLAGS='-DDQ_CUDA=OFF' make loadable` or `-DDQ_CUDA=ON`,
adding `-DCMAKE_CUDA_ARCHITECTURES=<arch>` for your GPU as described in the laya.cpp README.
The PostgreSQL module is configured automatically when `pg_config` and the server headers
are found; `-DDQ_POSTGRES=OFF` skips it.

## Models

Download a pinned checkpoint into `models/laya` (requires `pip install huggingface_hub`):

```sh
make model                            # english
make model MODEL_VARIANT=multilingual # models/laya/multilingual
make model MODEL_VARIANT=typed-decisions
```

The `english` checkpoint lives at the model-store root; the other variants live in a
subdirectory named after the variant, matching the laya.cpp layout.

## Load options

`dq_load(dir, options)` and the lazy-loading settings take a JSON object:

| Key | Default | Meaning |
|---|---|---|
| `variant` | `english` | `english`, `multilingual` or `typed-decisions`; appended to `dir` when not `english`. |
| `cuda` | true when built with CUDA | Use the CUDA backend. |
| `metal` | true on Apple builds without CUDA | Use the Apple Metal backend. Faster, but see the accuracy note below. |
| `key` | `$DQ_API_KEY` | Bearer token for an HTTP endpoint. Prefer the environment variable; a key written into SQL lands in your shell history. |
| `model` | `jev-latest` | Model name sent to an HTTP endpoint. |
| `tensor_core` | false | Compensated Tensor Core FP32 projections (CUDA). |
| `flash` | false | Fused FP32 attention (CUDA). |
| `bf16` | false | Native mixed BF16 (CUDA; implies `flash`). |

When no model is resident, the first inference call loads one from `DQ_MODEL_DIR` and
`DQ_OPTIONS` in the environment (PostgreSQL consults its `decision_query.model_dir` and
`decision_query.options` settings first). If nothing is found the call fails with
`No decision backend loaded; call dq_load(dir_or_url) or set DQ_MODEL_DIR`.

## Decision backends

`dq_load` takes a checkpoint directory or the URL of a service speaking the
System One request shape. The SQL functions are identical either way.

```sql
select dq_load('./models/laya');                             -- local, in-process
select dq_load('https://api.typesafe.ai/v1/systemone');      -- TypeSafe Jev
select dq_load('http://localhost:8000/v1/systemone',
                 json_object('model', 'reflex-4b'));           -- a local server
```

Credentials resolve in this order: the `key` option, then `key_file`, then
`$DQ_API_KEY_FILE`, then `$DQ_API_KEY`.

Prefer a file. A key written into a SQL statement is a key in your shell history,
and on PostgreSQL it also reaches `pg_stat_activity` and the statement log.

```sql
select dq_load('https://api.typesafe.ai/v1/systemone',
                 json_object('key_file', '/etc/decision-query/api-key'));
```

On PostgreSQL, set the superuser-only `decision_query.api_key_file` GUC instead:

```
decision_query.api_key_file = '/etc/decision-query/api-key'   # postgresql.conf
```

It names a path rather than holding the secret, so the value never appears in
`pg_settings`, `SHOW ALL` or a config dump. `$DQ_API_KEY` works there too but
is cluster-wide and cannot be scoped to a role.

`https://` requires OpenSSL at build time. CMake reports which you have:
`decision-query: HTTPS decision endpoints enabled`, or a note that only `http://` will work.

A remote backend sends the text you are asking about to that service. Local
checkpoints send nothing anywhere.

The two multi-option functions take **different shapes**, which is easy to get
wrong: `choice` takes an object of option to description, and `score` takes an
array of level descriptions, lowest first. A bare array passed to `choice` works
against a local checkpoint but is rejected by the System One HTTP shape, so the
object form is the portable one:

```sql
select choice('git push origin main', 'Which tool does this command use?',
                   json_object('git', 'the git version control tool',
                               'docker', 'the docker container tool'));
```

`score` places a row on an ordered rubric and returns a number in the range of
the levels you gave it:

```sql
select score('rm -rf /', 'How destructive is this?',
             json_array('harmless', 'local damage', 'irreversible'));
```

## Tutorial

[Searching shell history in plain English](docs/searching-shell-history.md) walks
through cloning, building, loading the extension against an
[atuin](https://atuin.sh) database, and asking questions like *"does this command
contain an API key, and whose is it?"*

## Tests

```sh
make test-loadable                                     # SQLite, no checkpoint needed
DQ_MODEL_DIR=models/laya make cli test-loadable      # SQLite with checkpoint and CLI parity
DQ_MODEL_DIR=models/laya DQ_OPTIONS='{"cuda": false}' make postgres
sudo make postgres-install && make test-postgres       # pg_regress, not as root
```

## Limitations

- Scalar functions run one forward pass per row. Use `decide()` to evaluate several
  questions about the same row in one batch. Cross-row batching is not available.
- Strict FP32 on the CPU is slow for a 28-layer encoder: about a second per question on a
  few cores. Use the CUDA build for table-scale workloads.
- **Metal trades exactness for speed, and is on by default on Apple hardware.** ggml's
  Metal backend ignores the FP32 accumulation the runtime requests, so public numbers
  drift beyond the 0.0001 tolerance the project holds ports to: measured at up to 0.0011
  on an M3, against this build's own CPU FP32 path. Selected categories were unaffected.
  Pass `json_object('metal', 0)` for numbers that satisfy the documented tolerance, at
  roughly five times the latency. See [Metal](laya.cpp/docs/precision.md#apple-metal).
- When built with CUDA, load the model before other CUDA users in the same process; the
  runtime disables TF32 before initializing cuBLAS.
- SQLite keeps one model per process; PostgreSQL backends each load their own copy. See
  the module READMEs.

## License

MIT, see [LICENSE](LICENSE). laya.cpp, ggml and the Laya checkpoints retain their own licenses.
