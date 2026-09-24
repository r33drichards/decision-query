# decision-query for MySQL

The MySQL module exposes the same Laya typed decisions as the SQLite and PostgreSQL
modules, as [loadable functions](https://dev.mysql.com/doc/extending-mysql/8.0/en/adding-loadable-function.html)
built on the shared engine in `engine/decision_engine.hpp`. It needs MySQL 8.0 or later.

```sql
SELECT id, subject FROM tickets
 WHERE noul(body, 'Does the customer request a refund?') > 0.5;

SELECT id, choice(body, 'Which department should handle this?',
                  '["billing", "technical", "sales"]') AS department
  FROM tickets;

SELECT id, JSON_EXTRACT(
           decide(JSON_OBJECT('subject', subject, 'body', body),
                  '{"urgency": {"type": "score",
                                "instructions": "How urgent is the request?",
                                "criteria": ["not urgent", "soon", "immediate"]}}'),
           '$.urgency.score') AS urgency
  FROM tickets;
```

## Build and install

Requirements: the MySQL client development headers (for `udf_registration_types.h`)
plus the same toolchain as the other modules (C++20 compiler, CMake 3.24+, ICU,
nlohmann-json, optional CUDA). On Debian-like systems:

```sh
sudo apt-get install cmake ninja-build libicu-dev nlohmann-json3-dev libmysqlclient-dev
make mysql                  # build/mysql/decision_query.so
sudo make mysql-install     # copies it into the plugin_dir reported by mysql_config
mysql -u root < mysql/install.sql
```

`install.sql` runs `CREATE FUNCTION ... SONAME 'decision_query.so'` for each function;
the registrations live in `mysql.func` and survive restarts. `uninstall.sql` removes
them. If your server's `plugin_dir` differs from what `mysql_config --plugindir` reports
(check with `SELECT @@plugin_dir`), configure with `-DDQ_MYSQL_PLUGIN_DIR=<dir>` or copy
the file there yourself. CMake builds the module automatically when it finds the header;
`-DDQ_MYSQL=OFF` skips it.

## SQL reference

| Function | Returns | Description |
|---|---|---|
| `dq_version()` | string | Module version, e.g. `v0.0.1`. |
| `dq_load(directory_or_url [, options])` | string | Loads a checkpoint or HTTP endpoint and returns the backend name (`CPU`, `CUDA0`). Replaces the resident model only after the new one loads. |
| `dq_backend()` | string | Backend of the resident model, NULL when none is loaded. |
| `dq_last_error()` | string | The most recent per-row error on this connection (see below), NULL if none. |
| `noul(state, instructions [, criteria])` | real | Probability that the statement holds. Optional criteria: `{"true": "...", "false": "..."}`. |
| `choice(state, instructions, criteria)` | string | The selected option. Criteria: a JSON array of names or an object mapping names to descriptions. |
| `score(state, instructions, criteria)` | real | Expected ordinal score over the criteria levels, scored 0 through n-1. |
| `decide(state, questions)` | string | The full answers object for a questions object, evaluated in one batch, as JSON text. |

Criteria, questions and options are JSON text, so string literals and
`JSON_OBJECT()` / `JSON_ARRAY()` both work. A NULL argument yields NULL without
running the model.

MySQL passes JSON values to a loadable function as plain strings, so the module
cannot tell `JSON_OBJECT(...)` from text by type. A state that parses as a JSON
object is passed to the model as structured data; anything else is text.

Arguments are converted to utf8mb4 before they reach the model, and text results
are utf8mb4, so MySQL's JSON functions accept `decide()` directly. This
uses the server's `mysql_udf_metadata` service (MySQL 8.0.19+).

## Loading a model

`dq_load` works as in the other modules. Without it, the first inference call
loads from the server's `DQ_MODEL_DIR` and `DQ_OPTIONS` environment variables. With
systemd, set them with `systemctl edit mysql`:

```ini
[Service]
Environment=DQ_MODEL_DIR=/srv/models/laya
```

`DQ_OPTIONS` takes the same JSON load options as `dq_load`.

Unlike PostgreSQL, MySQL runs every connection as a thread of one process, so one
resident model serves every connection. Calls into it are serialized.

For an HTTP endpoint, give the server `DQ_API_KEY_FILE` (or `DQ_API_KEY`). The
`key_file` option is refused from SQL: loadable functions cannot be restricted by
privilege, so it would let any account make the server read a file of its choosing
and send the contents to an endpoint of its choosing. For the same reason, bear in
mind that any account can call `dq_load` with a URL, and the server's own key goes
to whichever endpoint it names.

## Errors

A loadable function can only return an error message while a statement is being
prepared. So everything that can be checked then is: the argument count, constant
JSON arguments, `dq_load` with constant arguments, and whether a model can be
loaded at all. These fail the statement with the usual message:

```
ERROR 1123 (HY000): Can't initialize function 'noul'; No decision backend loaded; call dq_load(dir_or_url) or set DQ_MODEL_DIR
```

A failure that only shows up per row, such as a criteria column that is not
valid JSON or an endpoint that stops answering, makes that row's value NULL, and
MySQL returns NULL for the rest of the statement without calling the function
again. The message goes to the server error log and to `dq_last_error()`.

## Tests

`make test-mysql` initializes a temporary data directory, starts a private
`mysqld` with `plugin_dir` set to the build tree, registers the functions and runs
the checks through the `mysql` client. Nothing is installed. With a checkpoint, the
model-backed checks run too:

```sh
make mysql test-mysql                         # no checkpoint needed
DQ_MODEL_DIR=models/laya make test-mysql      # adds the model-backed checks
```

On Ubuntu the packaged AppArmor profile confines `/usr/sbin/mysqld` to the system
paths; unload it for the test with
`sudo apparmor_parser -R /etc/apparmor.d/usr.sbin.mysqld`.

## Limitations

- Model memory lives outside MySQL's memory accounting and stays allocated until
  the server exits.
- Scalar functions run one forward pass per row; `decide()` batches several
  questions about the same row. Cross-row batching is not available.
- Strict FP32 on the CPU is slow for a 28-layer encoder. Use the CUDA build for
  table-scale workloads.
