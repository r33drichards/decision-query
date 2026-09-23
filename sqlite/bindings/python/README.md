# The `sqlite-laya` Python package

`sqlite-laya` packages the `laya` SQLite extension for Python applications using the
builtin [`sqlite3`](https://docs.python.org/3/library/sqlite3.html) module.

```
pip install sqlite-laya
```

## Usage

The package exports two functions: `loadable_path()`, which returns the full path to the
loadable extension (without the platform suffix, which SQLite infers), and `load(conn)`,
which loads the extension into a [sqlite3 Connection](https://docs.python.org/3/library/sqlite3.html#connection-objects).

```python
import sqlite3
import decision_query

conn = sqlite3.connect(':memory:')
conn.enable_load_extension(True)
decision_query.load(conn)
conn.enable_load_extension(False)

conn.execute("select dq_load('models/laya')")
print(conn.execute("select noul('Please refund the duplicate charge.', 'Does the customer ask for a refund?')").fetchone()[0])
```

See the repository README for the SQL API and model setup.

## Compatibility

The wheel bundles the loadable module built on the packaging machine. The module links
the system ICU libraries dynamically and, when built with CUDA, the CUDA runtime, so a
wheel is only usable on a compatible system. It is not repaired with `auditwheel`. If the
package does not load on your platform, build the extension from source and load the
resulting module directly with `Connection.load_extension()`.
