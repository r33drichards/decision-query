# sqlaya

A SQLite extension that answers [Laya](https://huggingface.co/convaiinnovations/laya) typed
decisions from SQL. It embeds [laya.cpp](https://github.com/r33drichards/laya.cpp), the
native C++ inference runtime, so a table of text can be classified, scored or filtered
without leaving the database:

```sql
.load ./dist/debug/laya
select laya_load('models/laya');

select id, subject from tickets
 where laya_noul(body, 'Does the customer request a refund?') > 0.5;

select id, laya_choice(body, 'Which department should handle this?',
                       json_array('billing', 'technical', 'sales')) as department
  from tickets;

select id, json_extract(laya(body, '{"urgency": {"type": "score",
        "instructions": "How urgent is the request?",
        "criteria": ["not urgent", "soon", "immediate"]}}'), '$.urgency.score') as urgency
  from tickets;
```

One checkpoint stays resident per process, on the GPU when built with CUDA or on the CPU
otherwise. Every SQLite connection in the process shares it.

A PostgreSQL extension with the same functions lives in [postgres/](postgres/README.md):

```sql
CREATE EXTENSION laya;
SET laya.model_dir = '/srv/models/laya';
SELECT id FROM tickets WHERE laya_noul(body, 'Does the customer request a refund?') > 0.5;
```

## Build

Requirements: a C++20 compiler, CMake 3.24+, ICU, nlohmann-json, and the SQLite extension
headers. Optional: the CUDA toolkit for GPU inference. On Debian-like systems:

```sh
sudo apt-get install cmake ninja-build libicu-dev nlohmann-json3-dev libsqlite3-dev
git clone --recurse-submodules https://github.com/r33drichards/sqlaya.git
cd sqlaya
make loadable          # dist/debug/laya.so (or laya.dylib)
make static            # dist/debug/libsqlite_laya.a and sqlaya.h
make loadable-release  # optimized build in dist/release
```

The CUDA backend is enabled automatically when CMake finds a CUDA compiler. Force a
choice with `CMAKE_FLAGS='-DSQLAYA_CUDA=OFF' make loadable` or `-DSQLAYA_CUDA=ON`,
adding `-DCMAKE_CUDA_ARCHITECTURES=<arch>` for your GPU as described in the laya.cpp README.
The module links ICU (and the CUDA runtime, when enabled) dynamically; ggml and the laya
runtime are folded into the module and only `sqlite3_laya_init` is exported.

## Models

Download a pinned checkpoint into `models/laya` (requires `pip install huggingface_hub`):

```sh
make model                            # english
make model MODEL_VARIANT=multilingual # models/laya/multilingual
make model MODEL_VARIANT=typed-decisions
```

The `english` checkpoint lives at the model-store root; the other variants live in a
subdirectory named after the variant, matching the laya.cpp layout.

## SQL reference

| Function | Returns | Description |
|---|---|---|
| `laya_version()` | TEXT | Extension version, e.g. `v0.0.1`. |
| `laya_load(dir)`, `laya_load(dir, options)` | TEXT | Loads a checkpoint and returns the backend name (`CPU`, `CUDA0`). Replaces the resident model only after the new one loads. |
| `laya_backend()` | TEXT or NULL | Backend of the resident model, NULL when none is loaded. |
| `laya_noul(state, instructions)`, `laya_noul(state, instructions, criteria)` | REAL | Probability that the statement holds. Optional criteria: `{"true": "...", "false": "..."}` descriptions. |
| `laya_choice(state, instructions, criteria)` | TEXT | The selected option. Criteria: a JSON array of names or a JSON object mapping names to descriptions. |
| `laya_score(state, instructions, criteria)` | REAL | Expected ordinal score. Criteria: a JSON array of level descriptions, scored 0 through n-1. |
| `laya(state, questions)` | TEXT (JSON) | The full answers object for a questions object, evaluated in one batch. |

`options` for `laya_load` is a JSON object:

| Key | Default | Meaning |
|---|---|---|
| `variant` | `english` | `english`, `multilingual` or `typed-decisions`; appended to `dir` when not `english`. |
| `cuda` | true when built with CUDA | Use the CUDA backend. |
| `tensor_core` | false | Compensated Tensor Core FP32 projections (CUDA). |
| `flash` | false | Fused FP32 attention (CUDA). |
| `bf16` | false | Native mixed BF16 (CUDA; implies `flash`). |

```sql
select laya_load('models/laya', json_object('variant', 'multilingual', 'tensor_core', 1, 'flash', 1));
```

`questions` for `laya()` uses the laya.cpp request schema: an object of question ids, each
with `type` (`choice`, `score` or `noul`), `instructions`, and `criteria`. The result is the
`answers` object from laya.cpp, including `confidence`, `probabilities` and `action`
metadata, and carries SQLite's JSON subtype so `json_extract` and `json_object` nest it
directly.

Argument rules:

- A NULL `state`, `instructions` or `criteria` yields NULL without running the model.
- `state` and `instructions` are passed as text. A value produced by SQLite's JSON
  functions (`json()`, `json_object()`, ...) is passed as structured JSON instead, so a row
  can be summarized as `laya_noul(json_object('subject', subject, 'body', body), ...)`.
- Invalid JSON, unknown question types and other request errors raise a SQL error carrying
  the laya.cpp message.

### Model resolution

The first inference call without a resident model loads one from the environment:
`LAYA_MODEL_DIR` (default `models/laya`, relative to the working directory) and
`LAYA_OPTIONS` (the same JSON as the `laya_load` options). If that directory does not exist
the call fails with `No Laya model loaded; call laya_load(dir) or set LAYA_MODEL_DIR`.

## PostgreSQL

```sh
sudo apt-get install postgresql-16 postgresql-server-dev-16
make postgres               # build_postgres/laya.so
sudo make postgres-install  # into the directories reported by pg_config
make test-postgres          # pg_regress on a temporary instance (not as root)
```

See [postgres/README.md](postgres/README.md) for the SQL reference, the `laya.model_dir`
and `laya.options` settings, and the per-backend memory model.

## Python

`make python` builds a wheel for the `sqlite_laya` package in `dist/debug/wheels`. See
[bindings/python/README.md](bindings/python/README.md); the wheel depends on the system ICU
libraries of the machine that built it.

## Tests

```sh
make test-loadable                           # model-independent tests
make model && make cli                       # checkpoint and laya-cli for parity checks
LAYA_MODEL_DIR=models/laya make test-loadable
```

With a checkpoint, the suite loads it, checks the smoke cases from laya.cpp, runs table
scans, and compares every public number against `laya-cli` output within 0.0001. Pass
`LAYA_OPTIONS='{"cuda": true, "tensor_core": true, "flash": true}'` and
`LAYA_CLI_FLAGS='--tensor-core-fp32 --flash-fp32'` to exercise the GPU path.

## Limitations

- One resident model per process; `laya_load` replaces it. Calls are serialized across
  connections and threads, as the laya runtime requires.
- Scalar functions run one forward pass per row. Use `laya()` to evaluate several
  questions about the same row in one batch. Cross-row batching is not available.
- Strict FP32 on the CPU is slow for a 28-layer encoder: about a second per question on a
  few cores. Use the CUDA build for table-scale workloads.
- When built with CUDA, load the model before other CUDA users in the same process; the
  runtime disables TF32 before initializing cuBLAS.
- The WebAssembly and Windows targets of the original extension template are not supported.
- PostgreSQL backends each load their own copy of the model; see [postgres/README.md](postgres/README.md).

## License

MIT, see [LICENSE](LICENSE). laya.cpp, ggml and the Laya checkpoints retain their own licenses.
