# ADBC Driver for Virtuoso

An [Arrow Database Connectivity (ADBC)](https://arrow.apache.org/adbc/) driver
for OpenLink Virtuoso. It loads as a peer of the upstream
`adbc_driver_postgresql` / `adbc_driver_sqlite` drivers via the
`adbc_driver_manager` Python package or the `arrow-adbc` C driver manager.

**Status.** The driver implements the full ADBC 1.1.0 surface:
database/connection/statement lifecycles, read-only queries, prepared
statements + parameter binding, bulk ingestion, catalog/metadata,
SPARQL passthrough, typed options, GetStatistics, ExecuteSchema,
ExecutePartitions, and StatementCancel.

## Quick start

```sh
# Build the driver
make -C libsrc/adbc
make -C libsrc/adbc check

# The shared library is at .libs/libvirtadbc.{so,dylib}

# Install (optional)
make install
# places libvirtadbc.so in $(libdir) and adbc-driver-virtuoso.pc in
# $(libdir)/pkgconfig/
```

### Python (via adbc_driver_manager)

```sh
# Build the wheel
make python-wheel
pip install python/dist/adbc_driver_virtuoso-*.whl
```

```python
import adbc_driver_manager

with adbc_driver_manager.AdbcDatabase(driver="virtuoso") as db:
    db.set_options(uri="virtuoso://user:pass@host:port")
    with adbc_driver_manager.AdbcConnection(db) as cn:
        with adbc_driver_manager.AdbcStatement(cn) as st:
            st.set_sql_query("SELECT 1, 'hello', 3.14")
            stream, rows = st.execute_query()
            print(stream.read_all().to_pandas())
```

## Layout

```
./
├── adbc_entry.c                 -- AdbcDriverInit dispatch table
├── database.c / connection.c    -- AdbcDatabase + AdbcConnection
├── statement.c                  -- AdbcStatement (query, prepare, bind, ingest)
├── arrow_reader.c               -- Virtuoso row buffer -> ArrowArrayStream
├── arrow_writer.c               -- ArrowArray -> Virtuoso bind block / bulk insert
├── type_map.c                   -- DV_* / SQL_* <-> ArrowSchema format strings
├── catalog.c                    -- GetInfo / GetObjects / GetTableSchema / GetStatistics
├── error.c                      -- AdbcError construction / SQLSTATE mapping
├── options.c                    -- typed Get/SetOption variants
├── virtuo_adbc.h                -- private driver header
├── tests/
│   ├── smoke_test.c             -- dispatch table validation
│   ├── unit_test.c              -- driver unit + integration tests
│   └── validation/              -- upstream arrow-adbc validation harness
├── python/                      -- Python wheel
│   ├── pyproject.toml
│   └── adbc_driver_virtuoso/__init__.py
├── adbc-driver-virtuoso.pc.in   -- pkg-config template
└── adbc-driver-virtuoso.pc      -- generated pkg-config file
```

## Supported ADBC features

| Feature                          | Status      |
| -------------------------------- | ----------- |
| AdbcDatabase lifecycle           | Done        |
| AdbcConnection lifecycle         | Done        |
| Autocommit toggle                | Done        |
| Commit / Rollback                | Done        |
| SELECT → ArrowArrayStream        | Done        |
| Type mapping (SQL → Arrow)       | Done        |
| Batching (configurable row size) | Done        |
| Prepared statements              | Done        |
| Bind single batch                | Done        |
| BindStream                       | Done        |
| GetTableTypes                    | Done        |
| GetInfo                          | Done        |
| GetTableSchema                   | Done        |
| GetObjects (depth-aware)         | Done        |
| Bulk ingest (CREATE/APPEND/...)  | Done        |
| SPARQL passthrough (`sparql `)   | Done        |
| StatementCancel                  | Done        |
| StatementExecuteSchema           | Done        |
| GetStatistics / StatisticNames   | Done        |
| ExecutePartitions / ReadPartition| Done        |
| Substrait (SetSubstraitPlan)     | Not impl.   |

## Virtuoso-specific options

| Option key                              | Type   | Scope     | Effect |
| --------------------------------------- | ------ | --------- | ------ |
| `uri`                                   | string | database  | Connection URI: `virtuoso://user:pass@host:port` or `host:port` |
| `username` / `password`                 | string | database  | Credentials (alternative to uri) |
| `adbc.virtuoso.charset`                 | string | database  | Server character set (default UTF-8) |
| `adbc.virtuoso.log_enable`              | int    | database  | Enable CLI/RPC logging (0/1, default 0) |
| `adbc.connection.autocommit`            | string | connection| `true` (default) / `false` |
| `adbc.connection.read_only`             | string | connection| `true` / `false` (default false) |
| `adbc.virtuoso.dialect`                 | string | statement | `sql` (default) or `sparql` |
| `adbc.virtuoso.fetch.batch_rows`        | int    | statement | Rows per ArrowArrayStream batch (default 4096) |
| `adbc.ingest.target_table`              | string | statement | Bulk ingest target (enables ingest mode) |
| `adbc.ingest.mode`                      | string | statement | `create` / `append` / `replace` / `create_append` |
| `adbc.ingest.target_db_schema`          | string | statement | Schema qualifier for ingest target |
| `adbc.ingest.temporary`                 | string | statement | Create a temporary table (`true`/`false`) |

### SPARQL dialect

When `adbc.virtuoso.dialect=sparql`, ExecuteQuery and Prepare prepend the
bareword `sparql ` to the statement text before sending it on the wire —
the same convention isql, JDBC and the legacy ODBC tooling use.

Result `ArrowSchema` metadata:
- Top-level: `virtuoso:dialect=sparql`
- Each column: `virtuoso:rdf=true`

Column values come back as ordinary `utf8` (all Virtuoso RDF types surface
as `SQL_VARCHAR` through the ODBC layer). String encodings follow
Virtuoso's standard textual conventions: bare IRIs, `_:bN` blanks,
`"val"^^<type>` typed literals, `"val"@lang` language-tagged literals.

## Type mapping

| Virtuoso SQL type                  | Arrow type      | Notes |
| ---------------------------------- | --------------- | ----- |
| `BOOLEAN` / `BIT`                  | `bool`          |       |
| `TINYINT`                          | `int8`          |       |
| `SMALLINT`                         | `int16`         |       |
| `INTEGER`                          | `int32`         |       |
| `BIGINT`                           | `int64`         |       |
| `REAL`                             | `float32`       |       |
| `FLOAT` / `DOUBLE`                 | `float64`       |       |
| `NUMERIC` / `DECIMAL`              | `utf8`          | precision/scale preserved as string |
| `CHAR` / `VARCHAR`                 | `utf8`          |       |
| `LONG VARCHAR`                     | `large_utf8`    |       |
| `BINARY` / `VARBINARY`             | `binary`        |       |
| `LONG VARBINARY`                   | `large_binary`  |       |
| `DATE`                             | `date32[days]`  |       |
| `TIME`                             | `time64[us]`    |       |
| `TIMESTAMP`                        | `timestamp[us]` |       |
| `IRI_ID` (DV_IRI_ID)               | `utf8`          | + metadata `virtuoso:type=iri_id` |
| `DV_RDF`                           | `utf8`          | formatted string per SPARQL section |
| `GEOMETRY` (DV_GEO)                | `binary`        | WKB, + metadata `virtuoso:type=geo` |

## Known issues

1. **Unsigned integer ingest**: Unsigned integer types (UINT8-UINT64) are
   not yet mapped in `virt_arrow_to_virtuoso_ddl()`. Ingest of these types
   via bulk insert synthesizes DDL that will fail. Workaround: use signed
   integer types or pre-create the table with compatible columns.
2. **RDF typed literal projection**: DV_IRI_ID, DV_RDF, and DV_GEOMETRY
   columns all surface as `SQL_VARCHAR` through the ODBC descriptor layer.
   Exposing them as distinct Arrow types (struct for RDF literals, WKB
   for geometry) requires a server-side helper and is deferred.
3. **Concurrent statements**: ADBC connections are documented as
   "serialised access". Creating multiple AdbcStatement handles on one
   connection and interleaving their execution is not supported.
4. **DDL implicit commit**: Virtuoso DDL statements implicitly commit any
   open transaction. This is standard Virtuoso behaviour but differs from
   some other ADBC drivers.
