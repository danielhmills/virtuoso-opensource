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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql.h"
#include "sqlext.h"

#include "nanoarrow.h"

#include "virtuoso_adbc.h"

extern SQLRETURN SQL_API virtodbc__SQLAllocStmt    (SQLHDBC, SQLHSTMT *);
extern SQLRETURN SQL_API virtodbc__SQLFreeStmt     (SQLHSTMT, SQLUSMALLINT);
extern SQLRETURN SQL_API virtodbc__SQLNumResultCols (SQLHSTMT, SQLSMALLINT *);
extern SQLRETURN SQL_API virtodbc__SQLPrepare      (SQLHSTMT, SQLCHAR *,
                                                    SQLINTEGER);
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
                                          struct ArrowArrayStream *out,
                                          struct AdbcError *err);

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

    sr = SQLPrepare (hstmt, (SQLCHAR *) self->sql, SQL_NTS);
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

extern enum ArrowType virt_sql_to_arrow_type (int sql_type, int *fallback);

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
    if (!self->sql)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecuteQuery: no SQL set");
    if (!self->cn || !self->cn->hdbc)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecuteQuery: connection is closed");

    if (rows_affected) *rows_affected = -1;

    /* Bound-parameter path: prepared HSTMT + per-row execute, no
     * result-set support in this phase.                                */
    if (self->has_bound_batch || self->has_bound_stream) {
        if (out_stream)
            memset (out_stream, 0, sizeof (*out_stream));
        return execute_bound (self, rows_affected, err);
    }

    /* Prepared-without-binds path: SQLExecute on the persistent HSTMT. */
    if (self->prepared && self->hstmt) {
        sr = SQLExecute ((SQLHSTMT) self->hstmt);
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
                                 out_stream, err);
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

    sr = SQLExecDirect (hstmt, (SQLCHAR *) self->sql, SQL_NTS);
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
                             out_stream, err);
    if (rc != ADBC_STATUS_OK) {
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }
    /* The reader owns hstmt now. */
    return ADBC_STATUS_OK;
}
