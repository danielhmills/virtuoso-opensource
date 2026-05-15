/*
 *  arrow_reader.c
 *
 *  ArrowArrayStream implementation that pulls rows from a Virtuoso
 *  CLI statement handle (HSTMT) via SQLFetch / SQLGetData and emits
 *  Arrow record batches built through nanoarrow.
 *
 *  Phase 4. Per-cell SQLGetData keeps the code simple; phase 4e adds
 *  configurable batching, and a future revision may move to bound
 *  columns (SQLBindCol) for higher throughput.
 *
 *  Lifecycle:
 *     virt_reader_create   -- given an executed HSTMT, prepares the
 *                             cached schema and the column descriptor
 *                             table, then publishes an ArrowArrayStream
 *                             on out_stream. Ownership of the HSTMT
 *                             transfers to the reader.
 *     get_schema           -- returns a deep copy of the cached schema.
 *     get_next             -- pulls up to batch_rows rows, emits one
 *                             record batch. EOF is signalled by a
 *                             zero-length release()'d array (per the
 *                             Arrow C stream contract).
 *     release              -- frees columns, SQLFreeStmt(HSTMT), clears
 *                             current_hstmt on the parent connection.
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the GNU GPL v2; see COPYING in the project root.
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql.h"
#include "sqlext.h"
#include "sqltypes.h"

#include "nanoarrow.h"

#include "virtuoso_adbc.h"

extern SQLRETURN SQL_API virtodbc__SQLNumResultCols (SQLHSTMT, SQLSMALLINT *);
extern SQLRETURN SQL_API virtodbc__SQLDescribeCol  (SQLHSTMT, SQLUSMALLINT,
                                                    SQLCHAR *, SQLSMALLINT,
                                                    SQLSMALLINT *, SQLSMALLINT *,
                                                    SQLULEN *, SQLSMALLINT *,
                                                    SQLSMALLINT *);
extern SQLRETURN SQL_API virtodbc__SQLFetch        (SQLHSTMT);
extern SQLRETURN SQL_API virtodbc__SQLGetData      (SQLHSTMT, SQLUSMALLINT,
                                                    SQLSMALLINT, SQLPOINTER,
                                                    SQLLEN, SQLLEN *);
extern SQLRETURN SQL_API virtodbc__SQLFreeStmt     (SQLHSTMT, SQLUSMALLINT);

/* From type_map.c */
extern enum ArrowType virt_sql_to_arrow_type (int sql_type, int *is_fallback);
extern int            virt_sql_to_c_type     (int sql_type);
extern const char    *virt_sql_type_name     (int sql_type);

/* ------------------------------------------------------------------ */
/*  Reader state                                                       */
/* ------------------------------------------------------------------ */

typedef struct virt_reader_col {
    int  sql_type;
    int  c_type;            /* SQL_C_* passed to SQLGetData */
    int  is_string_like;    /* 1 for utf8/binary/large_*    */
    int  finished;          /* sticky: subsequent batches skip emitting */
} virt_reader_col_t;

typedef struct VirtAdbcReader {
    VirtAdbcConnection *cn;
    SQLHSTMT            hstmt;
    /* Stream's schema. Built once during virt_reader_create. */
    struct ArrowSchema  schema;
    int                 ncols;
    virt_reader_col_t  *cols;
    /* Batch sizing */
    int64_t             batch_rows;
    /* EOF flag: once SQLFetch returned SQL_NO_DATA, all subsequent
     * get_next calls emit a zero-length, released array (Arrow EOF).  */
    int                 eof;
    /* Reusable cell buffer (grown on demand) */
    char               *cellbuf;
    size_t              cellbuf_cap;
    /* Last error message kept for ArrowArrayStream.get_last_error.    */
    char               *last_err;
} VirtAdbcReader;

#define DEFAULT_BATCH_ROWS 4096

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void
reader_set_err (VirtAdbcReader *r, const char *fmt, ...)
{
    va_list ap;
    int len;
    char *buf;

    if (r->last_err) { free (r->last_err); r->last_err = NULL; }
    if (!fmt) return;
    va_start (ap, fmt);
    len = vsnprintf (NULL, 0, fmt, ap);
    va_end (ap);
    if (len < 0) return;
    buf = (char *) malloc ((size_t) len + 1);
    if (!buf) return;
    va_start (ap, fmt);
    vsnprintf (buf, (size_t) len + 1, fmt, ap);
    va_end (ap);
    r->last_err = buf;
}

static int
cellbuf_reserve (VirtAdbcReader *r, size_t need)
{
    char *nb;
    size_t nc;

    if (need <= r->cellbuf_cap)
        return 0;
    nc = r->cellbuf_cap ? r->cellbuf_cap : 256;
    while (nc < need)
        nc *= 2;
    nb = (char *) realloc (r->cellbuf, nc);
    if (!nb)
        return -1;
    r->cellbuf = nb;
    r->cellbuf_cap = nc;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Schema construction (called once)                                  */
/* ------------------------------------------------------------------ */

static AdbcStatusCode
reader_build_schema (VirtAdbcReader *r, struct AdbcError *err)
{
    SQLSMALLINT ncols = 0;
    SQLRETURN sr;
    int i;
    struct ArrowError ae;

    sr = virtodbc__SQLNumResultCols (r->hstmt, &ncols);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL, r->hstmt, err);

    r->ncols = (int) ncols;
    if (r->ncols == 0)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "ExecuteQuery: result set has no columns");

    r->cols = (virt_reader_col_t *) calloc ((size_t) r->ncols,
                                            sizeof (*r->cols));
    if (!r->cols)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");

    ArrowSchemaInit (&r->schema);
    if (ArrowSchemaSetTypeStruct (&r->schema, r->ncols) != 0)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "ArrowSchemaSetTypeStruct failed");

    for (i = 0; i < r->ncols; i++) {
        SQLCHAR colname[256];
        SQLSMALLINT cbname = 0, sqltype = 0, scale = 0, nullable = 0;
        SQLULEN precision = 0;
        enum ArrowType atype;
        int fallback = 0;
        struct ArrowSchema *child = r->schema.children[i];

        colname[0] = '\0';
        sr = virtodbc__SQLDescribeCol (r->hstmt, (SQLUSMALLINT) (i + 1),
                                       colname, (SQLSMALLINT) sizeof (colname),
                                       &cbname, &sqltype, &precision, &scale,
                                       &nullable);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
            return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL,
                                       r->hstmt, err);

        r->cols[i].sql_type = (int) sqltype;
        r->cols[i].c_type   = virt_sql_to_c_type ((int) sqltype);
        atype = virt_sql_to_arrow_type ((int) sqltype, &fallback);

        if (atype == NANOARROW_TYPE_TIMESTAMP) {
            if (ArrowSchemaSetTypeDateTime (child, NANOARROW_TYPE_TIMESTAMP,
                                            NANOARROW_TIME_UNIT_MICRO,
                                            NULL) != 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "ArrowSchemaSetTypeDateTime[ts] failed");
        } else if (atype == NANOARROW_TYPE_TIME64) {
            if (ArrowSchemaSetTypeDateTime (child, NANOARROW_TYPE_TIME64,
                                            NANOARROW_TIME_UNIT_MICRO,
                                            NULL) != 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "ArrowSchemaSetTypeDateTime[time] failed");
        } else if (atype == NANOARROW_TYPE_DATE32) {
            if (ArrowSchemaSetType (child, NANOARROW_TYPE_DATE32) != 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "SetType(date32) failed");
        } else {
            if (ArrowSchemaSetType (child, atype) != 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "SetType(%d) failed", (int) atype);
        }

        r->cols[i].is_string_like =
            (atype == NANOARROW_TYPE_STRING
             || atype == NANOARROW_TYPE_LARGE_STRING
             || atype == NANOARROW_TYPE_BINARY
             || atype == NANOARROW_TYPE_LARGE_BINARY);

        memset (&ae, 0, sizeof (ae));
        if (ArrowSchemaSetName (child,
                                (const char *) (colname[0] ? colname
                                                           : (SQLCHAR *) "")) != 0)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "SetName failed");

        (void) fallback;        /* phase 8 will surface these */
    }

    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/*  Per-cell append                                                    */
/* ------------------------------------------------------------------ */

static int
days_from_civil (int y, unsigned m, unsigned d)
{
    /* Howard Hinnant's days_from_civil */
    int y_adj = (int) (m <= 2 ? y - 1 : y);
    const unsigned era = (unsigned) ((y_adj >= 0 ? y_adj : y_adj - 399) / 400);
    const unsigned yoe = (unsigned) (y_adj - (int) era * 400);
    const unsigned doy =
        (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int) (era * 146097 + doe) - 719468;     /* days since 1970-01-01 */
}

static AdbcStatusCode
reader_append_cell (VirtAdbcReader *r, struct ArrowArray *batch, int icol)
{
    SQLRETURN sr;
    SQLLEN pcb = 0;
    virt_reader_col_t *col = &r->cols[icol];
    struct ArrowArray *child = batch->children[icol];
    union {
        int32_t              i32;
        int64_t              i64;
        float                f32;
        double               f64;
        SQL_DATE_STRUCT      date;
        SQL_TIME_STRUCT      time;
        SQL_TIMESTAMP_STRUCT ts;
    } v;

    if (col->is_string_like) {
        /* SQLGetData with a fixed buffer; on truncation (01004) we
         * grow and call again. SQLGetData remembers per-column where
         * it left off. To keep the protocol simple in phase 4 we use
         * a single big enough call. cellbuf_cap doubles until pcb
         * fits.                                                       */
        size_t off = 0;
        if (cellbuf_reserve (r, 256) < 0)
            return ADBC_STATUS_INTERNAL;
        sr = virtodbc__SQLGetData (r->hstmt, (SQLUSMALLINT) (icol + 1),
                                   col->c_type, r->cellbuf,
                                   (SQLLEN) r->cellbuf_cap, &pcb);
        if (sr == SQL_NO_DATA) {
            if (ArrowArrayAppendNull (child, 1) != 0)
                return ADBC_STATUS_INTERNAL;
            return ADBC_STATUS_OK;
        }
        if (pcb == SQL_NULL_DATA) {
            if (ArrowArrayAppendNull (child, 1) != 0)
                return ADBC_STATUS_INTERNAL;
            return ADBC_STATUS_OK;
        }
        if (sr == SQL_SUCCESS_WITH_INFO) {
            /* Possibly truncated -- pcb reports full length. Grow,
             * read remainder. SQL_C_CHAR/SQL_C_BINARY treat pcb as
             * the number of bytes available after the previous call. */
            if (pcb > 0 && (size_t) pcb >= r->cellbuf_cap) {
                /* What we already have: cellbuf_cap - 1 bytes for SQL_C_CHAR
                 * (NUL-terminated), or cellbuf_cap bytes for SQL_C_BINARY. */
                size_t already = (col->c_type == SQL_C_CHAR)
                                 ? r->cellbuf_cap - 1
                                 : r->cellbuf_cap;
                size_t total   = (size_t) pcb;
                size_t need    = total + 1;
                char  *staging;
                if (cellbuf_reserve (r, need) < 0)
                    return ADBC_STATUS_INTERNAL;
                /* Move what we already have aside, then read the rest
                 * straight into r->cellbuf + already.                  */
                staging = (char *) malloc (already);
                if (!staging)
                    return ADBC_STATUS_INTERNAL;
                memcpy (staging, r->cellbuf, already);
                sr = virtodbc__SQLGetData (r->hstmt,
                                           (SQLUSMALLINT) (icol + 1),
                                           col->c_type,
                                           r->cellbuf + already,
                                           (SQLLEN) (r->cellbuf_cap - already),
                                           &pcb);
                memcpy (r->cellbuf, staging, already);
                free (staging);
                if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
                    reader_set_err (r, "SQLGetData (cont) failed");
                    return ADBC_STATUS_INTERNAL;
                }
                off = total;
            } else if (pcb > 0) {
                off = (size_t) pcb;
            }
        } else if (sr == SQL_SUCCESS) {
            off = (pcb > 0) ? (size_t) pcb : 0;
        } else {
            reader_set_err (r, "SQLGetData failed (rc=%d)", (int) sr);
            return ADBC_STATUS_INTERNAL;
        }

        if (col->c_type == SQL_C_CHAR) {
            struct ArrowStringView sv = { r->cellbuf, (int64_t) off };
            if (ArrowArrayAppendString (child, sv) != 0)
                return ADBC_STATUS_INTERNAL;
        } else {
            struct ArrowBufferView bv = {
                { r->cellbuf },
                (int64_t) off
            };
            if (ArrowArrayAppendBytes (child, bv) != 0)
                return ADBC_STATUS_INTERNAL;
        }
        return ADBC_STATUS_OK;
    }

    /* Fixed-width numeric / temporal types */
    memset (&v, 0, sizeof (v));
    sr = virtodbc__SQLGetData (r->hstmt, (SQLUSMALLINT) (icol + 1),
                               col->c_type, &v, (SQLLEN) sizeof (v), &pcb);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        reader_set_err (r, "SQLGetData fixed failed (rc=%d)", (int) sr);
        return ADBC_STATUS_INTERNAL;
    }
    if (pcb == SQL_NULL_DATA) {
        if (ArrowArrayAppendNull (child, 1) != 0)
            return ADBC_STATUS_INTERNAL;
        return ADBC_STATUS_OK;
    }

    switch (col->c_type) {
    case SQL_C_SLONG:
        if (ArrowArrayAppendInt (child, (int64_t) v.i32) != 0)
            return ADBC_STATUS_INTERNAL;
        break;
    case SQL_C_SBIGINT:
        if (ArrowArrayAppendInt (child, v.i64) != 0)
            return ADBC_STATUS_INTERNAL;
        break;
    case SQL_C_FLOAT:
        if (ArrowArrayAppendDouble (child, (double) v.f32) != 0)
            return ADBC_STATUS_INTERNAL;
        break;
    case SQL_C_DOUBLE:
        if (ArrowArrayAppendDouble (child, v.f64) != 0)
            return ADBC_STATUS_INTERNAL;
        break;
    case SQL_C_TYPE_DATE: {
        int days = days_from_civil ((int) v.date.year, v.date.month,
                                    v.date.day);
        if (ArrowArrayAppendInt (child, (int64_t) days) != 0)
            return ADBC_STATUS_INTERNAL;
        break;
    }
    case SQL_C_TYPE_TIME: {
        int64_t us = ((int64_t) v.time.hour * 3600 + v.time.minute * 60
                      + v.time.second) * 1000000LL;
        if (ArrowArrayAppendInt (child, us) != 0)
            return ADBC_STATUS_INTERNAL;
        break;
    }
    case SQL_C_TYPE_TIMESTAMP: {
        int days = days_from_civil ((int) v.ts.year, v.ts.month, v.ts.day);
        int64_t us = (int64_t) days * 86400LL * 1000000LL
                     + ((int64_t) v.ts.hour * 3600 + v.ts.minute * 60
                        + v.ts.second) * 1000000LL
                     + (int64_t) (v.ts.fraction / 1000u);   /* ns -> us */
        if (ArrowArrayAppendInt (child, us) != 0)
            return ADBC_STATUS_INTERNAL;
        break;
    }
    default:
        reader_set_err (r, "internal: unhandled C type %d", col->c_type);
        return ADBC_STATUS_INTERNAL;
    }
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/*  ArrowArrayStream callbacks                                         */
/* ------------------------------------------------------------------ */

static int
virt_reader_get_schema (struct ArrowArrayStream *stream,
                        struct ArrowSchema *out)
{
    VirtAdbcReader *r = (VirtAdbcReader *) stream->private_data;
    if (!r) return EINVAL;
    return ArrowSchemaDeepCopy (&r->schema, out);
}

static int
virt_reader_get_next (struct ArrowArrayStream *stream,
                      struct ArrowArray *out)
{
    VirtAdbcReader *r = (VirtAdbcReader *) stream->private_data;
    struct ArrowArray batch;
    struct ArrowError ae;
    int64_t rows = 0;
    int rc;

    if (!r) return EINVAL;
    memset (out, 0, sizeof (*out));
    if (r->eof) {
        /* Spec: a released array (null length, no buffers) signals EOF.
         * Zero-init satisfies that.                                     */
        return 0;
    }

    memset (&batch, 0, sizeof (batch));
    memset (&ae, 0, sizeof (ae));
    if (ArrowArrayInitFromSchema (&batch, &r->schema, &ae) != 0) {
        reader_set_err (r, "ArrowArrayInitFromSchema: %s", ae.message);
        return EIO;
    }
    if (ArrowArrayStartAppending (&batch) != 0) {
        ArrowArrayRelease (&batch);
        reader_set_err (r, "ArrowArrayStartAppending failed");
        return EIO;
    }

    for (rows = 0; rows < r->batch_rows; rows++) {
        SQLRETURN sr = virtodbc__SQLFetch (r->hstmt);
        int i;
        if (sr == SQL_NO_DATA) {
            r->eof = 1;
            break;
        }
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
            reader_set_err (r, "SQLFetch failed (rc=%d)", (int) sr);
            ArrowArrayRelease (&batch);
            return EIO;
        }
        for (i = 0; i < r->ncols; i++) {
            AdbcStatusCode arc =
                reader_append_cell (r, &batch, i);
            if (arc != ADBC_STATUS_OK) {
                ArrowArrayRelease (&batch);
                return EIO;
            }
        }
        if (ArrowArrayFinishElement (&batch) != 0) {
            ArrowArrayRelease (&batch);
            reader_set_err (r, "ArrowArrayFinishElement failed");
            return EIO;
        }
    }

    if (rows == 0) {
        /* No rows fetched (immediate EOF): emit a zero-length released
         * array as the EOF marker.                                      */
        ArrowArrayRelease (&batch);
        return 0;
    }

    rc = ArrowArrayFinishBuildingDefault (&batch, &ae);
    if (rc != 0) {
        reader_set_err (r, "ArrowArrayFinishBuildingDefault: %s", ae.message);
        ArrowArrayRelease (&batch);
        return EIO;
    }
    /* Move into out. */
    memcpy (out, &batch, sizeof (batch));
    memset (&batch, 0, sizeof (batch));
    return 0;
}

static const char *
virt_reader_get_last_error (struct ArrowArrayStream *stream)
{
    VirtAdbcReader *r = (VirtAdbcReader *) stream->private_data;
    return r ? r->last_err : NULL;
}

static void
virt_reader_release (struct ArrowArrayStream *stream)
{
    VirtAdbcReader *r;
    if (!stream || !stream->private_data) return;
    r = (VirtAdbcReader *) stream->private_data;

    /* Clear current_hstmt on the connection (under the mutex) before
     * freeing the statement -- so Cancel can't race on a dead handle. */
    if (r->cn) {
        pthread_mutex_t *m = (pthread_mutex_t *) r->cn->mu;
        pthread_mutex_lock (m);
        if (r->cn->current_hstmt == r->hstmt)
            r->cn->current_hstmt = NULL;
        pthread_mutex_unlock (m);
    }

    if (r->hstmt) {
        virtodbc__SQLFreeStmt (r->hstmt, SQL_DROP);
        r->hstmt = NULL;
    }
    if (r->schema.release)
        r->schema.release (&r->schema);
    free (r->cols);
    free (r->cellbuf);
    free (r->last_err);
    free (r);
    stream->private_data = NULL;
    stream->release = NULL;
}

/* ------------------------------------------------------------------ */
/*  Public factory                                                     */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_reader_create (VirtAdbcConnection *cn, void *hstmt, int64_t batch_rows,
                    struct ArrowArrayStream *out_stream,
                    struct AdbcError *err)
{
    VirtAdbcReader *r;
    AdbcStatusCode rc;

    if (!out_stream)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "virt_reader_create: NULL out");

    r = (VirtAdbcReader *) calloc (1, sizeof (*r));
    if (!r)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    r->cn         = cn;
    r->hstmt      = (SQLHSTMT) hstmt;
    r->batch_rows = batch_rows > 0 ? batch_rows : DEFAULT_BATCH_ROWS;

    rc = reader_build_schema (r, err);
    if (rc != ADBC_STATUS_OK) {
        if (r->schema.release)
            r->schema.release (&r->schema);
        free (r->cols);
        free (r);
        return rc;
    }

    /* Publish HSTMT as the connection's current statement so Cancel
     * can target it.                                                  */
    if (cn) {
        pthread_mutex_t *m = (pthread_mutex_t *) cn->mu;
        pthread_mutex_lock (m);
        cn->current_hstmt = hstmt;
        pthread_mutex_unlock (m);
    }

    out_stream->private_data    = r;
    out_stream->get_schema      = virt_reader_get_schema;
    out_stream->get_next        = virt_reader_get_next;
    out_stream->get_last_error  = virt_reader_get_last_error;
    out_stream->release         = virt_reader_release;
    return ADBC_STATUS_OK;
}
