/*
 *  catalog.c
 *
 *  ADBC connection-level metadata: GetTableTypes, GetInfo,
 *  GetTableSchema, GetObjects.
 *
 *  Phase 6. All four entrypoints produce Arrow data shaped to the
 *  ADBC 1.1.0 spec. GetTableTypes returns the static list ["TABLE",
 *  "VIEW", "SYSTEM TABLE"]; GetInfo emits the dense-union info_value
 *  schema; GetTableSchema prepares "SELECT * FROM <table>" and
 *  walks the result-set descriptor via SQLNumResultCols /
 *  SQLDescribeCol; GetObjects walks SQLTables + SQLColumns and
 *  groups the result into the nested catalog/schema/table/column
 *  tree the spec defines.
 *
 *  Constraints (table_constraints, USAGE_SCHEMA) are surfaced as
 *  empty lists in this phase; a later iteration may populate them
 *  by adding SQLPrimaryKeys / SQLForeignKeys calls.
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
#include "sqltypes.h"

#include "nanoarrow.h"

#include "virtuoso_adbc.h"

extern SQLRETURN SQL_API virtodbc__SQLAllocStmt    (SQLHDBC, SQLHSTMT *);
extern SQLRETURN SQL_API virtodbc__SQLFreeStmt     (SQLHSTMT, SQLUSMALLINT);
extern SQLRETURN SQL_API virtodbc__SQLFetch        (SQLHSTMT);
extern SQLRETURN SQL_API virtodbc__SQLGetData      (SQLHSTMT, SQLUSMALLINT,
                                                    SQLSMALLINT, SQLPOINTER,
                                                    SQLLEN, SQLLEN *);
extern SQLRETURN SQL_API virtodbc__SQLNumResultCols (SQLHSTMT, SQLSMALLINT *);
extern SQLRETURN SQL_API virtodbc__SQLDescribeCol  (SQLHSTMT, SQLUSMALLINT,
                                                    SQLCHAR *, SQLSMALLINT,
                                                    SQLSMALLINT *, SQLSMALLINT *,
                                                    SQLULEN *, SQLSMALLINT *,
                                                    SQLSMALLINT *);
extern SQLRETURN SQL_API SQLPrepare    (SQLHSTMT, SQLCHAR *, SQLINTEGER);
extern SQLRETURN SQL_API SQLTables     (SQLHSTMT, SQLCHAR *, SQLSMALLINT,
                                        SQLCHAR *, SQLSMALLINT, SQLCHAR *,
                                        SQLSMALLINT, SQLCHAR *, SQLSMALLINT);
extern SQLRETURN SQL_API SQLColumns    (SQLHSTMT, SQLCHAR *, SQLSMALLINT,
                                        SQLCHAR *, SQLSMALLINT, SQLCHAR *,
                                        SQLSMALLINT, SQLCHAR *, SQLSMALLINT);

extern enum ArrowType virt_sql_to_arrow_type (int sql_type, int *fallback);

/* ====================================================================
 *   Single-batch ArrowArrayStream helper
 * ==================================================================== */

typedef struct one_batch_state {
    struct ArrowSchema schema;
    struct ArrowArray  array;
    int                consumed;
    char              *last_err;
} one_batch_state_t;

static int
one_batch_get_schema (struct ArrowArrayStream *s, struct ArrowSchema *out)
{
    one_batch_state_t *st = (one_batch_state_t *) s->private_data;
    return ArrowSchemaDeepCopy (&st->schema, out);
}

static int
one_batch_get_next (struct ArrowArrayStream *s, struct ArrowArray *out)
{
    one_batch_state_t *st = (one_batch_state_t *) s->private_data;
    memset (out, 0, sizeof (*out));
    if (st->consumed || st->array.length == 0)
        return 0;       /* EOF */
    /* Move ownership of the array to the caller. */
    memcpy (out, &st->array, sizeof (st->array));
    memset (&st->array, 0, sizeof (st->array));
    st->consumed = 1;
    return 0;
}

static const char *
one_batch_last_err (struct ArrowArrayStream *s)
{
    one_batch_state_t *st = (one_batch_state_t *) s->private_data;
    return st ? st->last_err : NULL;
}

static void
one_batch_release (struct ArrowArrayStream *s)
{
    one_batch_state_t *st = (one_batch_state_t *) s->private_data;
    if (!st) return;
    if (st->schema.release) st->schema.release (&st->schema);
    if (st->array.release)  st->array.release  (&st->array);
    free (st->last_err);
    free (st);
    s->private_data = NULL;
    s->release      = NULL;
}

/* Take ownership of schema + array (zeroes the caller's copies) and
 * publishes a single-batch ArrowArrayStream on *out.                  */
static AdbcStatusCode
publish_one_batch (struct ArrowSchema *schema, struct ArrowArray *array,
                   struct ArrowArrayStream *out, struct AdbcError *err)
{
    one_batch_state_t *st;

    st = (one_batch_state_t *) calloc (1, sizeof (*st));
    if (!st)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    memcpy (&st->schema, schema, sizeof (*schema));
    memcpy (&st->array,  array,  sizeof (*array));
    memset (schema, 0, sizeof (*schema));
    memset (array,  0, sizeof (*array));

    out->private_data    = st;
    out->get_schema      = one_batch_get_schema;
    out->get_next        = one_batch_get_next;
    out->get_last_error  = one_batch_last_err;
    out->release         = one_batch_release;
    return ADBC_STATUS_OK;
}

/* ====================================================================
 *   GetTableTypes  (phase 6a)
 * ==================================================================== */

AdbcStatusCode
virt_cn_get_table_types (struct AdbcConnection *cn,
                         struct ArrowArrayStream *out, struct AdbcError *err)
{
    struct ArrowSchema schema;
    struct ArrowArray  array;
    struct ArrowError  ae;
    static const char *const kTypes[] = { "TABLE", "VIEW", "SYSTEM TABLE" };
    size_t i;

    (void) cn;
    if (!out)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "GetTableTypes: NULL out");

    memset (&schema, 0, sizeof (schema));
    memset (&array,  0, sizeof (array));
    memset (&ae, 0, sizeof (ae));

    ArrowSchemaInit (&schema);
    if (ArrowSchemaSetTypeStruct (&schema, 1) != 0
        || ArrowSchemaSetType (schema.children[0], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (schema.children[0], "table_type") != 0)
        goto schema_err;
    schema.children[0]->flags &= ~ARROW_FLAG_NULLABLE;

    if (ArrowArrayInitFromSchema (&array, &schema, &ae) != 0
        || ArrowArrayStartAppending (&array) != 0)
        goto build_err;

    for (i = 0; i < sizeof (kTypes) / sizeof (kTypes[0]); i++) {
        if (ArrowArrayAppendString (array.children[0],
                                    ArrowCharView (kTypes[i])) != 0
            || ArrowArrayFinishElement (&array) != 0)
            goto build_err;
    }
    if (ArrowArrayFinishBuildingDefault (&array, &ae) != 0)
        goto build_err;

    return publish_one_batch (&schema, &array, out, err);

schema_err:
    if (schema.release) schema.release (&schema);
    return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                         "GetTableTypes: schema build failed");
build_err:
    if (array.release)  array.release  (&array);
    if (schema.release) schema.release (&schema);
    return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                         "GetTableTypes: array build failed: %s",
                         ae.message[0] ? ae.message : "(no msg)");
}

/* ====================================================================
 *   GetInfo  (phase 6b)
 *
 *   The info_value column is a dense union with members:
 *     0: utf8        (string_value)
 *     1: bool        (bool_value)
 *     2: int64       (int64_value)
 *     3: int32       (int32_bitmask)
 *     4: list<utf8>  (string_list)
 *     5: map<i32,list<i32>> (int32_to_int32_list_map)
 *   We only ever emit type_id=0 (string) or type_id=2 (int64), but
 *   the full schema is required by the spec.
 * ==================================================================== */

static int
info_schema_build (struct ArrowSchema *schema)
{
    struct ArrowSchema *iv;

    ArrowSchemaInit (schema);
    if (ArrowSchemaSetTypeStruct (schema, 2) != 0)
        return -1;

    if (ArrowSchemaSetType (schema->children[0], NANOARROW_TYPE_UINT32) != 0
        || ArrowSchemaSetName (schema->children[0], "info_name") != 0)
        return -1;
    schema->children[0]->flags &= ~ARROW_FLAG_NULLABLE;

    iv = schema->children[1];
    if (ArrowSchemaSetTypeUnion (iv, NANOARROW_TYPE_DENSE_UNION, 6) != 0
        || ArrowSchemaSetName (iv, "info_value") != 0)
        return -1;

    if (ArrowSchemaSetType (iv->children[0], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (iv->children[0], "string_value") != 0)
        return -1;
    if (ArrowSchemaSetType (iv->children[1], NANOARROW_TYPE_BOOL) != 0
        || ArrowSchemaSetName (iv->children[1], "bool_value") != 0)
        return -1;
    if (ArrowSchemaSetType (iv->children[2], NANOARROW_TYPE_INT64) != 0
        || ArrowSchemaSetName (iv->children[2], "int64_value") != 0)
        return -1;
    if (ArrowSchemaSetType (iv->children[3], NANOARROW_TYPE_INT32) != 0
        || ArrowSchemaSetName (iv->children[3], "int32_bitmask") != 0)
        return -1;
    if (ArrowSchemaSetType (iv->children[4], NANOARROW_TYPE_LIST) != 0
        || ArrowSchemaSetName (iv->children[4], "string_list") != 0
        || ArrowSchemaSetType (iv->children[4]->children[0],
                               NANOARROW_TYPE_STRING) != 0)
        return -1;
    if (ArrowSchemaSetType (iv->children[5], NANOARROW_TYPE_MAP) != 0
        || ArrowSchemaSetName (iv->children[5], "int32_to_int32_list_map") != 0
        || ArrowSchemaSetType (iv->children[5]->children[0]->children[0],
                               NANOARROW_TYPE_INT32) != 0
        || ArrowSchemaSetType (iv->children[5]->children[0]->children[1],
                               NANOARROW_TYPE_LIST) != 0
        || ArrowSchemaSetType (
               iv->children[5]->children[0]->children[1]->children[0],
               NANOARROW_TYPE_INT32) != 0)
        return -1;
    return 0;
}

static AdbcStatusCode
info_append_string (struct ArrowArray *array, uint32_t code,
                    const char *value, struct AdbcError *err)
{
    if (ArrowArrayAppendUInt (array->children[0], (uint64_t) code) != 0
        || ArrowArrayAppendString (array->children[1]->children[0],
                                   ArrowCharView (value ? value : "")) != 0
        || ArrowArrayFinishUnionElement (array->children[1], 0) != 0
        || ArrowArrayFinishElement (array) != 0)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "GetInfo append string");
    return ADBC_STATUS_OK;
}

static AdbcStatusCode
info_append_int (struct ArrowArray *array, uint32_t code, int64_t value,
                 struct AdbcError *err)
{
    if (ArrowArrayAppendUInt (array->children[0], (uint64_t) code) != 0
        || ArrowArrayAppendInt (array->children[1]->children[2], value) != 0
        || ArrowArrayFinishUnionElement (array->children[1], 2) != 0
        || ArrowArrayFinishElement (array) != 0)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "GetInfo append int");
    return ADBC_STATUS_OK;
}

/* Query the server for "OpenLink Virtuoso Server X.Y.Z" or similar.
 * Returns a malloc()'d string; caller frees. May return NULL.        */
static char *
fetch_vendor_version (SQLHDBC hdbc)
{
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    char buf[256];
    SQLLEN pcb = 0;
    SQLRETURN sr;

    if (virtodbc__SQLAllocStmt (hdbc, &hstmt) != SQL_SUCCESS)
        return NULL;
    sr = SQLExecDirect (hstmt, (SQLCHAR *) "select sys_stat('st_dbms_ver')",
                        SQL_NTS);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        goto out;
    sr = virtodbc__SQLFetch (hstmt);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        goto out;
    sr = virtodbc__SQLGetData (hstmt, 1, SQL_C_CHAR, buf, sizeof (buf), &pcb);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        goto out;
    virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
    if (pcb <= 0 || pcb >= (SQLLEN) sizeof (buf))
        return NULL;
    return strdup (buf);
out:
    virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
    return NULL;
}

extern SQLRETURN SQL_API SQLExecDirect (SQLHSTMT, SQLCHAR *, SQLINTEGER);

AdbcStatusCode
virt_cn_get_info (struct AdbcConnection *cn, const uint32_t *info_codes,
                  size_t info_codes_length, struct ArrowArrayStream *out,
                  struct AdbcError *err)
{
    VirtAdbcConnection *vcn;
    struct ArrowSchema schema;
    struct ArrowArray  array;
    struct ArrowError  ae;
    char              *vendor_version = NULL;
    AdbcStatusCode rc;
    size_t i;
    /* Default set if caller passes NULL info_codes. */
    static const uint32_t kDefaultCodes[] = {
        ADBC_INFO_VENDOR_NAME,
        ADBC_INFO_VENDOR_VERSION,
        ADBC_INFO_DRIVER_NAME,
        ADBC_INFO_DRIVER_VERSION,
        ADBC_INFO_DRIVER_ADBC_VERSION,
    };

    if (!cn || !cn->private_data)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "GetInfo: connection not initialised");
    vcn = (VirtAdbcConnection *) cn->private_data;
    if (!vcn->connected)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "GetInfo: connection not open");
    if (!out)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "GetInfo: NULL out");

    if (!info_codes) {
        info_codes        = kDefaultCodes;
        info_codes_length = sizeof (kDefaultCodes) / sizeof (kDefaultCodes[0]);
    }

    memset (&schema, 0, sizeof (schema));
    memset (&array,  0, sizeof (array));
    memset (&ae, 0, sizeof (ae));

    if (info_schema_build (&schema) != 0) {
        if (schema.release) schema.release (&schema);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "GetInfo: schema build failed");
    }

    if (ArrowArrayInitFromSchema (&array, &schema, &ae) != 0
        || ArrowArrayStartAppending (&array) != 0) {
        if (array.release)  array.release  (&array);
        if (schema.release) schema.release (&schema);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "GetInfo: array init failed: %s",
                             ae.message[0] ? ae.message : "");
    }

    for (i = 0; i < info_codes_length; i++) {
        uint32_t code = info_codes[i];
        switch (code) {
        case ADBC_INFO_VENDOR_NAME:
            rc = info_append_string (&array, code, "OpenLink Virtuoso", err);
            break;
        case ADBC_INFO_VENDOR_VERSION:
            if (!vendor_version)
                vendor_version = fetch_vendor_version ((SQLHDBC) vcn->hdbc);
            rc = info_append_string (&array, code,
                                     vendor_version ? vendor_version
                                                    : "unknown", err);
            break;
        case ADBC_INFO_DRIVER_NAME:
            rc = info_append_string (&array, code, VIRT_ADBC_DRIVER_NAME, err);
            break;
        case ADBC_INFO_DRIVER_VERSION:
            rc = info_append_string (&array, code, VIRT_ADBC_DRIVER_VERSION,
                                     err);
            break;
        case ADBC_INFO_DRIVER_ADBC_VERSION:
            rc = info_append_int (&array, code,
                                  (int64_t) ADBC_VERSION_1_1_0, err);
            break;
        default:
            /* Unrecognised codes are silently skipped per the spec. */
            continue;
        }
        if (rc != ADBC_STATUS_OK) {
            free (vendor_version);
            if (array.release)  array.release  (&array);
            if (schema.release) schema.release (&schema);
            return rc;
        }
    }
    free (vendor_version);

    if (ArrowArrayFinishBuildingDefault (&array, &ae) != 0) {
        if (array.release)  array.release  (&array);
        if (schema.release) schema.release (&schema);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "GetInfo: finish: %s",
                             ae.message[0] ? ae.message : "");
    }

    return publish_one_batch (&schema, &array, out, err);
}

/* ====================================================================
 *   GetTableSchema  (phase 6c)
 *
 *   SQLPrepare "SELECT * FROM [<cat>.][<schema>.]<table>" and walk
 *   the prepared-statement column descriptor. Identifiers are
 *   double-quoted; rejection of NULL/empty table_name is done up
 *   front.
 * ==================================================================== */

static char *
build_qualified_sql (const char *catalog, const char *db_schema,
                     const char *table)
{
    /* "select * from [<cat>.][<sch>.]<tbl> where 0 = 1"        */
    size_t cap = 64
                 + (catalog   ? strlen (catalog)   + 4 : 0)
                 + (db_schema ? strlen (db_schema) + 4 : 0)
                 + strlen (table) + 4;
    char *buf = (char *) malloc (cap);
    if (!buf) return NULL;
    if (catalog && db_schema && table)
        snprintf (buf, cap,
                  "select * from \"%s\".\"%s\".\"%s\" where 0 = 1",
                  catalog, db_schema, table);
    else if (db_schema && table)
        snprintf (buf, cap, "select * from \"%s\".\"%s\" where 0 = 1",
                  db_schema, table);
    else
        snprintf (buf, cap, "select * from \"%s\" where 0 = 1", table);
    return buf;
}

AdbcStatusCode
virt_cn_get_table_schema (struct AdbcConnection *cn, const char *catalog,
                          const char *db_schema, const char *table_name,
                          struct ArrowSchema *out, struct AdbcError *err)
{
    VirtAdbcConnection *vcn;
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    SQLRETURN sr;
    SQLSMALLINT ncols = 0;
    int i;
    char *sql = NULL;
    AdbcStatusCode rc;

    if (!cn || !cn->private_data)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "GetTableSchema: connection not initialised");
    vcn = (VirtAdbcConnection *) cn->private_data;
    if (!vcn->connected)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "GetTableSchema: connection not open");
    if (!table_name || !*table_name)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "GetTableSchema: empty table name");
    if (!out)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "GetTableSchema: NULL out");

    sql = build_qualified_sql (catalog, db_schema, table_name);
    if (!sql)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");

    sr = virtodbc__SQLAllocStmt ((SQLHDBC) vcn->hdbc, &hstmt);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        free (sql);
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, vcn->hdbc, NULL,
                                   err);
    }

    sr = SQLPrepare (hstmt, (SQLCHAR *) sql, SQL_NTS);
    free (sql);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        rc = virt_err_from_odbc (ADBC_STATUS_NOT_FOUND, NULL, NULL, hstmt, err);
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }

    sr = virtodbc__SQLNumResultCols (hstmt, &ncols);
    if ((sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) || ncols <= 0) {
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return virt_err_set (err, ADBC_STATUS_NOT_FOUND, NULL, 0,
                             "GetTableSchema: '%s' has no columns",
                             table_name);
    }

    ArrowSchemaInit (out);
    if (ArrowSchemaSetTypeStruct (out, ncols) != 0) {
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "SetTypeStruct failed");
    }

    for (i = 0; i < ncols; i++) {
        SQLCHAR colname[256];
        SQLSMALLINT cbname = 0, sqltype = 0, scale = 0, nullable = 0;
        SQLULEN precision = 0;
        enum ArrowType atype;
        int fallback = 0;

        colname[0] = 0;
        sr = virtodbc__SQLDescribeCol (hstmt, (SQLUSMALLINT) (i + 1),
                                       colname, sizeof (colname),
                                       &cbname, &sqltype, &precision, &scale,
                                       &nullable);
        if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
            if (out->release) out->release (out);
            virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
            return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL, hstmt,
                                       err);
        }
        atype = virt_sql_to_arrow_type ((int) sqltype, &fallback);
        if (atype == NANOARROW_TYPE_TIMESTAMP)
            ArrowSchemaSetTypeDateTime (out->children[i],
                                        NANOARROW_TYPE_TIMESTAMP,
                                        NANOARROW_TIME_UNIT_MICRO, NULL);
        else if (atype == NANOARROW_TYPE_TIME64)
            ArrowSchemaSetTypeDateTime (out->children[i],
                                        NANOARROW_TYPE_TIME64,
                                        NANOARROW_TIME_UNIT_MICRO, NULL);
        else
            ArrowSchemaSetType (out->children[i], atype);
        ArrowSchemaSetName (out->children[i],
                            (const char *) (colname[0] ? colname
                                                       : (SQLCHAR *) ""));
        if (!nullable)
            out->children[i]->flags &= ~ARROW_FLAG_NULLABLE;
    }

    virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
    return ADBC_STATUS_OK;
}

/* ====================================================================
 *   GetObjects  (phase 6d)
 *
 *   In-memory representation we accumulate while walking SQLTables
 *   and SQLColumns. After everything is fetched we materialise the
 *   nested Arrow result in one pass.
 * ==================================================================== */

typedef struct cat_col {
    char *name;
    int32_t  ordinal;
    int16_t  xdbc_data_type;
    int16_t  xdbc_nullable;
    int32_t  xdbc_column_size;
    int16_t  xdbc_decimal_digits;
    char    *xdbc_type_name;
} cat_col_t;

typedef struct cat_tab {
    char     *name;
    char     *type;
    cat_col_t *cols;
    int       ncols;
    int       cols_cap;
} cat_tab_t;

typedef struct cat_sch {
    char    *name;
    cat_tab_t *tabs;
    int      ntabs;
    int      tabs_cap;
} cat_sch_t;

typedef struct cat_cat {
    char     *name;
    cat_sch_t *schs;
    int       nschs;
    int       schs_cap;
} cat_cat_t;

typedef struct cat_root {
    cat_cat_t *cats;
    int        ncats;
    int        cats_cap;
} cat_root_t;

static char *
dup_or_empty (const char *s)
{
    return strdup (s ? s : "");
}

static cat_cat_t *
cat_get_or_add (cat_root_t *r, const char *name)
{
    int i;
    for (i = 0; i < r->ncats; i++)
        if (strcmp (r->cats[i].name, name ? name : "") == 0)
            return &r->cats[i];
    if (r->ncats == r->cats_cap) {
        int nc = r->cats_cap ? r->cats_cap * 2 : 4;
        cat_cat_t *nb = realloc (r->cats, (size_t) nc * sizeof (*nb));
        if (!nb) return NULL;
        r->cats = nb; r->cats_cap = nc;
    }
    memset (&r->cats[r->ncats], 0, sizeof (r->cats[r->ncats]));
    r->cats[r->ncats].name = dup_or_empty (name);
    return &r->cats[r->ncats++];
}

static cat_sch_t *
sch_get_or_add (cat_cat_t *c, const char *name)
{
    int i;
    for (i = 0; i < c->nschs; i++)
        if (strcmp (c->schs[i].name, name ? name : "") == 0)
            return &c->schs[i];
    if (c->nschs == c->schs_cap) {
        int nc = c->schs_cap ? c->schs_cap * 2 : 4;
        cat_sch_t *nb = realloc (c->schs, (size_t) nc * sizeof (*nb));
        if (!nb) return NULL;
        c->schs = nb; c->schs_cap = nc;
    }
    memset (&c->schs[c->nschs], 0, sizeof (c->schs[c->nschs]));
    c->schs[c->nschs].name = dup_or_empty (name);
    return &c->schs[c->nschs++];
}

static cat_tab_t *
tab_add (cat_sch_t *s, const char *name, const char *type)
{
    if (s->ntabs == s->tabs_cap) {
        int nc = s->tabs_cap ? s->tabs_cap * 2 : 8;
        cat_tab_t *nb = realloc (s->tabs, (size_t) nc * sizeof (*nb));
        if (!nb) return NULL;
        s->tabs = nb; s->tabs_cap = nc;
    }
    memset (&s->tabs[s->ntabs], 0, sizeof (s->tabs[s->ntabs]));
    s->tabs[s->ntabs].name = dup_or_empty (name);
    s->tabs[s->ntabs].type = dup_or_empty (type);
    return &s->tabs[s->ntabs++];
}

static int
col_add (cat_tab_t *t, const cat_col_t *src)
{
    if (t->ncols == t->cols_cap) {
        int nc = t->cols_cap ? t->cols_cap * 2 : 8;
        cat_col_t *nb = realloc (t->cols, (size_t) nc * sizeof (*nb));
        if (!nb) return -1;
        t->cols = nb; t->cols_cap = nc;
    }
    t->cols[t->ncols] = *src;
    t->ncols++;
    return 0;
}

static void
cat_root_free (cat_root_t *r)
{
    int i, j, k, n;
    for (i = 0; i < r->ncats; i++) {
        for (j = 0; j < r->cats[i].nschs; j++) {
            for (k = 0; k < r->cats[i].schs[j].ntabs; k++) {
                for (n = 0; n < r->cats[i].schs[j].tabs[k].ncols; n++) {
                    free (r->cats[i].schs[j].tabs[k].cols[n].name);
                    free (r->cats[i].schs[j].tabs[k].cols[n].xdbc_type_name);
                }
                free (r->cats[i].schs[j].tabs[k].cols);
                free (r->cats[i].schs[j].tabs[k].name);
                free (r->cats[i].schs[j].tabs[k].type);
            }
            free (r->cats[i].schs[j].tabs);
            free (r->cats[i].schs[j].name);
        }
        free (r->cats[i].schs);
        free (r->cats[i].name);
    }
    free (r->cats);
    memset (r, 0, sizeof (*r));
}

/* Fetch a single VARCHAR column as a malloc'd string. NULL on
 * SQL_NULL_DATA or fetch error.                                       */
static char *
fetch_string_col (SQLHSTMT hstmt, int icol)
{
    char buf[1024];
    SQLLEN pcb = 0;
    SQLRETURN sr =
        virtodbc__SQLGetData (hstmt, (SQLUSMALLINT) icol, SQL_C_CHAR, buf,
                              sizeof (buf), &pcb);
    if ((sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) || pcb == SQL_NULL_DATA)
        return NULL;
    return strdup (buf);
}

static int
fetch_smallint_col (SQLHSTMT hstmt, int icol, int16_t *out)
{
    SQLSMALLINT v = 0;
    SQLLEN pcb = 0;
    SQLRETURN sr =
        virtodbc__SQLGetData (hstmt, (SQLUSMALLINT) icol, SQL_C_SHORT, &v,
                              sizeof (v), &pcb);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) return -1;
    *out = (pcb == SQL_NULL_DATA) ? 0 : (int16_t) v;
    return 0;
}

static int
fetch_int_col (SQLHSTMT hstmt, int icol, int32_t *out)
{
    SQLINTEGER v = 0;
    SQLLEN pcb = 0;
    SQLRETURN sr =
        virtodbc__SQLGetData (hstmt, (SQLUSMALLINT) icol, SQL_C_LONG, &v,
                              sizeof (v), &pcb);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) return -1;
    *out = (pcb == SQL_NULL_DATA) ? 0 : (int32_t) v;
    return 0;
}

/* ---- Walk SQLTables and bucket rows by (catalog, schema). ---------- */

static AdbcStatusCode
walk_tables (SQLHDBC hdbc, const char *catalog, const char *db_schema,
             const char *table_name, const char *const *table_type,
             cat_root_t *root, struct AdbcError *err)
{
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    SQLRETURN sr;
    char  type_filter[256] = { 0 };
    AdbcStatusCode rc = ADBC_STATUS_OK;

    sr = virtodbc__SQLAllocStmt (hdbc, &hstmt);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, hdbc, NULL, err);

    /* Build comma-separated table types if filter passed.            */
    if (table_type) {
        size_t off = 0;
        int i;
        for (i = 0; table_type[i] != NULL; i++) {
            size_t l = strlen (table_type[i]);
            if (off + l + 2 >= sizeof (type_filter)) break;
            if (off) type_filter[off++] = ',';
            memcpy (type_filter + off, table_type[i], l);
            off += l;
        }
        type_filter[off] = 0;
    }

    sr = SQLTables (hstmt,
                    catalog   ? (SQLCHAR *) catalog   : NULL,
                    catalog   ? SQL_NTS              : 0,
                    db_schema ? (SQLCHAR *) db_schema : NULL,
                    db_schema ? SQL_NTS              : 0,
                    table_name? (SQLCHAR *) table_name: NULL,
                    table_name? SQL_NTS              : 0,
                    type_filter[0] ? (SQLCHAR *) type_filter : NULL,
                    type_filter[0] ? SQL_NTS         : 0);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        rc = virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, NULL, hstmt, err);
        virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
        return rc;
    }

    while (virtodbc__SQLFetch (hstmt) == SQL_SUCCESS) {
        char *cat  = fetch_string_col (hstmt, 1);
        char *sch  = fetch_string_col (hstmt, 2);
        char *nm   = fetch_string_col (hstmt, 3);
        char *typ  = fetch_string_col (hstmt, 4);
        cat_cat_t *c;
        cat_sch_t *s;

        if (!nm) {
            free (cat); free (sch); free (typ);
            continue;
        }
        c = cat_get_or_add (root, cat ? cat : "");
        s = c ? sch_get_or_add (c, sch ? sch : "") : NULL;
        if (s) tab_add (s, nm, typ ? typ : "TABLE");
        free (cat); free (sch); free (nm); free (typ);
    }
    virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
    return rc;
}

/* For each accumulated table, ask SQLColumns about its columns. */
static AdbcStatusCode
walk_columns (SQLHDBC hdbc, cat_root_t *root, const char *column_name,
              struct AdbcError *err)
{
    int i, j, k;
    AdbcStatusCode rc = ADBC_STATUS_OK;

    for (i = 0; i < root->ncats; i++) {
        for (j = 0; j < root->cats[i].nschs; j++) {
            for (k = 0; k < root->cats[i].schs[j].ntabs; k++) {
                SQLHSTMT hstmt = SQL_NULL_HSTMT;
                SQLRETURN sr;
                const char *cat = root->cats[i].name;
                const char *sch = root->cats[i].schs[j].name;
                const char *tab = root->cats[i].schs[j].tabs[k].name;

                sr = virtodbc__SQLAllocStmt (hdbc, &hstmt);
                if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
                    rc = virt_err_from_odbc (ADBC_STATUS_INTERNAL, NULL, hdbc,
                                             NULL, err);
                    return rc;
                }
                sr = SQLColumns (hstmt,
                                 *cat ? (SQLCHAR *) cat : NULL,
                                 *cat ? SQL_NTS : 0,
                                 *sch ? (SQLCHAR *) sch : NULL,
                                 *sch ? SQL_NTS : 0,
                                 (SQLCHAR *) tab, SQL_NTS,
                                 column_name ? (SQLCHAR *) column_name : NULL,
                                 column_name ? SQL_NTS : 0);
                if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
                    /* Soft failure -- some Virtuoso views don't return
                     * column metadata; leave columns empty. */
                    virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
                    continue;
                }
                while (virtodbc__SQLFetch (hstmt) == SQL_SUCCESS) {
                    cat_col_t col;
                    memset (&col, 0, sizeof (col));
                    col.name = fetch_string_col (hstmt, 4);
                    if (!col.name) continue;
                    col.xdbc_type_name = fetch_string_col (hstmt, 6);
                    {
                        int16_t dt = 0; int32_t cs = 0;
                        int16_t dd = 0; int16_t nbl = 0;
                        int32_t ord = 0;
                        fetch_smallint_col (hstmt, 5, &dt);
                        fetch_int_col      (hstmt, 7, &cs);
                        fetch_smallint_col (hstmt, 9, &dd);
                        fetch_smallint_col (hstmt, 11, &nbl);
                        fetch_int_col      (hstmt, 17, &ord);
                        col.xdbc_data_type      = dt;
                        col.xdbc_column_size    = cs;
                        col.xdbc_decimal_digits = dd;
                        col.xdbc_nullable       = nbl;
                        col.ordinal             = ord;
                    }
                    col_add (&root->cats[i].schs[j].tabs[k], &col);
                }
                virtodbc__SQLFreeStmt (hstmt, SQL_DROP);
            }
        }
    }
    return rc;
}

/* ---- Schema construction for the GetObjects nested struct ---------- */

static int
column_schema_init (struct ArrowSchema *s)
{
    /* COLUMN_SCHEMA */
    if (ArrowSchemaSetTypeStruct (s, 19) != 0) return -1;
#define COL(idx, name, type) do {                                       \
        if (ArrowSchemaSetType (s->children[idx], type) != 0) return -1;\
        if (ArrowSchemaSetName (s->children[idx], name) != 0) return -1;\
    } while (0)
    COL (0,  "column_name",            NANOARROW_TYPE_STRING);
    s->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    COL (1,  "ordinal_position",       NANOARROW_TYPE_INT32);
    COL (2,  "remarks",                NANOARROW_TYPE_STRING);
    COL (3,  "xdbc_data_type",         NANOARROW_TYPE_INT16);
    COL (4,  "xdbc_type_name",         NANOARROW_TYPE_STRING);
    COL (5,  "xdbc_column_size",       NANOARROW_TYPE_INT32);
    COL (6,  "xdbc_decimal_digits",    NANOARROW_TYPE_INT16);
    COL (7,  "xdbc_num_prec_radix",    NANOARROW_TYPE_INT16);
    COL (8,  "xdbc_nullable",          NANOARROW_TYPE_INT16);
    COL (9,  "xdbc_column_def",        NANOARROW_TYPE_STRING);
    COL (10, "xdbc_sql_data_type",     NANOARROW_TYPE_INT16);
    COL (11, "xdbc_datetime_sub",      NANOARROW_TYPE_INT16);
    COL (12, "xdbc_char_octet_length", NANOARROW_TYPE_INT32);
    COL (13, "xdbc_is_nullable",       NANOARROW_TYPE_STRING);
    COL (14, "xdbc_scope_catalog",     NANOARROW_TYPE_STRING);
    COL (15, "xdbc_scope_schema",      NANOARROW_TYPE_STRING);
    COL (16, "xdbc_scope_table",       NANOARROW_TYPE_STRING);
    COL (17, "xdbc_is_autoincrement",  NANOARROW_TYPE_BOOL);
    COL (18, "xdbc_is_generatedcolumn",NANOARROW_TYPE_BOOL);
#undef COL
    return 0;
}

static int
constraint_schema_init (struct ArrowSchema *s)
{
    /* CONSTRAINT_SCHEMA; we leave the lists empty but the shape must
     * still be present for the spec to be honoured.                  */
    struct ArrowSchema *usage;
    if (ArrowSchemaSetTypeStruct (s, 4) != 0) return -1;
    if (ArrowSchemaSetType (s->children[0], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (s->children[0], "constraint_name") != 0)
        return -1;
    if (ArrowSchemaSetType (s->children[1], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (s->children[1], "constraint_type") != 0)
        return -1;
    s->children[1]->flags &= ~ARROW_FLAG_NULLABLE;
    if (ArrowSchemaSetType (s->children[2], NANOARROW_TYPE_LIST) != 0
        || ArrowSchemaSetName (s->children[2], "constraint_column_names") != 0
        || ArrowSchemaSetType (s->children[2]->children[0],
                               NANOARROW_TYPE_STRING) != 0)
        return -1;
    s->children[2]->flags &= ~ARROW_FLAG_NULLABLE;
    if (ArrowSchemaSetType (s->children[3], NANOARROW_TYPE_LIST) != 0
        || ArrowSchemaSetName (s->children[3], "constraint_column_usage") != 0)
        return -1;
    usage = s->children[3]->children[0];
    if (ArrowSchemaSetTypeStruct (usage, 4) != 0) return -1;
    if (ArrowSchemaSetType (usage->children[0], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (usage->children[0], "fk_catalog") != 0)
        return -1;
    if (ArrowSchemaSetType (usage->children[1], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (usage->children[1], "fk_db_schema") != 0)
        return -1;
    if (ArrowSchemaSetType (usage->children[2], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (usage->children[2], "fk_table") != 0)
        return -1;
    usage->children[2]->flags &= ~ARROW_FLAG_NULLABLE;
    if (ArrowSchemaSetType (usage->children[3], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (usage->children[3], "fk_column_name") != 0)
        return -1;
    usage->children[3]->flags &= ~ARROW_FLAG_NULLABLE;
    return 0;
}

static int
table_schema_init (struct ArrowSchema *s)
{
    if (ArrowSchemaSetTypeStruct (s, 4) != 0) return -1;
    if (ArrowSchemaSetType (s->children[0], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (s->children[0], "table_name") != 0)
        return -1;
    s->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    if (ArrowSchemaSetType (s->children[1], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (s->children[1], "table_type") != 0)
        return -1;
    s->children[1]->flags &= ~ARROW_FLAG_NULLABLE;
    if (ArrowSchemaSetType (s->children[2], NANOARROW_TYPE_LIST) != 0
        || ArrowSchemaSetName (s->children[2], "table_columns") != 0)
        return -1;
    if (column_schema_init (s->children[2]->children[0]) != 0) return -1;
    if (ArrowSchemaSetType (s->children[3], NANOARROW_TYPE_LIST) != 0
        || ArrowSchemaSetName (s->children[3], "table_constraints") != 0)
        return -1;
    if (constraint_schema_init (s->children[3]->children[0]) != 0) return -1;
    return 0;
}

static int
db_schema_schema_init (struct ArrowSchema *s)
{
    if (ArrowSchemaSetTypeStruct (s, 2) != 0) return -1;
    if (ArrowSchemaSetType (s->children[0], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (s->children[0], "db_schema_name") != 0)
        return -1;
    if (ArrowSchemaSetType (s->children[1], NANOARROW_TYPE_LIST) != 0
        || ArrowSchemaSetName (s->children[1], "db_schema_tables") != 0)
        return -1;
    return table_schema_init (s->children[1]->children[0]);
}

static int
get_objects_schema_init (struct ArrowSchema *schema)
{
    ArrowSchemaInit (schema);
    if (ArrowSchemaSetTypeStruct (schema, 2) != 0) return -1;
    if (ArrowSchemaSetType (schema->children[0], NANOARROW_TYPE_STRING) != 0
        || ArrowSchemaSetName (schema->children[0], "catalog_name") != 0)
        return -1;
    if (ArrowSchemaSetType (schema->children[1], NANOARROW_TYPE_LIST) != 0
        || ArrowSchemaSetName (schema->children[1], "catalog_db_schemas") != 0)
        return -1;
    return db_schema_schema_init (schema->children[1]->children[0]);
}

/* ---- Materialisation into an ArrowArray ---------------------------- */

static void
append_column_row (struct ArrowArray *col_arr, const cat_col_t *col)
{
    /* The ordering matches column_schema_init. */
    ArrowArrayAppendString (col_arr->children[0],  ArrowCharView (col->name));
    ArrowArrayAppendInt    (col_arr->children[1],  col->ordinal);
    ArrowArrayAppendNull   (col_arr->children[2],  1);            /* remarks       */
    ArrowArrayAppendInt    (col_arr->children[3],  col->xdbc_data_type);
    if (col->xdbc_type_name)
        ArrowArrayAppendString (col_arr->children[4],
                                ArrowCharView (col->xdbc_type_name));
    else
        ArrowArrayAppendNull   (col_arr->children[4], 1);
    ArrowArrayAppendInt    (col_arr->children[5],  col->xdbc_column_size);
    ArrowArrayAppendInt    (col_arr->children[6],  col->xdbc_decimal_digits);
    ArrowArrayAppendNull   (col_arr->children[7],  1);            /* prec radix    */
    ArrowArrayAppendInt    (col_arr->children[8],  col->xdbc_nullable);
    ArrowArrayAppendNull   (col_arr->children[9],  1);            /* column_def    */
    ArrowArrayAppendNull   (col_arr->children[10], 1);            /* sql_data_type */
    ArrowArrayAppendNull   (col_arr->children[11], 1);            /* dt sub        */
    ArrowArrayAppendNull   (col_arr->children[12], 1);            /* char_octet    */
    ArrowArrayAppendString (col_arr->children[13],
                            ArrowCharView (col->xdbc_nullable ? "YES" : "NO"));
    ArrowArrayAppendNull   (col_arr->children[14], 1);
    ArrowArrayAppendNull   (col_arr->children[15], 1);
    ArrowArrayAppendNull   (col_arr->children[16], 1);
    ArrowArrayAppendNull   (col_arr->children[17], 1);
    ArrowArrayAppendNull   (col_arr->children[18], 1);
    ArrowArrayFinishElement (col_arr);
}

static AdbcStatusCode
materialise_objects (cat_root_t *root, int depth,
                     struct ArrowSchema *schema, struct ArrowArray *array,
                     struct AdbcError *err)
{
    struct ArrowError ae;
    int i, j, k, n;

    memset (&ae, 0, sizeof (ae));
    if (ArrowArrayInitFromSchema (array, schema, &ae) != 0)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "GetObjects array init: %s",
                             ae.message[0] ? ae.message : "");
    ArrowArrayStartAppending (array);

    for (i = 0; i < root->ncats; i++) {
        struct ArrowArray *schemas_list  = array->children[1];
        struct ArrowArray *schemas_items = schemas_list->children[0];

        ArrowArrayAppendString (array->children[0],
                                ArrowCharView (root->cats[i].name));

        if (depth == ADBC_OBJECT_DEPTH_CATALOGS) {
            ArrowArrayAppendNull (schemas_list, 1);
            ArrowArrayFinishElement (array);
            continue;
        }

        for (j = 0; j < root->cats[i].nschs; j++) {
            struct ArrowArray *tables_list  = schemas_items->children[1];
            struct ArrowArray *tables_items = tables_list->children[0];

            ArrowArrayAppendString (schemas_items->children[0],
                                    ArrowCharView (
                                        root->cats[i].schs[j].name));

            if (depth == ADBC_OBJECT_DEPTH_DB_SCHEMAS) {
                ArrowArrayAppendNull (tables_list, 1);
            } else {
                for (k = 0; k < root->cats[i].schs[j].ntabs; k++) {
                    struct ArrowArray *cols_list  = tables_items->children[2];
                    struct ArrowArray *cons_list  = tables_items->children[3];
                    struct ArrowArray *cols_items = cols_list->children[0];

                    ArrowArrayAppendString (tables_items->children[0],
                        ArrowCharView (root->cats[i].schs[j].tabs[k].name));
                    ArrowArrayAppendString (tables_items->children[1],
                        ArrowCharView (root->cats[i].schs[j].tabs[k].type));

                    if (depth == ADBC_OBJECT_DEPTH_TABLES) {
                        ArrowArrayAppendNull (cols_list, 1);
                    } else {
                        for (n = 0; n < root->cats[i].schs[j].tabs[k].ncols; n++)
                            append_column_row (cols_items,
                                &root->cats[i].schs[j].tabs[k].cols[n]);
                        ArrowArrayFinishElement (cols_list);
                    }
                    /* Constraints: always empty list in this phase. */
                    ArrowArrayFinishElement (cons_list);
                    ArrowArrayFinishElement (tables_items);
                }
                ArrowArrayFinishElement (tables_list);
            }
            ArrowArrayFinishElement (schemas_items);
        }
        ArrowArrayFinishElement (schemas_list);
        ArrowArrayFinishElement (array);
    }

    if (ArrowArrayFinishBuildingDefault (array, &ae) != 0)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "GetObjects finish: %s",
                             ae.message[0] ? ae.message : "");
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_cn_get_objects (struct AdbcConnection *cn, int depth,
                     const char *catalog, const char *db_schema,
                     const char *table_name, const char **table_type,
                     const char *column_name,
                     struct ArrowArrayStream *out, struct AdbcError *err)
{
    VirtAdbcConnection *vcn;
    cat_root_t root;
    struct ArrowSchema schema;
    struct ArrowArray  array;
    AdbcStatusCode rc;

    if (!cn || !cn->private_data)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "GetObjects: connection not initialised");
    vcn = (VirtAdbcConnection *) cn->private_data;
    if (!vcn->connected)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "GetObjects: connection not open");
    if (!out)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "GetObjects: NULL out");

    memset (&root,   0, sizeof (root));
    memset (&schema, 0, sizeof (schema));
    memset (&array,  0, sizeof (array));

    /* depth=ALL (0) is the "drill all the way to columns" sentinel.
     * depth >= TABLES (3) plus the explicit catalogs/schemas-only
     * cases are handled below in materialise_objects.                 */
    rc = walk_tables ((SQLHDBC) vcn->hdbc, catalog, db_schema, table_name,
                      table_type, &root, err);
    if (rc != ADBC_STATUS_OK) goto fail;

    if (depth == ADBC_OBJECT_DEPTH_ALL) {
        rc = walk_columns ((SQLHDBC) vcn->hdbc, &root, column_name, err);
        if (rc != ADBC_STATUS_OK) goto fail;
    }

    if (get_objects_schema_init (&schema) != 0) {
        rc = virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                           "GetObjects: schema build failed");
        goto fail;
    }
    rc = materialise_objects (&root, depth, &schema, &array, err);
    if (rc != ADBC_STATUS_OK) goto fail;

    cat_root_free (&root);
    return publish_one_batch (&schema, &array, out, err);

fail:
    cat_root_free (&root);
    if (array.release)  array.release  (&array);
    if (schema.release) schema.release (&schema);
    return rc;
}
