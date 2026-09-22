# laya for PostgreSQL

The `laya` PostgreSQL extension exposes the same Laya typed decisions as the SQLite
module in this repository, built from the [pg_extension](https://github.com/mkindahl/pg_extension)
CMake template and the shared engine in `engine/laya_engine.hpp`.

```sql
CREATE EXTENSION laya;
SET laya.model_dir = '/srv/models/laya';

SELECT id, subject FROM tickets
 WHERE laya_noul(body, 'Does the customer request a refund?') > 0.5;

SELECT id, laya_choice(body, 'Which department should handle this?',
                       '["billing", "technical", "sales"]') AS department
  FROM tickets;

SELECT id, laya(jsonb_build_object('subject', subject, 'body', body),
                '{"urgency": {"type": "score",
                              "instructions": "How urgent is the request?",
                              "criteria": ["not urgent", "soon", "immediate"]}}')
             -> 'urgency' ->> 'score' AS urgency
  FROM tickets;
```

## Build and install

Requirements: PostgreSQL 13 or later with its server development files, plus the
same toolchain as the SQLite module (C++20 compiler, CMake 3.24+, ICU, nlohmann-json,
optional CUDA). On Debian-like systems:

```sh
sudo apt-get install cmake ninja-build libicu-dev nlohmann-json3-dev \
  postgresql-16 postgresql-server-dev-16
make postgres               # build/postgres/laya.so and laya.control
sudo make postgres-install  # copies into the directories reported by pg_config
```

Set `PGPATH` (or `PostgreSQL_ROOT`) to select another PostgreSQL installation, and
`CMAKE_FLAGS='-DSQLAYA_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=<arch>'` to force the CUDA
backend. The module exports only the PostgreSQL entry points; ggml and the laya runtime
are folded in, and ICU (plus the CUDA runtime, when enabled) is linked dynamically.

## SQL reference

| Function | Returns | Description |
|---|---|---|
| `laya_version()` | text | Extension version, e.g. `v0.0.1`. |
| `laya_load(directory text)`, `laya_load(directory text, options jsonb)` | text | Loads a checkpoint into this backend and returns the backend name (`CPU`, `CUDA0`). Replaces the resident model only after the new one loads. |
| `laya_backend()` | text | Backend of this backend's resident model, NULL when none is loaded. |
| `laya_noul(state, instructions text)`, `laya_noul(state, instructions text, criteria jsonb)` | float8 | Probability that the statement holds. Optional criteria: `{"true": "...", "false": "..."}`. |
| `laya_choice(state, instructions text, criteria jsonb)` | text | The selected option. Criteria: a JSON array of names or an object mapping names to descriptions. |
| `laya_score(state, instructions text, criteria jsonb)` | float8 | Expected ordinal score over the criteria levels, scored 0 through n-1. |
| `laya(state, questions jsonb)` | jsonb | The full answers object for a questions object, evaluated in one batch. |

`state` is `text` or `jsonb`. A jsonb state is passed to the model as structured data,
so `jsonb_build_object('subject', subject, 'body', body)` summarizes a row. All
inference functions are strict: a NULL argument yields NULL without running the model.
They are declared `STABLE`, `PARALLEL RESTRICTED` and with a high cost so the planner
evaluates cheaper predicates first and does not spread model loading across parallel
workers. `laya_load` is `VOLATILE` and `PARALLEL UNSAFE`.

`options` is a JSON object with the keys `variant` (`english`, `multilingual`,
`typed-decisions`), `cuda`, `tensor_core`, `flash` and `bf16`; see the repository README.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `laya.model_dir` | `''` | Checkpoint directory loaded by the first inference call in a backend. Falls back to the `LAYA_MODEL_DIR` environment variable of the server, then `models/laya` relative to the data directory. |
| `laya.options` | `''` | JSON load options for that lazy load. Falls back to `LAYA_OPTIONS`. |

Both are user-settable, so they work in `postgresql.conf`, `ALTER DATABASE ... SET`,
`ALTER ROLE ... SET` or a plain `SET`. When no model is resident and the directory does
not exist, inference fails with
`No Laya model loaded; call laya_load(dir) or set LAYA_MODEL_DIR`.

## Tests

`make test-postgres` runs the `basic` regression test on a temporary instance through
pg_regress. It needs the extension installed and must run as a non-superuser OS account
(pg_regress refuses root). With a checkpoint, configure the model-backed test too:

```sh
LAYA_MODEL_DIR=models/laya LAYA_OPTIONS='{"cuda": false}' make postgres
sudo make postgres-install
make test-postgres
```

The model test keeps numeric results inside assertions so its expected output is the
same on every backend. Update expected files after intentional changes with
`cmake --build build --target laya_update_results`.

## Limitations

- PostgreSQL runs one process per connection, and each backend that calls an inference
  function loads its own copy of the checkpoint (0.6 to 0.9 GB of RAM, or GPU memory).
  Use a connection pooler to bound the number of resident copies, and prefer long-lived
  connections.
- Model memory lives outside PostgreSQL's memory contexts and stays allocated until the
  backend exits.
- Scalar functions run one forward pass per row; `laya()` batches several questions
  about the same row. Cross-row batching is not available.
- Strict FP32 on the CPU is slow for a 28-layer encoder. Use the CUDA build for
  table-scale workloads, and load the model before any other CUDA user in the backend.
