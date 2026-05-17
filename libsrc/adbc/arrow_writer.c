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
 *  arrow_writer.c
 *
 *  Arrow record batch -> Virtuoso CLI parameter binding.
 *
 *  Phase 5: a writer holds per-parameter scratch buffers, calls
 *  SQLBindParameter once after the schema is known, then writes each
 *  Arrow row into the buffers and calls SQLExecute -- one round trip
 *  per row in this phase. (Phase 9 / a later perf pass may switch to
 *  SQL_ATTR_PARAMSET_SIZE for column-batched execution.)
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the GNU GPL v2; see COPYING in the project root.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sql.h"
#include "sqlext.h"
#include "sqltypes.h"

#include <nanoarrow/nanoarrow.h>

#include "virtuoso_adbc.h"

extern SQLRETURN SQL_API virtodbc__SQLBindParameter (SQLHSTMT, SQLUSMALLINT,
                                                     SQLSMALLINT, SQLSMALLINT,
                                                     SQLSMALLINT, SQLULEN,
                                                     SQLSMALLINT, SQLPOINTER,
                                                     SQLLEN, SQLLEN *);
extern SQLRETURN SQL_API SQLExecute (SQLHSTMT);

/* ------------------------------------------------------------------ */
/*  Per-parameter slot                                                 */
/* ------------------------------------------------------------------ */

typedef struct virt_param_slot {
    SQLSMALLINT c_type;
    SQLSMALLINT sql_type;
    SQLULEN     col_size;
    SQLSMALLINT scale;
    /* Indicator passed to SQLBindParameter; updated per row.         */
    SQLLEN      ind;
    /* Fixed-width payload. SQLBindParameter receives &fixed for
     * numeric / temporal types, &vbuf[0] for variable-width.         */
    union {
        int8_t               i8;
        int16_t              i16;
        int32_t              i32;
        int64_t              i64;
        float                f32;
        double               f64;
        SQL_DATE_STRUCT      date;
        SQL_TIME_STRUCT      time;
        SQL_TIMESTAMP_STRUCT ts;
    } fixed;
    /* Variable-width payload. Grown on demand.                       */
    uint8_t    *vbuf;
    SQLLEN      vbuf_cap;
    int         is_var;
} virt_param_slot_t;

struct VirtAdbcWriter {
    SQLHSTMT            hstmt;
    int                 nparams;
    virt_param_slot_t  *slots;
    int                 bound;       /* SQLBindParameter called once    */
    struct ArrowSchema  schema;      /* deep copy of the bind schema    */
    struct ArrowArrayView view;
};

/* ------------------------------------------------------------------ */
/*  Construction                                                       */
/* ------------------------------------------------------------------ */

/* Map an Arrow type id -> (c_type, sql_type, fixed-size hint).
 * Returns -1 for unsupported types. */
static int
infer_param_types (enum ArrowType atype, SQLSMALLINT *c_type,
                   SQLSMALLINT *sql_type, int *is_var)
{
    *is_var = 0;
    switch (atype) {
    case NANOARROW_TYPE_BOOL:
        *c_type = SQL_C_BIT;     *sql_type = SQL_BIT;          return 0;
    case NANOARROW_TYPE_INT8:
        *c_type = SQL_C_STINYINT; *sql_type = SQL_TINYINT;     return 0;
    case NANOARROW_TYPE_INT16:
        *c_type = SQL_C_SSHORT;  *sql_type = SQL_SMALLINT;     return 0;
    case NANOARROW_TYPE_INT32:
        *c_type = SQL_C_SLONG;   *sql_type = SQL_INTEGER;      return 0;
    case NANOARROW_TYPE_INT64:
        *c_type = SQL_C_SBIGINT; *sql_type = SQL_BIGINT;       return 0;
    case NANOARROW_TYPE_FLOAT:
        *c_type = SQL_C_FLOAT;   *sql_type = SQL_REAL;         return 0;
    case NANOARROW_TYPE_DOUBLE:
        *c_type = SQL_C_DOUBLE;  *sql_type = SQL_DOUBLE;       return 0;
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
        *c_type = SQL_C_CHAR;    *sql_type = SQL_VARCHAR;
        *is_var = 1;                                            return 0;
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:
        *c_type = SQL_C_BINARY;  *sql_type = SQL_VARBINARY;
        *is_var = 1;                                            return 0;
    case NANOARROW_TYPE_DATE32:
        *c_type = SQL_C_TYPE_DATE; *sql_type = SQL_TYPE_DATE;  return 0;
    case NANOARROW_TYPE_TIME64:
        *c_type = SQL_C_TYPE_TIME; *sql_type = SQL_TYPE_TIME;  return 0;
    case NANOARROW_TYPE_TIMESTAMP:
        *c_type = SQL_C_TYPE_TIMESTAMP;
        *sql_type = SQL_TYPE_TIMESTAMP;                        return 0;
    default:
        return -1;
    }
}

static int
slot_reserve_var (virt_param_slot_t *s, SQLLEN need)
{
    SQLLEN nc;
    uint8_t *nb;

    if (need <= s->vbuf_cap)
        return 0;
    nc = s->vbuf_cap ? s->vbuf_cap : 256;
    while (nc < need)
        nc *= 2;
    nb = (uint8_t *) realloc (s->vbuf, (size_t) nc);
    if (!nb)
        return -1;
    s->vbuf = nb;
    s->vbuf_cap = nc;
    return 0;
}

AdbcStatusCode
virt_writer_create (void *hstmt, struct ArrowSchema *bind_schema,
                    VirtAdbcWriter **out_writer, struct AdbcError *err)
{
    VirtAdbcWriter *w;
    int i;
    struct ArrowError ae;

    if (!bind_schema || !bind_schema->release || bind_schema->n_children <= 0)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "Bind: schema must be a non-empty struct");

    w = (VirtAdbcWriter *) calloc (1, sizeof (*w));
    if (!w)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    w->hstmt   = (SQLHSTMT) hstmt;
    w->nparams = (int) bind_schema->n_children;

    /* Deep-copy the schema so the writer owns its lifetime. */
    if (ArrowSchemaDeepCopy (bind_schema, &w->schema) != 0) {
        free (w);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "ArrowSchemaDeepCopy failed");
    }

    w->slots = (virt_param_slot_t *) calloc ((size_t) w->nparams,
                                             sizeof (*w->slots));
    if (!w->slots) {
        w->schema.release (&w->schema);
        free (w);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    }

    memset (&ae, 0, sizeof (ae));
    if (ArrowArrayViewInitFromSchema (&w->view, &w->schema, &ae) != 0) {
        free (w->slots);
        w->schema.release (&w->schema);
        free (w);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "ArrowArrayViewInitFromSchema: %s", ae.message);
    }

    /* Pre-fill slot types so we can SQLBindParameter at execute time. */
    for (i = 0; i < w->nparams; i++) {
        struct ArrowSchemaView sv;
        struct ArrowSchema *child = w->schema.children[i];
        SQLSMALLINT c_type = 0, sql_type = 0;
        int is_var = 0;

        memset (&ae, 0, sizeof (ae));
        if (ArrowSchemaViewInit (&sv, child, &ae) != 0) {
            virt_writer_destroy (w);
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "ArrowSchemaViewInit[%d]: %s", i, ae.message);
        }
        if (infer_param_types (sv.type, &c_type, &sql_type, &is_var) < 0) {
            virt_writer_destroy (w);
            return virt_err_set (err, ADBC_STATUS_NOT_IMPLEMENTED, NULL, 0,
                                 "Bind: parameter %d has unsupported type",
                                 i);
        }
        w->slots[i].c_type   = c_type;
        w->slots[i].sql_type = sql_type;
        w->slots[i].is_var   = is_var;
        w->slots[i].col_size = is_var ? 65535 : 0;
        w->slots[i].scale    = 0;
    }

    *out_writer = w;
    return ADBC_STATUS_OK;
}

void
virt_writer_destroy (VirtAdbcWriter *w)
{
    int i;
    if (!w) return;
    if (w->view.array != NULL || w->view.length != 0)
        ArrowArrayViewReset (&w->view);
    if (w->slots) {
        for (i = 0; i < w->nparams; i++)
            free (w->slots[i].vbuf);
        free (w->slots);
    }
    if (w->schema.release)
        w->schema.release (&w->schema);
    free (w);
}

/* ------------------------------------------------------------------ */
/*  Bind once + execute per row                                        */
/* ------------------------------------------------------------------ */

static AdbcStatusCode
writer_bind_once (VirtAdbcWriter *w, struct AdbcError *err)
{
    int i;

    if (w->bound)
        return ADBC_STATUS_OK;
    for (i = 0; i < w->nparams; i++) {
        virt_param_slot_t *s = &w->slots[i];
        SQLPOINTER ptr;
        SQLLEN buflen;
        SQLRETURN sr;

        if (s->is_var) {
            /* Initial allocation; grows on demand per row. */
            if (slot_reserve_var (s, 256) < 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "out of memory");
            ptr    = (SQLPOINTER) s->vbuf;
            buflen = s->vbuf_cap;
        } else {
            ptr    = (SQLPOINTER) &s->fixed;
            buflen = (SQLLEN) sizeof (s->fixed);
        }

        sr = virtodbc__SQLBindParameter (w->hstmt,
                                         (SQLUSMALLINT) (i + 1),
                                         SQL_PARAM_INPUT,
                                         s->c_type, s->sql_type,
                                         s->col_size, s->scale,
                                         ptr, buflen, &s->ind);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
            return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL,
                                       w->hstmt, err);
    }
    w->bound = 1;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode
writer_load_row (VirtAdbcWriter *w, int64_t row, struct AdbcError *err)
{
    int i;

    for (i = 0; i < w->nparams; i++) {
        virt_param_slot_t *s = &w->slots[i];
        struct ArrowArrayView *cv = w->view.children[i];

        if (ArrowArrayViewIsNull (cv, row)) {
            s->ind = SQL_NULL_DATA;
            continue;
        }

        switch (s->c_type) {
        case SQL_C_BIT:
            s->fixed.i8 = (int8_t) (ArrowArrayViewGetIntUnsafe (cv, row) ? 1 : 0);
            s->ind = sizeof (s->fixed.i8);
            break;
        case SQL_C_STINYINT:
            s->fixed.i8 = (int8_t) ArrowArrayViewGetIntUnsafe (cv, row);
            s->ind = sizeof (s->fixed.i8);
            break;
        case SQL_C_SSHORT:
            s->fixed.i16 = (int16_t) ArrowArrayViewGetIntUnsafe (cv, row);
            s->ind = sizeof (s->fixed.i16);
            break;
        case SQL_C_SLONG:
            s->fixed.i32 = (int32_t) ArrowArrayViewGetIntUnsafe (cv, row);
            s->ind = sizeof (s->fixed.i32);
            break;
        case SQL_C_SBIGINT:
            s->fixed.i64 = ArrowArrayViewGetIntUnsafe (cv, row);
            s->ind = sizeof (s->fixed.i64);
            break;
        case SQL_C_FLOAT:
            s->fixed.f32 = (float) ArrowArrayViewGetDoubleUnsafe (cv, row);
            s->ind = sizeof (s->fixed.f32);
            break;
        case SQL_C_DOUBLE:
            s->fixed.f64 = ArrowArrayViewGetDoubleUnsafe (cv, row);
            s->ind = sizeof (s->fixed.f64);
            break;
        case SQL_C_CHAR: {
            struct ArrowStringView sv = ArrowArrayViewGetStringUnsafe (cv, row);
            SQLLEN need = (SQLLEN) sv.size_bytes + 1;
            if (slot_reserve_var (s, need) < 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "out of memory growing string param");
            memcpy (s->vbuf, sv.data, (size_t) sv.size_bytes);
            s->vbuf[sv.size_bytes] = 0;
            s->ind = (SQLLEN) sv.size_bytes;
            /* Re-bind because the buffer pointer may have moved. */
            {
                SQLRETURN sr = virtodbc__SQLBindParameter (
                    w->hstmt, (SQLUSMALLINT) (i + 1),
                    SQL_PARAM_INPUT, s->c_type, s->sql_type,
                    s->col_size, s->scale,
                    (SQLPOINTER) s->vbuf, s->vbuf_cap, &s->ind);
                if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
                    return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL,
                                               w->hstmt, err);
            }
            break;
        }
        case SQL_C_BINARY: {
            struct ArrowBufferView bv = ArrowArrayViewGetBytesUnsafe (cv, row);
            SQLLEN need = (SQLLEN) bv.size_bytes;
            if (slot_reserve_var (s, need > 0 ? need : 1) < 0)
                return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                     "out of memory growing binary param");
            memcpy (s->vbuf, bv.data.data, (size_t) bv.size_bytes);
            s->ind = (SQLLEN) bv.size_bytes;
            {
                SQLRETURN sr = virtodbc__SQLBindParameter (
                    w->hstmt, (SQLUSMALLINT) (i + 1),
                    SQL_PARAM_INPUT, s->c_type, s->sql_type,
                    s->col_size, s->scale,
                    (SQLPOINTER) s->vbuf, s->vbuf_cap, &s->ind);
                if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
                    return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL,
                                               w->hstmt, err);
            }
            break;
        }
        case SQL_C_TYPE_DATE: {
            /* Arrow date32 = days since 1970-01-01 */
            int32_t d = (int32_t) ArrowArrayViewGetIntUnsafe (cv, row);
            int64_t y, m, day;
            int64_t z = d + 719468;
            int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            unsigned doe = (unsigned) (z - era * 146097);
            unsigned yoe =
                (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            int64_t y_adj = (int64_t) yoe + era * 400;
            unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            unsigned mp  = (5 * doy + 2) / 153;
            day = doy - (153 * mp + 2) / 5 + 1;
            m   = mp < 10 ? mp + 3 : mp - 9;
            y   = y_adj + (m <= 2 ? 1 : 0);
            s->fixed.date.year  = (SQLSMALLINT) y;
            s->fixed.date.month = (SQLUSMALLINT) m;
            s->fixed.date.day   = (SQLUSMALLINT) day;
            s->ind = sizeof (s->fixed.date);
            break;
        }
        case SQL_C_TYPE_TIME: {
            int64_t us  = ArrowArrayViewGetIntUnsafe (cv, row);
            int64_t sec = us / 1000000;
            s->fixed.time.hour   = (SQLUSMALLINT) ((sec / 3600) % 24);
            s->fixed.time.minute = (SQLUSMALLINT) ((sec / 60) % 60);
            s->fixed.time.second = (SQLUSMALLINT) (sec % 60);
            s->ind = sizeof (s->fixed.time);
            break;
        }
        case SQL_C_TYPE_TIMESTAMP: {
            int64_t us  = ArrowArrayViewGetIntUnsafe (cv, row);
            int64_t sec = us / 1000000;
            int64_t frac_us = us - sec * 1000000;
            int64_t days = sec / 86400;
            int64_t rem  = sec - days * 86400;
            if (rem < 0) { rem += 86400; days -= 1; }
            int64_t y, m, day;
            int64_t z = days + 719468;
            int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            unsigned doe = (unsigned) (z - era * 146097);
            unsigned yoe =
                (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            int64_t y_adj = (int64_t) yoe + era * 400;
            unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            unsigned mp  = (5 * doy + 2) / 153;
            day = doy - (153 * mp + 2) / 5 + 1;
            m   = mp < 10 ? mp + 3 : mp - 9;
            y   = y_adj + (m <= 2 ? 1 : 0);
            s->fixed.ts.year     = (SQLSMALLINT) y;
            s->fixed.ts.month    = (SQLUSMALLINT) m;
            s->fixed.ts.day      = (SQLUSMALLINT) day;
            s->fixed.ts.hour     = (SQLUSMALLINT) (rem / 3600);
            s->fixed.ts.minute   = (SQLUSMALLINT) ((rem / 60) % 60);
            s->fixed.ts.second   = (SQLUSMALLINT) (rem % 60);
            /* SQL_TIMESTAMP_STRUCT.fraction is in nanoseconds. */
            s->fixed.ts.fraction = (SQLUINTEGER) (frac_us * 1000);
            s->ind = sizeof (s->fixed.ts);
            break;
        }
        default:
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "internal: unhandled bind c_type %d",
                                 s->c_type);
        }
    }
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_writer_execute_batch (VirtAdbcWriter *w, struct ArrowArray *batch,
                           int64_t *rows_affected_inout,
                           struct AdbcError *err)
{
    AdbcStatusCode rc;
    struct ArrowError ae;
    int64_t r;

    if (!w || !batch)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "writer_execute: NULL writer/batch");

    rc = writer_bind_once (w, err);
    if (rc != ADBC_STATUS_OK)
        return rc;

    memset (&ae, 0, sizeof (ae));
    if (ArrowArrayViewSetArray (&w->view, batch, &ae) != 0)
        return virt_err_set (err, ADBC_STATUS_INVALID_DATA, NULL, 0,
                             "ArrowArrayViewSetArray: %s", ae.message);

    for (r = 0; r < batch->length; r++) {
        SQLRETURN sr;
        rc = writer_load_row (w, r, err);
        if (rc != ADBC_STATUS_OK)
            return rc;
        sr = SQLExecute (w->hstmt);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
            return virt_err_from_odbc (ADBC_STATUS_IO, NULL, NULL, w->hstmt,
                                       err);
        if (rows_affected_inout)
            (*rows_affected_inout)++;
    }
    return ADBC_STATUS_OK;
}
