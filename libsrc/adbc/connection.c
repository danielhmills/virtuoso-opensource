/*
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  This project is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; only version 2 of the License, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 *  connection.c
 *
 *  AdbcConnection lifecycle for the Virtuoso ADBC driver.
 *
 *  Phase 3 surface:
 *    ConnectionNew         allocate VirtAdbcConnection
 *    ConnectionSetOption*  store typed option
 *    ConnectionInit        open the Virtuoso CLI connection
 *    ConnectionRelease     disconnect + free
 *    ConnectionCommit      virtodbc__SQLTransact SQL_COMMIT
 *    ConnectionRollback    virtodbc__SQLTransact SQL_ROLLBACK
 *    ConnectionCancel      cancel any currently-executing statement
 *
 *  Per-connection mutex protects the current_hstmt pointer so that
 *  Cancel can run concurrently from another thread without racing
 *  on the connection handle.
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

#include "virtuoso_adbc.h"

/*
 *  libwic exports two flavours of each ODBC entrypoint:
 *    - virtodbc__SQLFoo (raw, no handle-type assertion)
 *    - SQLFoo          (public, asserts handle types)
 *  We use whichever exists; SQLDisconnect / SQLSetConnectOption only have
 *  the public form, others are available raw.
 */
extern SQLRETURN SQL_API virtodbc__SQLAllocConnect (SQLHENV, SQLHDBC *);
extern SQLRETURN SQL_API virtodbc__SQLFreeConnect  (SQLHDBC);
extern SQLRETURN SQL_API virtodbc__SQLTransact     (SQLHENV, SQLHDBC,
                                                    SQLUSMALLINT);
extern SQLRETURN SQL_API virtodbc__SQLCancel       (SQLHSTMT);

extern SQLRETURN SQL_API SQLDisconnect      (SQLHDBC);
extern SQLRETURN SQL_API SQLDriverConnect   (SQLHDBC, SQLHWND, SQLCHAR *,
                                             SQLSMALLINT, SQLCHAR *,
                                             SQLSMALLINT, SQLSMALLINT *,
                                             SQLUSMALLINT);
extern SQLRETURN SQL_API SQLSetConnectOption (SQLHDBC, SQLUSMALLINT,
                                              SQLULEN);

/* From statement.c / arrow_reader.c — used by ReadPartition. */
extern SQLRETURN SQL_API virtodbc__SQLAllocStmt    (SQLHDBC, SQLHSTMT *);
extern SQLRETURN SQL_API virtodbc__SQLFreeStmt     (SQLHSTMT, SQLUSMALLINT);
extern SQLRETURN SQL_API virtodbc__SQLNumResultCols (SQLHSTMT, SQLSMALLINT *);
extern SQLRETURN SQL_API SQLExecDirect (SQLHSTMT, SQLCHAR *, SQLINTEGER);
extern AdbcStatusCode virt_reader_create (VirtAdbcConnection *cn, void *hstmt,
                                          int64_t batch_rows,
                                          int sparql_dialect,
                                          struct ArrowArrayStream *out,
                                          struct AdbcError *err);

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static pthread_mutex_t *
mu_new (void)
{
    pthread_mutex_t *m = (pthread_mutex_t *) malloc (sizeof (*m));
    if (!m)
        return NULL;
    if (pthread_mutex_init (m, NULL) != 0) {
        free (m);
        return NULL;
    }
    return m;
}

static void
mu_free (pthread_mutex_t *m)
{
    if (!m)
        return;
    pthread_mutex_destroy (m);
    free (m);
}

#define CN_SELF(cn, err)                                                 \
    VirtAdbcConnection *self;                                            \
    if (!(cn) || !(cn)->private_data)                                    \
        return virt_err_set ((err), ADBC_STATUS_INVALID_STATE, NULL, 0,  \
                             "connection handle is not initialised");    \
    self = (VirtAdbcConnection *) (cn)->private_data

/* ------------------------------------------------------------------ */
/* New / Release                                                       */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_cn_new (struct AdbcConnection *cn, struct AdbcError *err)
{
    VirtAdbcConnection *self;

    if (!cn)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "ConnectionNew: NULL connection");
    if (cn->private_data)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ConnectionNew called twice on the same handle");

    self = (VirtAdbcConnection *) calloc (1, sizeof (*self));
    if (!self)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    self->autocommit = 1;             /* ADBC default */
    self->mu = mu_new ();
    if (!self->mu) {
        free (self);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "could not initialise connection mutex");
    }
    cn->private_data = self;
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_cn_release (struct AdbcConnection *cn, struct AdbcError *err)
{
    VirtAdbcConnection *self;

    if (!cn)
        return ADBC_STATUS_OK;
    self = (VirtAdbcConnection *) cn->private_data;
    if (!self)
        return ADBC_STATUS_OK;

    if (self->hdbc) {
        if (self->connected)
            SQLDisconnect ((SQLHDBC) self->hdbc);
        virtodbc__SQLFreeConnect ((SQLHDBC) self->hdbc);
        self->hdbc = NULL;
    }
    virt_opt_free_all (&self->opts);
    mu_free ((pthread_mutex_t *) self->mu);
    free (self);
    cn->private_data = NULL;
    (void) err;
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* SetOption / GetOption                                              */
/*                                                                    */
/* A subset of keys take effect immediately on the live connection.   */
/* Other keys are stored and consulted at Init time (Init reads its   */
/* config from the option map, so pre-Init keys "just work").          */
/* ------------------------------------------------------------------ */

static AdbcStatusCode
apply_string_opt_live (VirtAdbcConnection *self, const char *key,
                       const char *value, struct AdbcError *err)
{
    if (strcmp (key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
        if (!value)
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "autocommit: missing value");
        if (strcmp (value, ADBC_OPTION_VALUE_ENABLED) == 0) {
            if (!self->autocommit && self->in_txn)
                /* spec: switching back into autocommit commits open txn */
                virtodbc__SQLTransact (SQL_NULL_HENV, (SQLHDBC) self->hdbc,
                                       SQL_COMMIT);
            self->autocommit = 1;
            self->in_txn = 0;
        } else if (strcmp (value, ADBC_OPTION_VALUE_DISABLED) == 0) {
            self->autocommit = 0;
        } else {
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "autocommit: expected 'true' or 'false'");
        }
        return ADBC_STATUS_OK;
    }
    if (strcmp (key, ADBC_CONNECTION_OPTION_READ_ONLY) == 0) {
        if (!value)
            return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                                 "readonly: missing value");
        self->read_only =
            (strcmp (value, ADBC_OPTION_VALUE_ENABLED) == 0) ? 1 : 0;
        return ADBC_STATUS_OK;
    }
    /* Not a live-effect key; just store it. */
    return virt_opt_set_string (&self->opts, key, value, err);
}

AdbcStatusCode
virt_cn_set_option (struct AdbcConnection *cn, const char *key,
                    const char *value, struct AdbcError *err)
{
    CN_SELF (cn, err);
    if (self->connected)
        return apply_string_opt_live (self, key, value, err);
    return virt_opt_set_string (&self->opts, key, value, err);
}

AdbcStatusCode
virt_cn_set_option_bytes (struct AdbcConnection *cn, const char *key,
                          const uint8_t *value, size_t len,
                          struct AdbcError *err)
{
    CN_SELF (cn, err);
    return virt_opt_set_bytes (&self->opts, key, value, len, err);
}

AdbcStatusCode
virt_cn_set_option_int (struct AdbcConnection *cn, const char *key,
                        int64_t value, struct AdbcError *err)
{
    CN_SELF (cn, err);
    return virt_opt_set_int (&self->opts, key, value, err);
}

AdbcStatusCode
virt_cn_set_option_double (struct AdbcConnection *cn, const char *key,
                           double value, struct AdbcError *err)
{
    CN_SELF (cn, err);
    return virt_opt_set_double (&self->opts, key, value, err);
}

AdbcStatusCode
virt_cn_get_option (struct AdbcConnection *cn, const char *key,
                    char *out, size_t *len, struct AdbcError *err)
{
    CN_SELF (cn, err);

    /* Synthesise values for live-effect knobs. */
    if (strcmp (key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
        const char *v = self->autocommit ? ADBC_OPTION_VALUE_ENABLED
                                         : ADBC_OPTION_VALUE_DISABLED;
        size_t need = strlen (v) + 1;
        if (out && *len > 0) {
            size_t cp = need <= *len ? need : *len;
            memcpy (out, v, cp - 1);
            out[cp - 1] = '\0';
        }
        *len = need;
        return ADBC_STATUS_OK;
    }
    if (strcmp (key, ADBC_CONNECTION_OPTION_READ_ONLY) == 0) {
        const char *v = self->read_only ? ADBC_OPTION_VALUE_ENABLED
                                        : ADBC_OPTION_VALUE_DISABLED;
        size_t need = strlen (v) + 1;
        if (out && *len > 0) {
            size_t cp = need <= *len ? need : *len;
            memcpy (out, v, cp - 1);
            out[cp - 1] = '\0';
        }
        *len = need;
        return ADBC_STATUS_OK;
    }
    return virt_opt_get_string (self->opts, key, out, len, err);
}

AdbcStatusCode
virt_cn_get_option_bytes (struct AdbcConnection *cn, const char *key,
                          uint8_t *out, size_t *len, struct AdbcError *err)
{
    CN_SELF (cn, err);
    return virt_opt_get_bytes (self->opts, key, out, len, err);
}

AdbcStatusCode
virt_cn_get_option_int (struct AdbcConnection *cn, const char *key,
                        int64_t *out, struct AdbcError *err)
{
    CN_SELF (cn, err);
    return virt_opt_get_int (self->opts, key, out, err);
}

AdbcStatusCode
virt_cn_get_option_double (struct AdbcConnection *cn, const char *key,
                           double *out, struct AdbcError *err)
{
    CN_SELF (cn, err);
    return virt_opt_get_double (self->opts, key, out, err);
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_cn_init (struct AdbcConnection *cn, struct AdbcDatabase *db,
              struct AdbcError *err)
{
    VirtAdbcDatabase *vdb;
    SQLHDBC hdbc = SQL_NULL_HDBC;
    SQLRETURN sr;
    char *connstr = NULL;
    AdbcStatusCode rc;

    CN_SELF (cn, err);

    if (!db || !db->private_data)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "ConnectionInit: database is NULL or not initialised");
    vdb = (VirtAdbcDatabase *) db->private_data;
    if (!vdb->initialised || !vdb->henv)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ConnectionInit: DatabaseInit was not called");
    if (self->connected)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ConnectionInit called twice");

    rc = virt_build_connstr (vdb->opts, &connstr, err);
    if (rc != ADBC_STATUS_OK)
        return rc;

    sr = virtodbc__SQLAllocConnect ((SQLHENV) vdb->henv, &hdbc);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        free (connstr);
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, vdb->henv, NULL, NULL,
                                   err);
    }
    self->hdbc = (void *) hdbc;

    sr = SQLDriverConnect (hdbc, NULL, (SQLCHAR *) connstr, SQL_NTS,
                           NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        rc = virt_err_from_odbc (ADBC_STATUS_IO, NULL, hdbc, NULL, err);
        virtodbc__SQLFreeConnect (hdbc);
        self->hdbc = NULL;
        free (connstr);
        return rc;
    }
    free (connstr);

    /* Map autocommit/read_only onto the freshly-opened handle. */
    {
        SQLUINTEGER ac = self->autocommit ? SQL_AUTOCOMMIT_ON
                                          : SQL_AUTOCOMMIT_OFF;
        SQLSetConnectOption (hdbc, SQL_AUTOCOMMIT, ac);
    }
    if (self->read_only) {
        SQLSetConnectOption (hdbc, SQL_ACCESS_MODE,
                             (SQLUINTEGER) SQL_MODE_READ_ONLY);
    }

    self->db = vdb;
    self->connected = 1;
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Transaction control                                                */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_cn_commit (struct AdbcConnection *cn, struct AdbcError *err)
{
    SQLRETURN sr;
    CN_SELF (cn, err);
    if (!self->connected)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "commit: connection is not open");
    if (self->autocommit)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "commit: autocommit is enabled");
    sr = virtodbc__SQLTransact (SQL_NULL_HENV, (SQLHDBC) self->hdbc, SQL_COMMIT);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_from_odbc (ADBC_STATUS_IO, NULL, self->hdbc, NULL, err);
    self->in_txn = 0;
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_cn_rollback (struct AdbcConnection *cn, struct AdbcError *err)
{
    SQLRETURN sr;
    CN_SELF (cn, err);
    if (!self->connected)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "rollback: connection is not open");
    if (self->autocommit)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "rollback: autocommit is enabled");
    sr = virtodbc__SQLTransact (SQL_NULL_HENV, (SQLHDBC) self->hdbc,
                                SQL_ROLLBACK);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_from_odbc (ADBC_STATUS_IO, NULL, self->hdbc, NULL, err);
    self->in_txn = 0;
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Cancel                                                              */
/*                                                                    */
/* ADBC-spec contract: ConnectionCancel cancels the running statement.*/
/* statement.c will publish current_hstmt while executing. We grab it */
/* under the mutex and call virtodbc__SQLCancel. If no statement is   */
/* currently running we return OK silently.                           */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_cn_cancel (struct AdbcConnection *cn, struct AdbcError *err)
{
    pthread_mutex_t *m;
    SQLHSTMT victim = SQL_NULL_HSTMT;

    CN_SELF (cn, err);
    if (!self->connected)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "cancel: connection is not open");

    m = (pthread_mutex_t *) self->mu;
    pthread_mutex_lock (m);
    victim = (SQLHSTMT) self->current_hstmt;
    pthread_mutex_unlock (m);

    if (victim != SQL_NULL_HSTMT) {
        SQLRETURN sr = virtodbc__SQLCancel (victim);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
            return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, self->hdbc,
                                       victim, err);
    }
    return ADBC_STATUS_OK;
}

/* ====================================================================
 *  Phase 9c — ConnectionReadPartition
 *
 *  Deserialise a partition descriptor produced by
 *  StatementExecutePartitions, re-execute the contained SQL on a fresh
 *  HSTMT (possibly on a different connection or in a different
 *  process), and return an ArrowArrayStream of the result rows.
 *
 *  Partition descriptor format (single-partition, version 1):
 *
 *    Offset  Size   Content
 *    ------  ----   -------
 *    0       4      Magic "VIRT"
 *    4       2      Version (uint16, little-endian) — currently 1
 *    6       2      Flags (uint16, little-endian) — bit 0 = is_sparql
 *    8       4      SQL length (uint32, little-endian)
 *    12      N      SQL text (utf-8, NOT null-terminated)
 * ==================================================================== */

AdbcStatusCode
virt_cn_read_partition (struct AdbcConnection *cn,
                        const uint8_t *serialized, size_t length,
                        struct ArrowArrayStream *out, struct AdbcError *err)
{
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    SQLRETURN sr;
    SQLSMALLINT ncols = 0;
    AdbcStatusCode rc;
    char *sql = NULL;
    uint32_t sql_len;
    uint16_t version, flags;
    int is_sparql;

    CN_SELF (cn, err);
    if (!self->connected)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ReadPartition: connection is not open");
    if (!serialized || length < VIRT_PARTITION_HDR_LEN)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "ReadPartition: invalid partition descriptor");
    if (!out)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "ReadPartition: NULL out");

    /* Validate magic and version. */
    if (memcmp (serialized, VIRT_PARTITION_MAGIC, 4) != 0)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "ReadPartition: bad magic (not a Virtuoso partition)");

    version = (uint16_t) serialized[4] | ((uint16_t) serialized[5] << 8);
    if (version != VIRT_PARTITION_VERSION)
        return virt_err_set (err, ADBC_STATUS_NOT_IMPLEMENTED, NULL, 0,
                             "ReadPartition: unsupported version %u", version);

    flags = (uint16_t) serialized[6] | ((uint16_t) serialized[7] << 8);
    is_sparql = (flags & 0x01) ? 1 : 0;

    sql_len  = (uint32_t) serialized[8]
            | ((uint32_t) serialized[9] << 8)
            | ((uint32_t) serialized[10] << 16)
            | ((uint32_t) serialized[11] << 24);

    if (VIRT_PARTITION_HDR_LEN + (size_t) sql_len > length)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "ReadPartition: truncated (declared %u bytes, have %zu)",
                             sql_len, length - VIRT_PARTITION_HDR_LEN);

    sql = (char *) malloc ((size_t) sql_len + 1);
    if (!sql) return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                   "out of memory");
    memcpy (sql, serialized + VIRT_PARTITION_HDR_LEN, sql_len);
    sql[sql_len] = '\0';

    /* Allocate a fresh HSTMT and execute the deserialised query. */
    sr = virtodbc__SQLAllocStmt ((SQLHDBC) self->hdbc, &hstmt);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        free (sql);
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, self->hdbc, NULL,
                                   err);
    }

    /* Rebuild the wire SQL with sparql prefix if the original partition
     * was flagged as SPARQL dialect.                                   */
    {
        const char *q = sql;
        char *prefixed = NULL;
        if (is_sparql) {
            size_t need = 7 + sql_len + 1;
            prefixed = (char *) malloc (need);
            if (!prefixed) { free (sql); virtodbc__SQLFreeStmt (hstmt, SQL_DROP); return ADBC_STATUS_INTERNAL; }
            memcpy (prefixed, "sparql ", 7);
            memcpy (prefixed + 7, sql, sql_len + 1);
            q = prefixed;
        }
        sr = SQLExecDirect (hstmt, (SQLCHAR *) q, SQL_NTS);
        free (prefixed);
    }
    free (sql);

    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        rc = virt_err_from_odbc (ADBC_STATUS_IO, NULL, NULL, hstmt, err);
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }

    sr = virtodbc__SQLNumResultCols (hstmt, &ncols);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        rc = virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL, hstmt, err);
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }

    if (ncols == 0) {
        memset (out, 0, sizeof (*out));
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return ADBC_STATUS_OK;
    }

    rc = virt_reader_create (self, hstmt, 0 /*use default batch_rows*/,
                             is_sparql, out, err);
    if (rc != ADBC_STATUS_OK) {
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }
    return ADBC_STATUS_OK;
}
