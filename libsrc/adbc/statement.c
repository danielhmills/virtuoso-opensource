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

/* The public SQLExecDirect entry handles input-escape preprocessing
 * (NMAKE_INPUT_ESCAPED_NARROW) which the raw virtodbc__SQLExecDirect
 * does not.                                                          */
extern SQLRETURN SQL_API SQLExecDirect (SQLHSTMT, SQLCHAR *, SQLINTEGER);
extern SQLRETURN SQL_API SQLRowCount (SQLHSTMT, SQLLEN *);

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

AdbcStatusCode
virt_st_release (struct AdbcStatement *st, struct AdbcError *err)
{
    VirtAdbcStatement *self;

    if (!st)
        return ADBC_STATUS_OK;
    self = (VirtAdbcStatement *) st->private_data;
    if (!self)
        return ADBC_STATUS_OK;

    /* If there's an unconsumed reader still owning an HSTMT, release it. */
    if (self->hstmt) {
        virtodbc__SQLFreeStmt ((SQLHSTMT) self->hstmt, SQL_DROP);
        self->hstmt = NULL;
    }
    free (self->sql);
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
/* ExecuteQuery                                                        */
/* ------------------------------------------------------------------ */

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
