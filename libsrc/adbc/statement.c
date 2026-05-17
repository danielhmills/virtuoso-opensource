/*
 *  statement.c
 *
 *  AdbcStatement lifecycle for the Virtuoso ADBC driver.
 *
 *  Phase 4 surface:
 *    StatementNew         allocate VirtAdbcStatement
 *    StatementRelease     close any in-flight reader; free state
 *    StatementSetSqlQuery store the SQL string (no Prepare yet)
 *    StatementSetOption   typed string option (notably
 *                         adbc.virtuoso.fetch.batch_rows)
 *    StatementExecuteQuery direct-execute (SQLAllocStmt + SQLExecDirect)
 *                         then wrap the HSTMT in an ArrowArrayStream.
 *                         Statements that produce no result set
 *                         (DDL, DML) return rows_affected from
 *                         SQLRowCount and out=NULL.
 *
 *  Bind / BindStream / Prepare / GetParameterSchema land in phase 5.
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the GNU GPL v2; see COPYING in the project root.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql.h"
#include "sqlext.h"

#include <nanoarrow/nanoarrow.h>

#include "virtuoso_adbc.h"

extern SQLRETURN SQL_API virtodbc__SQLAllocStmt    (SQLHDBC, SQLHSTMT *);
extern SQLRETURN SQL_API virtodbc__SQLFreeStmt     (SQLHSTMT, SQLUSMALLINT);
extern SQLRETURN SQL_API virtodbc__SQLNumResultCols (SQLHSTMT, SQLSMALLINT *);
extern SQLRETURN SQL_API virtodbc__SQLDescribeCol  (SQLHSTMT, SQLUSMALLINT,
                                                    SQLCHAR *, SQLSMALLINT,
                                                    SQLSMALLINT *, SQLSMALLINT *,
                                                    SQLULEN *, SQLSMALLINT *,
                                                    SQLSMALLINT *);
extern SQLRETURN SQL_API virtodbc__SQLPrepare      (SQLHSTMT, SQLCHAR *,
                                                    SQLINTEGER);
extern SQLRETURN SQL_API virtodbc__SQLCancel       (SQLHSTMT);
extern SQLRETURN SQL_API SQLExecute      (SQLHSTMT);
extern SQLRETURN SQL_API SQLNumParams    (SQLHSTMT, SQLSMALLINT *);
extern SQLRETURN SQL_API SQLDescribeParam (SQLHSTMT, SQLUSMALLINT,
                                           SQLSMALLINT *, SQLULEN *,
                                           SQLSMALLINT *, SQLSMALLINT *);

/* The public SQLExecDirect / SQLPrepare entries handle input-escape
 * preprocessing (NMAKE_INPUT_ESCAPED_NARROW) which the raw
 * virtodbc__ forms do not.                                           */
extern SQLRETURN SQL_API SQLExecDirect (SQLHSTMT, SQLCHAR *, SQLINTEGER);
extern SQLRETURN SQL_API SQLPrepare    (SQLHSTMT, SQLCHAR *, SQLINTEGER);
extern SQLRETURN SQL_API SQLRowCount   (SQLHSTMT, SQLLEN *);

/* From arrow_reader.c */
extern AdbcStatusCode virt_reader_create (VirtAdbcConnection *cn, void *hstmt,
                                          int64_t batch_rows,
                                          int sparql_dialect,
                                          struct ArrowArrayStream *out,
                                          struct AdbcError *err);

/* From type_map.c */
extern enum ArrowType virt_sql_to_arrow_type (int sql_type, int *is_fallback);
extern const char *virt_arrow_to_virtuoso_ddl (enum ArrowType atype);

#define ST_SELF(st, err)                                                 \
    VirtAdbcStatement *self;                                             \
    if (!(st) || !(st)->private_data)                                    \
        return virt_err_set ((err), ADBC_STATUS_INVALID_STATE, NULL, 0,  \
                             "statement handle is not initialised");    \
    self = (VirtAdbcStatement *) (st)->private_data

/* ------------------------------------------------------------------ */
/* New / Release                                                       */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_st_new (struct AdbcConnection *cn, struct AdbcStatement *st,
             struct AdbcError *err)
{
    VirtAdbcStatement *self;
    VirtAdbcConnection *vcn;

    if (!st)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "StatementNew: NULL statement");
    if (st->private_data)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "StatementNew called twice on the same handle");
    if (!cn || !cn->private_data)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "StatementNew: connection is not initialised");
    vcn = (VirtAdbcConnection *) cn->private_data;
    if (!vcn->connected)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "StatementNew: connection is not open");

    self = (VirtAdbcStatement *) calloc (1, sizeof (*self));
    if (!self)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    self->cn         = vcn;
    self->batch_rows = 0;       /* 0 -> reader uses its default */
    st->private_data = self;
    return ADBC_STATUS_OK;
}

static void
release_bound (VirtAdbcStatement *self)
{
    if (self->has_bound_batch) {
        if (self->bound_batch.release)
            self->bound_batch.release (&self->bound_batch);
        if (self->bound_schema.release)
            self->bound_schema.release (&self->bound_schema);
        self->has_bound_batch = 0;
    }
    if (self->has_bound_stream) {
        if (self->bound_stream.release)
            self->bound_stream.release (&self->bound_stream);
        self->has_bound_stream = 0;
    }
    if (self->writer) {
        virt_writer_destroy (self->writer);
        self->writer = NULL;
    }
}

AdbcStatusCode
virt_st_release (struct AdbcStatement *st, struct AdbcError *err)
{
    VirtAdbcStatement *self;

    if (!st)
        return ADBC_STATUS_OK;
    self = (VirtAdbcStatement *) st->private_data;
    if (!self)
        return ADBC_STATUS_OK;

    release_bound (self);
    if (self->hstmt) {
        virtodbc__SQLFreeStmt ((SQLHSTMT) self->hstmt, SQL_DROP);
        self->hstmt = NULL;
    }
    free (self->sql);
    free (self->ingest_target_table);
    free (self->ingest_target_catalog);
    free (self->ingest_target_db_schema);
    virt_opt_free_all (&self->opts);
    free (self);
    st->private_data = NULL;
    (void) err;
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* SetSqlQuery / SetOption                                             */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_st_set_sql_query (struct AdbcStatement *st, const char *query,
                       struct AdbcError *err)
{
    char *dup;
    ST_SELF (st, err);
    if (!query || !*query)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "SetSqlQuery: empty query");
    dup = strdup (query);
    if (!dup)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    free (self->sql);
    self->sql = dup;
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_st_set_option (struct AdbcStatement *st, const char *key,
                    const char *value, struct AdbcError *err)
{
    ST_SELF (st, err);
    if (!key)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "SetOption: NULL key");

    if (strcmp (key, "adbc.virtuoso.dialect") == 0) {
        /* Accept "sparql" or "sql" (default). The chosen value is
         * applied lazily at ExecuteQuery / Prepare time by prefixing
         * the statement text on the wire.                            */
        if (!value || strcmp (value, "sql") == 0)
            self->sparql_dialect = 0;
        else if (strcmp (value, "sparql") == 0)
            self->sparql_dialect = 1;
        else
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "%s: expected 'sparql' or 'sql', got '%s'",
                                 key, value);
        return ADBC_STATUS_OK;
    }
    if (strcmp (key, "adbc.virtuoso.fetch.batch_rows") == 0) {
        long n;
        char *end = NULL;
        if (!value)
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "%s: missing value", key);
        n = strtol (value, &end, 10);
        if (!end || *end != '\0' || n <= 0)
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "%s: expected positive integer, got '%s'",
                                 key, value);
        self->batch_rows = (int64_t) n;
        return ADBC_STATUS_OK;
    }

    /* Phase 7 bulk-ingest options. Setting target_table flips the
     * statement into ingest mode -- SetSqlQuery is then ignored and
     * the SQL is synthesised at ExecuteQuery time.                    */
    if (strcmp (key, ADBC_INGEST_OPTION_TARGET_TABLE) == 0) {
        char *dup = value ? strdup (value) : NULL;
        if (value && !dup)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "out of memory");
        free (self->ingest_target_table);
        self->ingest_target_table = dup;
        /* Default mode if not explicitly set yet. */
        if (self->ingest_mode == VIRT_INGEST_NONE)
            self->ingest_mode = VIRT_INGEST_CREATE;
        return ADBC_STATUS_OK;
    }
    if (strcmp (key, ADBC_INGEST_OPTION_TARGET_CATALOG) == 0) {
        char *dup = value ? strdup (value) : NULL;
        if (value && !dup)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "out of memory");
        free (self->ingest_target_catalog);
        self->ingest_target_catalog = dup;
        return ADBC_STATUS_OK;
    }
    if (strcmp (key, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA) == 0) {
        char *dup = value ? strdup (value) : NULL;
        if (value && !dup)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "out of memory");
        free (self->ingest_target_db_schema);
        self->ingest_target_db_schema = dup;
        return ADBC_STATUS_OK;
    }
    if (strcmp (key, ADBC_INGEST_OPTION_MODE) == 0) {
        if (!value)
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "%s: missing value", key);
        if (strcmp (value, ADBC_INGEST_OPTION_MODE_CREATE) == 0)
            self->ingest_mode = VIRT_INGEST_CREATE;
        else if (strcmp (value, ADBC_INGEST_OPTION_MODE_APPEND) == 0)
            self->ingest_mode = VIRT_INGEST_APPEND;
        else if (strcmp (value, ADBC_INGEST_OPTION_MODE_REPLACE) == 0)
            self->ingest_mode = VIRT_INGEST_REPLACE;
        else if (strcmp (value, ADBC_INGEST_OPTION_MODE_CREATE_APPEND) == 0)
            self->ingest_mode = VIRT_INGEST_CREATE_APPEND;
        else
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "%s: unknown mode '%s'", key, value);
        return ADBC_STATUS_OK;
    }
    if (strcmp (key, ADBC_INGEST_OPTION_TEMPORARY) == 0) {
        self->ingest_temporary =
            (value && strcmp (value, ADBC_OPTION_VALUE_ENABLED) == 0) ? 1 : 0;
        return ADBC_STATUS_OK;
    }

    /* Unknown keys are stored on the option map so GetOption can read
     * them back; phases 5-9 may grow the live-effect list.            */
    return virt_opt_set_string (&self->opts, key, value, err);
}

AdbcStatusCode
virt_st_set_option_int (struct AdbcStatement *st, const char *key,
                        int64_t value, struct AdbcError *err)
{
    ST_SELF (st, err);
    if (key && strcmp (key, "adbc.virtuoso.fetch.batch_rows") == 0) {
        if (value <= 0)
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "%s: must be positive", key);
        self->batch_rows = value;
        return ADBC_STATUS_OK;
    }
    return virt_opt_set_int (&self->opts, key, value, err);
}

AdbcStatusCode
virt_st_set_option_bytes (struct AdbcStatement *st, const char *key,
                          const uint8_t *value, size_t len,
                          struct AdbcError *err)
{
    ST_SELF (st, err);
    return virt_opt_set_bytes (&self->opts, key, value, len, err);
}

AdbcStatusCode
virt_st_set_option_double (struct AdbcStatement *st, const char *key,
                           double value, struct AdbcError *err)
{
    ST_SELF (st, err);
    return virt_opt_set_double (&self->opts, key, value, err);
}

AdbcStatusCode
virt_st_get_option (struct AdbcStatement *st, const char *key,
                    char *out, size_t *len, struct AdbcError *err)
{
    ST_SELF (st, err);
    if (key && strcmp (key, "adbc.virtuoso.fetch.batch_rows") == 0) {
        char tmp[32];
        size_t need;
        int64_t v = self->batch_rows ? self->batch_rows : 4096;
        snprintf (tmp, sizeof (tmp), "%lld", (long long) v);
        need = strlen (tmp) + 1;
        if (!len)
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "GetOption: NULL len");
        if (out && *len > 0) {
            size_t cp = need <= *len ? need : *len;
            memcpy (out, tmp, cp - 1);
            out[cp - 1] = '\0';
        }
        *len = need;
        return ADBC_STATUS_OK;
    }
    return virt_opt_get_string (self->opts, key, out, len, err);
}

AdbcStatusCode
virt_st_get_option_bytes (struct AdbcStatement *st, const char *key,
                          uint8_t *out, size_t *len, struct AdbcError *err)
{
    ST_SELF (st, err);
    return virt_opt_get_bytes (self->opts, key, out, len, err);
}

AdbcStatusCode
virt_st_get_option_int (struct AdbcStatement *st, const char *key,
                        int64_t *out, struct AdbcError *err)
{
    ST_SELF (st, err);
    if (key && strcmp (key, "adbc.virtuoso.fetch.batch_rows") == 0) {
        if (!out)
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "GetOptionInt: NULL out");
        *out = self->batch_rows ? self->batch_rows : 4096;
        return ADBC_STATUS_OK;
    }
    return virt_opt_get_int (self->opts, key, out, err);
}

AdbcStatusCode
virt_st_get_option_double (struct AdbcStatement *st, const char *key,
                           double *out, struct AdbcError *err)
{
    ST_SELF (st, err);
    return virt_opt_get_double (self->opts, key, out, err);
}

/* ====================================================================
 *  Phase 8 -- Dialect (SPARQL passthrough)
 *
 *  Virtuoso's "Type 4" SQL parser treats a leading bare-word "sparql"
 *  as a switch into the SPARQL grammar for the rest of the statement
 *  (the same convention isql, JDBC and the existing ODBC tooling
 *  use). When the user has set adbc.virtuoso.dialect=sparql we
 *  prepend that token before SQLPrepare / SQLExecDirect.
 *
 *  The output schema's metadata is annotated as virtuoso:dialect=sparql
 *  with each column carrying virtuoso:rdf=true, so downstream tools
 *  can recognise that string cells encode IRIs / blanks / literals.
 *  No further interpretation happens in the driver: typed literals
 *  and lang tags pass through as Virtuoso-formatted strings (e.g.
 *  "value"@en or "value"^^<http://...#int>). DV_IRI_ID, DV_RDF and
 *  DV_GEOMETRY columns are not distinguishable through the ODBC
 *  descriptor (Virtuoso maps them all to SQL_VARCHAR in
 *  dv_to_sql_type) -- exposing them as distinct Arrow types will
 *  require a server-side helper and is deferred.
 * ==================================================================== */

/* Returns a newly malloc'd "sparql <sql>" string when the dialect is
 * SPARQL, else NULL. Caller frees on success; if the return is NULL,
 * use the original self->sql verbatim.                              */
static char *
sql_with_dialect_prefix (const VirtAdbcStatement *self)
{
    const char *prefix = "sparql ";
    size_t plen, slen;
    char *out;

    if (!self->sparql_dialect || !self->sql)
        return NULL;
    plen = strlen (prefix);
    slen = strlen (self->sql);
    out = (char *) malloc (plen + slen + 1);
    if (!out) return NULL;
    memcpy (out, prefix, plen);
    memcpy (out + plen, self->sql, slen + 1);
    return out;
}

/* Phase 9d: register an HSTMT as the currently-executing one on both
 * the statement and the parent connection (under the connection mutex),
 * so that StatementCancel (called from another thread) can find it.  */
static void
st_register_pending (VirtAdbcStatement *self, SQLHSTMT hstmt)
{
    pthread_mutex_t *m = (pthread_mutex_t *) self->cn->mu;
    if (!m || !self->cn) return;
    pthread_mutex_lock (m);
    self->pending_hstmt        = hstmt;
    self->cn->current_hstmt    = hstmt;
    pthread_mutex_unlock (m);
}

static void
st_clear_pending (VirtAdbcStatement *self)
{
    pthread_mutex_t *m = self->cn ? (pthread_mutex_t *) self->cn->mu : NULL;
    if (!m) { self->pending_hstmt = NULL; return; }
    pthread_mutex_lock (m);
    if (self->cn->current_hstmt == self->pending_hstmt)
        self->cn->current_hstmt = NULL;
    self->pending_hstmt = NULL;
    pthread_mutex_unlock (m);
}

/* ====================================================================
 *  Phase 9d — StatementCancel
 *
 *  Thread-safe cancel of the statement's currently-executing HSTMT.
 *  Reads the pending_hstmt pointer under the connection mutex; if
 *  nothing is executing (no HSTMT registered) we return OK silently
 *  per ADBC convention — the next API call on the statement will
 *  return normally.
 * ==================================================================== */

AdbcStatusCode
virt_st_cancel (struct AdbcStatement *st, struct AdbcError *err)
{
    pthread_mutex_t *m;
    SQLHSTMT victim = SQL_NULL_HSTMT;
    ST_SELF (st, err);

    if (!self->cn || !self->cn->hdbc)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "StatementCancel: connection is closed");

    m = (pthread_mutex_t *) self->cn->mu;
    pthread_mutex_lock (m);
    victim = (SQLHSTMT) self->cn->current_hstmt;
    pthread_mutex_unlock (m);

    if (victim == SQL_NULL_HSTMT)
        return ADBC_STATUS_OK;      /* nothing to cancel; not an error */

    {
        SQLRETURN sr = virtodbc__SQLCancel (victim);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
            return virt_err_from_odbc (ADBC_STATUS_UNKNOWN, NULL, self->cn->hdbc,
                                       victim, err);
    }
    return ADBC_STATUS_OK;
}

/* ====================================================================
 *  Phase 9a — StatementExecuteSchema
 *
 *  Prepare (if needed), then use SQLNumResultCols + SQLDescribeCol to
 *  build the result ArrowSchema WITHOUT executing the query. This lets
 *  consumers discover the output shape before fetching rows.
 * ==================================================================== */

AdbcStatusCode
virt_st_execute_schema (struct AdbcStatement *st, struct ArrowSchema *out,
                        struct AdbcError *err)
{
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    SQLSMALLINT ncols = 0;
    SQLRETURN sr;
    AdbcStatusCode rc;
    int i;

    ST_SELF (st, err);
    if (!self->sql)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecuteSchema: no SQL set");
    if (!self->cn || !self->cn->hdbc)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecuteSchema: connection closed");
    if (!out)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "ExecuteSchema: NULL out");

    /* Prepare if not already done. */
    if (!self->prepared || !self->hstmt) {
        rc = virt_st_prepare (st, err);
        if (rc != ADBC_STATUS_OK)
            return rc;
    }
    hstmt = (SQLHSTMT) self->hstmt;

    sr = virtodbc__SQLNumResultCols (hstmt, &ncols);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL, hstmt, err);

    ArrowSchemaInit (out);
    if (ncols > 0) {
        if (ArrowSchemaSetTypeStruct (out, ncols) != 0)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "ArrowSchemaSetTypeStruct failed");
    }

    for (i = 0; i < ncols; i++) {
        SQLCHAR colname[256];
        SQLSMALLINT cbname = 0, sqltype = 0, scale = 0, nullable = 0;
        SQLULEN precision = 0;
        enum ArrowType atype;
        int fallback = 0;
        struct ArrowSchema *child = out->children[i];

        colname[0] = '\0';
        sr = virtodbc__SQLDescribeCol (hstmt, (SQLUSMALLINT) (i + 1),
                                       colname, (SQLSMALLINT) sizeof (colname),
                                       &cbname, &sqltype, &precision, &scale,
                                       &nullable);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
            return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL, hstmt,
                                       err);

        atype = virt_sql_to_arrow_type ((int) sqltype, &fallback);

        if (atype == NANOARROW_TYPE_TIMESTAMP) {
            if (ArrowSchemaSetTypeDateTime (child, NANOARROW_TYPE_TIMESTAMP,
                                           NANOARROW_TIME_UNIT_MICRO, NULL) != 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "SetTypeDateTime[ts] failed");
        } else if (atype == NANOARROW_TYPE_TIME64) {
            if (ArrowSchemaSetTypeDateTime (child, NANOARROW_TYPE_TIME64,
                                           NANOARROW_TIME_UNIT_MICRO, NULL) != 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "SetTypeDateTime[time] failed");
        } else if (atype == NANOARROW_TYPE_DATE32) {
            if (ArrowSchemaSetType (child, NANOARROW_TYPE_DATE32) != 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "SetType(date32) failed");
        } else {
            if (ArrowSchemaSetType (child, atype) != 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "SetType(%d) failed", (int) atype);
        }

        if (ArrowSchemaSetName (child,
                              (const char *) (colname[0] ? colname
                                                         : (SQLCHAR *) "")) != 0)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "SetName failed");
        (void) fallback;
    }

    return ADBC_STATUS_OK;
}

/* ====================================================================
 *  Phase 7 -- Bulk ingestion
 *
 *  When adbc.ingest.target_table is set, ExecuteQuery ignores any SQL
 *  the caller set via SetSqlQuery. Instead it derives:
 *
 *    - the qualified table name from target_catalog / target_db_schema
 *      / target_table
 *    - a CREATE TABLE DDL from the Arrow schema of the bound data
 *      (mapped through virt_arrow_to_virtuoso_ddl)
 *    - an INSERT statement parameterised on the same column list
 *
 *  Mode semantics:
 *    CREATE          -- always CREATE, error if table exists.
 *    APPEND          -- skip CREATE, INSERT into existing table.
 *    REPLACE         -- DROP TABLE IF EXISTS, then CREATE + INSERT.
 *    CREATE_APPEND   -- CREATE TABLE IF NOT EXISTS, then INSERT.
 *
 *  The INSERT loop reuses the phase-5 writer (Prepare -> bind ->
 *  per-row SQLExecute). rows_affected reports the total inserted.
 * ==================================================================== */

extern AdbcStatusCode virt_writer_create (void *hstmt,
                                          struct ArrowSchema *bind_schema,
                                          VirtAdbcWriter **out_writer,
                                          struct AdbcError *err);
extern void           virt_writer_destroy (VirtAdbcWriter *w);
extern AdbcStatusCode virt_writer_execute_batch (VirtAdbcWriter *w,
                                                 struct ArrowArray *batch,
                                                 int64_t *rows_affected_inout,
                                                 struct AdbcError *err);

/* Quote a Virtuoso identifier into out. Caller frees. Returns NULL
 * on OOM. Embedded double-quotes are doubled.                         */
static char *
quote_ident (const char *id)
{
    size_t n, out_cap, off = 0;
    char *out;
    if (!id) return NULL;
    n = strlen (id);
    out_cap = n * 2 + 3;        /* "..." plus doubled quotes */
    out = (char *) malloc (out_cap);
    if (!out) return NULL;
    out[off++] = '"';
    while (*id) {
        if (*id == '"') out[off++] = '"';
        out[off++] = *id++;
    }
    out[off++] = '"';
    out[off]   = '\0';
    return out;
}

/* Build a qualified ".cat"."sch"."tab" or fallback. Caller frees.    */
static char *
build_qualified_table_name (VirtAdbcStatement *self)
{
    char *qc = self->ingest_target_catalog
               ? quote_ident (self->ingest_target_catalog) : NULL;
    char *qs = self->ingest_target_db_schema
               ? quote_ident (self->ingest_target_db_schema) : NULL;
    char *qt = quote_ident (self->ingest_target_table);
    size_t need;
    char *out;

    if (!qt) { free (qc); free (qs); return NULL; }
    need = (qc ? strlen (qc) + 1 : 0)
         + (qs ? strlen (qs) + 1 : 0)
         + strlen (qt) + 1;
    out = (char *) malloc (need);
    if (!out) { free (qc); free (qs); free (qt); return NULL; }
    out[0] = '\0';
    if (qc) { strcat (out, qc); strcat (out, "."); }
    if (qs) { strcat (out, qs); strcat (out, "."); }
    strcat (out, qt);
    free (qc); free (qs); free (qt);
    return out;
}

/* Build "CREATE TABLE <qname> (col1 TYPE, col2 TYPE, ...)" from a
 * bind schema (top-level struct). Caller frees. err may be NULL.     */
static char *
build_create_ddl (const char *qname, const struct ArrowSchema *schema,
                  int if_not_exists, int temporary,
                  struct AdbcError *err)
{
    struct ArrowError ae;
    int64_t i;
    /* dynamic growable buffer */
    size_t cap = 256, off = 0;
    char  *buf = (char *) malloc (cap);
    if (!buf) {
        virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0, "out of memory");
        return NULL;
    }
    /* Virtuoso doesn't accept "IF NOT EXISTS" in CREATE TABLE; the
     * CREATE_APPEND path uses the silent-error flag instead.         */
    (void) if_not_exists;
    off += (size_t) snprintf (buf + off, cap - off,
        "CREATE %sTABLE %s (",
        temporary ? "TEMPORARY " : "",
        qname);

    for (i = 0; i < schema->n_children; i++) {
        struct ArrowSchema *child = schema->children[i];
        struct ArrowSchemaView sv;
        const char *ddl;
        char *qcol;
        size_t add;

        memset (&ae, 0, sizeof (ae));
        if (ArrowSchemaViewInit (&sv, child, &ae) != 0) {
            virt_err_set (err, ADBC_STATUS_INVALID_DATA, NULL, 0,
                          "ingest: cannot parse column %lld type: %s",
                          (long long) i, ae.message[0] ? ae.message : "");
            free (buf);
            return NULL;
        }
        ddl = virt_arrow_to_virtuoso_ddl (sv.type);
        if (!ddl) {
            virt_err_set (err, ADBC_STATUS_NOT_IMPLEMENTED, NULL, 0,
                          "ingest: column %lld has unsupported Arrow type %d",
                          (long long) i, (int) sv.type);
            free (buf);
            return NULL;
        }
        qcol = quote_ident (child->name && child->name[0]
                            ? child->name
                            : "col");
        if (!qcol) { free (buf); return NULL; }

        add = strlen (qcol) + 1 + strlen (ddl) + 3;     /* "x" TYPE, */
        if (off + add + 2 >= cap) {
            size_t nc = cap * 2;
            char *nb;
            while (nc < off + add + 2) nc *= 2;
            nb = realloc (buf, nc);
            if (!nb) { free (qcol); free (buf); return NULL; }
            buf = nb; cap = nc;
        }
        if (i > 0) { buf[off++] = ','; buf[off++] = ' '; }
        memcpy (buf + off, qcol, strlen (qcol));  off += strlen (qcol);
        buf[off++] = ' ';
        memcpy (buf + off, ddl, strlen (ddl));    off += strlen (ddl);
        free (qcol);
    }
    if (off + 2 >= cap) {
        cap += 4;
        buf = realloc (buf, cap);
        if (!buf) return NULL;
    }
    buf[off++] = ')';
    buf[off]   = '\0';
    return buf;
}

/* Build "INSERT INTO <qname> ("c1","c2") VALUES (?,?)" from a bind
 * schema.                                                            */
static char *
build_insert_sql (const char *qname, const struct ArrowSchema *schema)
{
    size_t cap = 256, off = 0;
    char  *buf = (char *) malloc (cap);
    int64_t i;
    if (!buf) return NULL;
    off += (size_t) snprintf (buf + off, cap - off, "INSERT INTO %s (", qname);

    for (i = 0; i < schema->n_children; i++) {
        char *qcol = quote_ident (schema->children[i]->name
                                  && schema->children[i]->name[0]
                                  ? schema->children[i]->name : "col");
        size_t add;
        if (!qcol) { free (buf); return NULL; }
        add = strlen (qcol) + 2;
        if (off + add + 32 >= cap) {
            size_t nc = cap * 2;
            char *nb;
            while (nc < off + add + 32) nc *= 2;
            nb = realloc (buf, nc); if (!nb) { free (qcol); free (buf); return NULL; }
            buf = nb; cap = nc;
        }
        if (i > 0) { buf[off++] = ','; buf[off++] = ' '; }
        memcpy (buf + off, qcol, strlen (qcol)); off += strlen (qcol);
        free (qcol);
    }
    if (off + 16 >= cap) {
        cap += 32;
        buf = realloc (buf, cap);
        if (!buf) return NULL;
    }
    off += (size_t) snprintf (buf + off, cap - off, ") VALUES (");
    for (i = 0; i < schema->n_children; i++) {
        if (off + 4 >= cap) {
            cap *= 2;
            buf = realloc (buf, cap);
            if (!buf) return NULL;
        }
        if (i > 0) { buf[off++] = ','; buf[off++] = ' '; }
        buf[off++] = '?';
    }
    buf[off++] = ')';
    buf[off]   = '\0';
    return buf;
}

/* Run a DDL statement on a fresh transient HSTMT. is_silent=1 lets a
 * "does not exist" error pass (for DROP TABLE IF EXISTS).            */
static AdbcStatusCode
exec_ddl (VirtAdbcConnection *cn, const char *sql, int is_silent,
          struct AdbcError *err)
{
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    SQLRETURN sr;
    AdbcStatusCode rc = ADBC_STATUS_OK;

    sr = virtodbc__SQLAllocStmt ((SQLHDBC) cn->hdbc, &hstmt);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, cn->hdbc, NULL,
                                   err);
    sr = SQLExecDirect (hstmt, (SQLCHAR *) sql, SQL_NTS);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        if (!is_silent)
            rc = virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL, hstmt,
                                     err);
    }
    virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
    return rc;
}

/* The bound-data schema: either bound_schema (single batch) or
 * peeled off the bound stream via get_schema. *owned is set to 1 if
 * the caller must release the returned schema, 0 if the schema is
 * borrowed from the statement. */
static AdbcStatusCode
get_bound_schema (VirtAdbcStatement *self, struct ArrowSchema **out,
                  int *owned, struct ArrowSchema *out_storage,
                  struct AdbcError *err)
{
    *owned = 0;
    if (self->has_bound_batch) {
        *out = &self->bound_schema;
        return ADBC_STATUS_OK;
    }
    if (self->has_bound_stream) {
        memset (out_storage, 0, sizeof (*out_storage));
        if (self->bound_stream.get_schema (&self->bound_stream,
                                           out_storage) != 0)
            return virt_err_set (err, ADBC_STATUS_IO, NULL, 0,
                                 "ingest: bound_stream.get_schema failed");
        *out   = out_storage;
        *owned = 1;
        return ADBC_STATUS_OK;
    }
    return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                         "ingest: no bound data");
}

/* Top-level ingest entry. Called from virt_st_execute_query when
 * ingest_target_table is set.                                        */
static AdbcStatusCode
execute_ingest (VirtAdbcStatement *self, int64_t *rows_affected,
                struct AdbcError *err)
{
    struct ArrowSchema  stream_schema_storage;
    struct ArrowSchema *bind_schema = NULL;
    int                 owned       = 0;
    char *qname = NULL, *ddl = NULL, *insert_sql = NULL;
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    SQLRETURN sr;
    AdbcStatusCode rc;
    int64_t affected = 0;

    if (!self->cn || !self->cn->hdbc)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ingest: connection closed");
    if (!self->ingest_target_table || !*self->ingest_target_table)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ingest: target_table is empty");
    if (!self->has_bound_batch && !self->has_bound_stream)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ingest: nothing bound (call Bind or BindStream)");

    rc = get_bound_schema (self, &bind_schema, &owned,
                           &stream_schema_storage, err);
    if (rc != ADBC_STATUS_OK)
        return rc;

    qname = build_qualified_table_name (self);
    if (!qname) { rc = ADBC_STATUS_INTERNAL; goto cleanup; }

    /* ----- Phase 7b/c: DDL ----- */
    switch (self->ingest_mode) {
    case VIRT_INGEST_REPLACE: {
        size_t need = strlen ("DROP TABLE ") + strlen (qname) + 1;
        char *drop = (char *) malloc (need);
        if (!drop) { rc = ADBC_STATUS_INTERNAL; goto cleanup; }
        snprintf (drop, need, "DROP TABLE %s", qname);
        /* silent: Virtuoso doesn't ship a portable IF EXISTS for DROP */
        (void) exec_ddl (self->cn, drop, /*silent*/ 1, NULL);
        free (drop);
    } /* fallthrough */
    case VIRT_INGEST_CREATE: {
        ddl = build_create_ddl (qname, bind_schema, /*if_not_exists*/ 0,
                                self->ingest_temporary, err);
        if (!ddl) { rc = ADBC_STATUS_INTERNAL; goto cleanup; }
        rc = exec_ddl (self->cn, ddl, /*silent*/ 0, err);
        if (rc != ADBC_STATUS_OK) goto cleanup;
        break;
    }
    case VIRT_INGEST_CREATE_APPEND:
        ddl = build_create_ddl (qname, bind_schema, /*if_not_exists*/ 1,
                                self->ingest_temporary, err);
        if (!ddl) { rc = ADBC_STATUS_INTERNAL; goto cleanup; }
        /* CREATE TABLE IF NOT EXISTS is silent when the table is there. */
        (void) exec_ddl (self->cn, ddl, /*silent*/ 1, NULL);
        break;
    case VIRT_INGEST_APPEND:
        /* no DDL */
        break;
    default:
        rc = virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                           "ingest: unknown mode");
        goto cleanup;
    }

    /* ----- INSERT loop ----- */
    insert_sql = build_insert_sql (qname, bind_schema);
    if (!insert_sql) { rc = ADBC_STATUS_INTERNAL; goto cleanup; }

    sr = virtodbc__SQLAllocStmt ((SQLHDBC) self->cn->hdbc, &hstmt);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        rc = virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, self->cn->hdbc,
                                 NULL, err);
        goto cleanup;
    }
    sr = SQLPrepare (hstmt, (SQLCHAR *) insert_sql, SQL_NTS);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        rc = virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL, hstmt, err);
        goto cleanup;
    }

    /* Build a fresh writer scoped to this ingest HSTMT. */
    {
        VirtAdbcWriter *w = NULL;
        rc = virt_writer_create (hstmt, bind_schema, &w, err);
        if (rc != ADBC_STATUS_OK) goto cleanup;

        if (self->has_bound_batch) {
            rc = virt_writer_execute_batch (w, &self->bound_batch, &affected,
                                            err);
        } else {
            /* Pull batches from the bound stream. */
            for (;;) {
                struct ArrowArray batch;
                memset (&batch, 0, sizeof (batch));
                if (self->bound_stream.get_next (&self->bound_stream,
                                                 &batch) != 0) {
                    rc = virt_err_set (err, ADBC_STATUS_IO, NULL, 0,
                                       "ingest: stream get_next failed");
                    break;
                }
                if (!batch.release) break;          /* EOF */
                rc = virt_writer_execute_batch (w, &batch, &affected, err);
                batch.release (&batch);
                if (rc != ADBC_STATUS_OK) break;
            }
        }
        virt_writer_destroy (w);
        if (rc != ADBC_STATUS_OK) goto cleanup;
    }

    if (rows_affected) *rows_affected = affected;

cleanup:
    if (hstmt)            virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
    free (insert_sql);
    free (ddl);
    free (qname);
    /* Consume bound data so the next ExecuteQuery doesn't replay. */
    if (self->has_bound_batch) {
        if (self->bound_batch.release)
            self->bound_batch.release (&self->bound_batch);
        if (self->bound_schema.release)
            self->bound_schema.release (&self->bound_schema);
        memset (&self->bound_batch,  0, sizeof (self->bound_batch));
        memset (&self->bound_schema, 0, sizeof (self->bound_schema));
        self->has_bound_batch = 0;
    }
    if (self->has_bound_stream) {
        if (self->bound_stream.release)
            self->bound_stream.release (&self->bound_stream);
        memset (&self->bound_stream, 0, sizeof (self->bound_stream));
        self->has_bound_stream = 0;
    }
    if (owned && stream_schema_storage.release)
        stream_schema_storage.release (&stream_schema_storage);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Prepare                                                             */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_st_prepare (struct AdbcStatement *st, struct AdbcError *err)
{
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    SQLRETURN sr;
    ST_SELF (st, err);

    if (!self->sql)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "Prepare: no SQL set");
    if (!self->cn || !self->cn->hdbc)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "Prepare: connection closed");
    if (self->prepared)
        return ADBC_STATUS_OK;       /* idempotent */

    sr = virtodbc__SQLAllocStmt ((SQLHDBC) self->cn->hdbc, &hstmt);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, self->cn->hdbc,
                                   NULL, err);

    {
        char *prefixed = sql_with_dialect_prefix (self);
        const char *q = prefixed ? prefixed : self->sql;
        st_register_pending (self, hstmt);
        sr = SQLPrepare (hstmt, (SQLCHAR *) q, SQL_NTS);
        st_clear_pending (self);
        free (prefixed);
    }
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        AdbcStatusCode rc =
            virt_err_from_odbc (ADBC_STATUS_INVALID_ARGUMENT, NULL, NULL,
                                hstmt, err);
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }
    self->hstmt    = hstmt;
    self->prepared = 1;
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* GetParameterSchema                                                  */
/*                                                                    */
/* Derived from SQLNumParams + SQLDescribeParam on the prepared       */
/* statement. Virtuoso's DescribeParam may return SQL_UNKNOWN_TYPE    */
/* for ambiguous parameters; we surface those as utf8 so callers can */
/* still bind something.                                              */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_st_get_parameter_schema (struct AdbcStatement *st,
                              struct ArrowSchema *out,
                              struct AdbcError *err)
{
    SQLSMALLINT nparams = 0;
    SQLRETURN sr;
    int i;
    ST_SELF (st, err);

    if (!self->prepared || !self->hstmt)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "GetParameterSchema: statement not prepared");
    if (!out)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "GetParameterSchema: NULL out");

    sr = SQLNumParams ((SQLHSTMT) self->hstmt, &nparams);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL,
                                   self->hstmt, err);

    ArrowSchemaInit (out);
    if (ArrowSchemaSetTypeStruct (out, nparams) != 0)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "ArrowSchemaSetTypeStruct failed");

    for (i = 0; i < nparams; i++) {
        SQLSMALLINT sql_type = 0, scale = 0, nullable = 0;
        SQLULEN col_size = 0;
        enum ArrowType atype;
        int fallback = 0;
        char  name[32];

        sr = SQLDescribeParam ((SQLHSTMT) self->hstmt,
                               (SQLUSMALLINT) (i + 1),
                               &sql_type, &col_size, &scale, &nullable);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
            sql_type = SQL_VARCHAR;  /* tolerate unknowns */

        atype = virt_sql_to_arrow_type ((int) sql_type, &fallback);
        if (ArrowSchemaSetType (out->children[i], atype) != 0)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "SetType for param %d failed", i);
        snprintf (name, sizeof (name), "p%d", i + 1);
        if (ArrowSchemaSetName (out->children[i], name) != 0)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "SetName for param %d failed", i);
    }
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Bind / BindStream                                                   */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_st_bind (struct AdbcStatement *st, struct ArrowArray *values,
              struct ArrowSchema *schema, struct AdbcError *err)
{
    ST_SELF (st, err);
    if (!values || !schema)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "Bind: NULL array/schema");

    release_bound (self);
    /* Move ownership of the caller's array+schema onto the statement. */
    memcpy (&self->bound_batch, values, sizeof (*values));
    memcpy (&self->bound_schema, schema, sizeof (*schema));
    memset (values, 0, sizeof (*values));
    memset (schema, 0, sizeof (*schema));
    self->has_bound_batch = 1;
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_st_bind_stream (struct AdbcStatement *st,
                     struct ArrowArrayStream *stream, struct AdbcError *err)
{
    ST_SELF (st, err);
    if (!stream)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "BindStream: NULL stream");

    release_bound (self);
    memcpy (&self->bound_stream, stream, sizeof (*stream));
    memset (stream, 0, sizeof (*stream));
    self->has_bound_stream = 1;
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* ExecuteQuery                                                        */
/* ------------------------------------------------------------------ */

static AdbcStatusCode
execute_bound (VirtAdbcStatement *self, int64_t *rows_affected,
               struct AdbcError *err)
{
    AdbcStatusCode rc;
    int64_t affected = 0;

    /* Bound parameters require an explicitly prepared HSTMT. */
    if (!self->prepared || !self->hstmt)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecuteQuery: bound parameters require Prepare first");

    /* Build the writer lazily once we know the bind schema. */
    if (!self->writer) {
        struct ArrowSchema *bs = self->has_bound_batch
                                 ? &self->bound_schema : NULL;
        struct ArrowSchema  stream_schema;
        memset (&stream_schema, 0, sizeof (stream_schema));
        if (!bs && self->has_bound_stream) {
            if (self->bound_stream.get_schema (&self->bound_stream,
                                               &stream_schema) != 0)
                return virt_err_set (err, ADBC_STATUS_IO, NULL, 0,
                                     "BindStream: get_schema failed: %s",
                                     self->bound_stream.get_last_error
                                     ? self->bound_stream.get_last_error (
                                           &self->bound_stream)
                                     : "(no msg)");
            bs = &stream_schema;
        }
        if (!bs)
            return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                                 "ExecuteQuery: bound data has no schema");
        rc = virt_writer_create (self->hstmt, bs, &self->writer, err);
        if (stream_schema.release)
            stream_schema.release (&stream_schema);
        if (rc != ADBC_STATUS_OK)
            return rc;
    }

    /* Single bound batch: execute and consume. */
    if (self->has_bound_batch) {
        rc = virt_writer_execute_batch (self->writer, &self->bound_batch,
                                        &affected, err);
        /* Consume so the next ExecuteQuery doesn't replay. */
        if (self->bound_batch.release)
            self->bound_batch.release (&self->bound_batch);
        memset (&self->bound_batch, 0, sizeof (self->bound_batch));
        if (self->bound_schema.release)
            self->bound_schema.release (&self->bound_schema);
        memset (&self->bound_schema, 0, sizeof (self->bound_schema));
        self->has_bound_batch = 0;
        if (rc != ADBC_STATUS_OK)
            return rc;
        if (rows_affected) *rows_affected = affected;
        return ADBC_STATUS_OK;
    }

    /* Bound stream: pull batches one at a time. */
    if (self->has_bound_stream) {
        for (;;) {
            struct ArrowArray batch;
            memset (&batch, 0, sizeof (batch));
            if (self->bound_stream.get_next (&self->bound_stream, &batch) != 0) {
                if (self->bound_stream.release)
                    self->bound_stream.release (&self->bound_stream);
                self->has_bound_stream = 0;
                return virt_err_set (err, ADBC_STATUS_IO, NULL, 0,
                                     "BindStream: get_next failed: %s",
                                     self->bound_stream.get_last_error
                                     ? self->bound_stream.get_last_error (
                                           &self->bound_stream)
                                     : "(no msg)");
            }
            if (!batch.release)
                break;            /* end of stream */
            rc = virt_writer_execute_batch (self->writer, &batch,
                                            &affected, err);
            batch.release (&batch);
            if (rc != ADBC_STATUS_OK) {
                if (self->bound_stream.release)
                    self->bound_stream.release (&self->bound_stream);
                self->has_bound_stream = 0;
                return rc;
            }
        }
        if (self->bound_stream.release)
            self->bound_stream.release (&self->bound_stream);
        self->has_bound_stream = 0;
        if (rows_affected) *rows_affected = affected;
        return ADBC_STATUS_OK;
    }

    /* Unreachable -- caller of execute_bound checked has_bound_*. */
    return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                         "execute_bound called with no bound data");
}

AdbcStatusCode
virt_st_execute_query (struct AdbcStatement *st,
                       struct ArrowArrayStream *out_stream,
                       int64_t *rows_affected, struct AdbcError *err)
{
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    SQLRETURN sr;
    SQLSMALLINT ncols = 0;
    AdbcStatusCode rc;

    ST_SELF (st, err);
    if (!self->cn || !self->cn->hdbc)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecuteQuery: connection is closed");

    if (rows_affected) *rows_affected = -1;

    /* Phase-7 bulk ingest takes precedence over SetSqlQuery and over
     * the phase-5 bound-parameter path.                               */
    if (self->ingest_mode != VIRT_INGEST_NONE
        && self->ingest_target_table) {
        if (out_stream)
            memset (out_stream, 0, sizeof (*out_stream));
        return execute_ingest (self, rows_affected, err);
    }

    if (!self->sql)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecuteQuery: no SQL set");

    /* Bound-parameter path: prepared HSTMT + per-row execute, no
     * result-set support in this phase.                                */
    if (self->has_bound_batch || self->has_bound_stream) {
        if (out_stream)
            memset (out_stream, 0, sizeof (*out_stream));
        return execute_bound (self, rows_affected, err);
    }

    /* Prepared-without-binds path: SQLExecute on the persistent HSTMT. */
    if (self->prepared && self->hstmt) {
        st_register_pending (self, (SQLHSTMT) self->hstmt);
        sr = SQLExecute ((SQLHSTMT) self->hstmt);
        st_clear_pending (self);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
            return virt_err_from_odbc (ADBC_STATUS_IO, NULL, NULL,
                                       self->hstmt, err);
        sr = virtodbc__SQLNumResultCols ((SQLHSTMT) self->hstmt, &ncols);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
            return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL,
                                       self->hstmt, err);
        if (ncols == 0) {
            SQLLEN n = -1;
            SQLRowCount ((SQLHSTMT) self->hstmt, &n);
            if (rows_affected) *rows_affected = (int64_t) n;
            if (out_stream)
                memset (out_stream, 0, sizeof (*out_stream));
            return ADBC_STATUS_OK;
        }
        if (!out_stream)
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "ExecuteQuery: result set requires out_stream");
        /* The reader takes ownership of the HSTMT; that ends the
         * prepared state.                                              */
        rc = virt_reader_create (self->cn, self->hstmt, self->batch_rows,
                                 self->sparql_dialect, out_stream, err);
        if (rc != ADBC_STATUS_OK)
            return rc;
        self->hstmt    = NULL;
        self->prepared = 0;
        return ADBC_STATUS_OK;
    }

    /* Phase-4 path: transient HSTMT + SQLExecDirect.                   */
    sr = virtodbc__SQLAllocStmt ((SQLHDBC) self->cn->hdbc, &hstmt);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, self->cn->hdbc,
                                   NULL, err);

    {
        char *prefixed = sql_with_dialect_prefix (self);
        const char *q = prefixed ? prefixed : self->sql;
        st_register_pending (self, hstmt);
        sr = SQLExecDirect (hstmt, (SQLCHAR *) q, SQL_NTS);
        st_clear_pending (self);
        free (prefixed);
    }
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        rc = virt_err_from_odbc (ADBC_STATUS_IO, NULL, NULL, hstmt, err);
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }

    /* Statements with no result columns are DDL/DML/CALL. We report
     * rows_affected (if requested) and don't produce a stream.        */
    sr = virtodbc__SQLNumResultCols (hstmt, &ncols);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        rc = virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL, hstmt, err);
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }

    if (ncols == 0) {
        SQLLEN n = -1;
        SQLRowCount (hstmt, &n);
        if (rows_affected) *rows_affected = (int64_t) n;
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        if (out_stream)
            memset (out_stream, 0, sizeof (*out_stream));
        return ADBC_STATUS_OK;
    }

    /* Result set path: hand the HSTMT to a reader. Caller must want
     * the rows.                                                        */
    if (!out_stream) {
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "ExecuteQuery: result set requires out_stream");
    }

    rc = virt_reader_create (self->cn, hstmt, self->batch_rows,
                             self->sparql_dialect, out_stream, err);
    if (rc != ADBC_STATUS_OK) {
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }
    /* The reader owns hstmt now. */
    return ADBC_STATUS_OK;
}

/* ====================================================================
 *  Phase 9c — StatementExecutePartitions
 *
 *  Serialise the statement's SQL (and dialect flag) into a self-
 *  contained partition descriptor that can be re-executed elsewhere
 *  via ConnectionReadPartition. Single-partition: one descriptor
 *  containing the full SQL text.
 *
 *  Format: see VIRT_PARTITION_HDR_LEN / VIRT_PARTITION_MAGIC in
 *  virtuoso_adbc.h.
 * ==================================================================== */

static void
partitions_release (struct AdbcPartitions *p)
{
    size_t i;
    if (!p) return;
    if (p->partitions) {
        for (i = 0; i < p->num_partitions; i++)
            free ((void *) p->partitions[i]);
        free (p->partitions);
        p->partitions = NULL;
    }
    if (p->partition_lengths) {
        free ((void *) p->partition_lengths);
        p->partition_lengths = NULL;
    }
    p->num_partitions = 0;
    p->private_data   = NULL;
    p->release        = NULL;
}

AdbcStatusCode
virt_st_execute_partitions (struct AdbcStatement *st,
                            struct ArrowSchema *schema,
                            struct AdbcPartitions *partitions,
                            int64_t *rows_affected,
                            struct AdbcError *err)
{
    AdbcStatusCode rc;
    size_t sql_len, blob_len;
    uint8_t *blob;
    uint16_t flags;
    ST_SELF (st, err);

    if (!self->sql)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecutePartitions: no SQL set");
    if (!self->cn || !self->cn->hdbc)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecutePartitions: connection closed");

    /* Get the result schema via ExecuteSchema. */
    if (schema) {
        rc = virt_st_execute_schema (st, schema, err);
        if (rc != ADBC_STATUS_OK)
            return rc;
    }

    if (rows_affected)
        *rows_affected = -1;

    if (!partitions)
        return ADBC_STATUS_OK;      /* caller only wanted schema */

    /* Build a single partition descriptor. */
    sql_len = strlen (self->sql);
    blob_len = VIRT_PARTITION_HDR_LEN + sql_len;
    blob = (uint8_t *) malloc (blob_len);
    if (!blob)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");

    /* Header. */
    memcpy (blob, VIRT_PARTITION_MAGIC, 4);
    blob[4] = (uint8_t) (VIRT_PARTITION_VERSION & 0xFF);
    blob[5] = (uint8_t) ((VIRT_PARTITION_VERSION >> 8) & 0xFF);
    flags   = (uint16_t) (self->sparql_dialect ? 0x01 : 0x00);
    blob[6] = (uint8_t) (flags & 0xFF);
    blob[7] = (uint8_t) ((flags >> 8) & 0xFF);
    blob[8]  = (uint8_t) (sql_len & 0xFF);
    blob[9]  = (uint8_t) ((sql_len >> 8) & 0xFF);
    blob[10] = (uint8_t) ((sql_len >> 16) & 0xFF);
    blob[11] = (uint8_t) ((sql_len >> 24) & 0xFF);
    memcpy (blob + VIRT_PARTITION_HDR_LEN, self->sql, sql_len);

    /* Populate AdbcPartitions. */
    memset (partitions, 0, sizeof (*partitions));
    partitions->num_partitions = 1;
    partitions->partitions = (const uint8_t **) malloc (sizeof (uint8_t *));
    if (!partitions->partitions) {
        free (blob);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    }
    partitions->partition_lengths = (const size_t *) malloc (sizeof (size_t));
    if (!partitions->partition_lengths) {
        free ((void *) partitions->partitions);
        partitions->partitions = NULL;
        free (blob);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    }
    partitions->partitions[0]       = blob;
    ((size_t *) partitions->partition_lengths)[0] = blob_len;
    partitions->release             = partitions_release;
    partitions->private_data        = NULL;

    return ADBC_STATUS_OK;
}
