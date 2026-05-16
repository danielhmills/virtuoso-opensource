## adbc-driver-virtuoso (Python wheel)

Minimal Python packaging for the Virtuoso ADBC driver.

### Building

```sh
# From the repo root, after make:
make python-wheel

# Or manually:
python -m build --wheel
```

The wheel bundles `libvirtadbc.{so,dylib}` and registers the
`adbc_driver_virtuoso.connect` entry point.

### Installing

```sh
pip install dist/adbc_driver_virtuoso-0.0.1.dev0-py3-none-any.whl
```

### Usage

```python
import adbc_driver_manager

with adbc_driver_manager.AdbcDatabase(driver="virtuoso") as db:
    db.set_options(uri="virtuoso://user:pass@host:port")
    with adbc_driver_manager.AdbcConnection(db) as cn:
        with adbc_driver_manager.AdbcStatement(cn) as st:
            st.set_sql_query("SELECT 1")
            stream, rows = st.execute_query()
            print(stream.read_all())
```

Or via the `adbc_driver_virtuoso` package directly:

```python
import adbc_driver_manager
import adbc_driver_virtuoso

adbc_driver_manager.register_driver(adbc_driver_virtuoso.connect)
```
