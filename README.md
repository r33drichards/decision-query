# sqlaya

[Laya](https://huggingface.co/convaiinnovations/laya) typed decisions as SQL functions,
for SQLite and PostgreSQL. Both extensions embed [laya.cpp](https://github.com/r33drichards/laya.cpp),
the native C++ inference runtime, so a table of text can be classified, scored or filtered
without leaving the database.

```sql
-- SQLite
.load ./dist/debug/laya
select id from tickets where laya_noul(body, 'Does the customer request a refund?') > 0.5;

-- PostgreSQL
CREATE EXTENSION laya;
SET laya.model_dir = '/srv/models/laya';
SELECT id FROM tickets WHERE laya_noul(body, 'Does the customer request a refund?') > 0.5;
```

Both modules provide `laya_load`, `laya_backend`, `laya_version`, `laya_noul`,
`laya_choice`, `laya_score` and `laya()`, which returns the full answers object as JSON.

## Layout

| Directory | Contents |
|---|---|
| [`engine/`](engine/) | Shared C++ engine: one resident checkpoint per process, serialized calls, lazy loading. `laya_engine.hpp` plus the `sqlaya-engine` CMake target that builds laya.cpp and ggml as static position-independent archives. |
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
git clone --recurse-submodules https://github.com/r33drichards/sqlaya.git
cd sqlaya
make loadable static        # SQLite: dist/debug/laya.so, libsqlite_laya.a, sqlaya.h
make postgres               # PostgreSQL: build/postgres/laya.so and laya.control
sudo make postgres-install  # into the directories reported by pg_config
```

The CUDA backend is enabled automatically when CMake finds a CUDA compiler. Force a
choice with `CMAKE_FLAGS='-DSQLAYA_CUDA=OFF' make loadable` or `-DSQLAYA_CUDA=ON`,
adding `-DCMAKE_CUDA_ARCHITECTURES=<arch>` for your GPU as described in the laya.cpp README.
The PostgreSQL module is configured automatically when `pg_config` and the server headers
are found; `-DSQLAYA_POSTGRES=OFF` skips it.

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

`laya_load(dir, options)` and the lazy-loading settings take a JSON object:

| Key | Default | Meaning |
|---|---|---|
| `variant` | `english` | `english`, `multilingual` or `typed-decisions`; appended to `dir` when not `english`. |
| `cuda` | true when built with CUDA | Use the CUDA backend. |
| `tensor_core` | false | Compensated Tensor Core FP32 projections (CUDA). |
| `flash` | false | Fused FP32 attention (CUDA). |
| `bf16` | false | Native mixed BF16 (CUDA; implies `flash`). |

When no model is resident, the first inference call loads one from `LAYA_MODEL_DIR` and
`LAYA_OPTIONS` in the environment (PostgreSQL consults its `laya.model_dir` and
`laya.options` settings first). If nothing is found the call fails with
`No Laya model loaded; call laya_load(dir) or set LAYA_MODEL_DIR`.

## Tests

```sh
make test-loadable                                     # SQLite, no checkpoint needed
LAYA_MODEL_DIR=models/laya make cli test-loadable      # SQLite with checkpoint and CLI parity
LAYA_MODEL_DIR=models/laya LAYA_OPTIONS='{"cuda": false}' make postgres
sudo make postgres-install && make test-postgres       # pg_regress, not as root
```

## Limitations

- Scalar functions run one forward pass per row. Use `laya()` to evaluate several
  questions about the same row in one batch. Cross-row batching is not available.
- Strict FP32 on the CPU is slow for a 28-layer encoder: about a second per question on a
  few cores. Use the CUDA build for table-scale workloads.
- When built with CUDA, load the model before other CUDA users in the same process; the
  runtime disables TF32 before initializing cuBLAS.
- SQLite keeps one model per process; PostgreSQL backends each load their own copy. See
  the module READMEs.

## License

MIT, see [LICENSE](LICENSE). laya.cpp, ggml and the Laya checkpoints retain their own licenses.
