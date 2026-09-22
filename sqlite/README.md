# laya for SQLite

The SQLite loadable module `laya` answers Laya typed decisions from SQL. It is built
from the shared engine in `engine/` and the vendored laya.cpp runtime.

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

## Build

From the repository root (see the [root README](../README.md) for prerequisites):

```sh
make loadable          # dist/debug/laya.so (or laya.dylib)
make static            # dist/debug/libsqlite_laya.a and sqlaya.h
make loadable-release  # optimized build in dist/release
```

The module links ICU (and the CUDA runtime, when enabled) dynamically; ggml and the laya
runtime are folded into the module and only `sqlite3_laya_init` is exported. The static
library holds only the extension code; embedding it also requires linking the
`sqlaya-engine` CMake target from `engine/`. This directory also configures on its own with
`cmake -S sqlite -B build-sqlite`.

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

`options` is the JSON object described in the [root README](../README.md#load-options):

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
- The WebAssembly and Windows targets of the original extension template are not supported.
