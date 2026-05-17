/*
 *  type_map.c
 *
 *  SQL_TYPE_* (Virtuoso ODBC) -> ArrowSchema/nanoarrow type mapping,
 *  plus the matching SQL_C_* target codes for SQLGetData.
 *
 *  Phase 4d. The table is intentionally exhaustive for the standard
 *  ODBC types; Virtuoso-specific extras (IRI_ID, RDF struct, GEOMETRY)
 *  are handled in phase 8 and slot in here at that point.
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
#include <string.h>

#include "sql.h"
#include "sqlext.h"

#include <nanoarrow/nanoarrow.h>

#include "virtuoso_adbc.h"

/* ------------------------------------------------------------------ */
/*  virt_sql_to_arrow_type:                                            */
/*    Map an ODBC SQL_TYPE_* code (as returned by SQLDescribeCol) to   */
/*    a nanoarrow ArrowType enum. Returns NANOARROW_TYPE_STRING as a   */
/*    last-resort fallback so we never produce an uninitialised        */
/*    column schema; callers may choose to treat that as a hard error  */
/*    instead by checking *is_fallback.                                */
/* ------------------------------------------------------------------ */

enum ArrowType
virt_sql_to_arrow_type (int sql_type, int *is_fallback)
{
    if (is_fallback) *is_fallback = 0;

    switch (sql_type) {
    case SQL_BIT:
        return NANOARROW_TYPE_BOOL;
    case SQL_TINYINT:
        return NANOARROW_TYPE_INT8;
    case SQL_SMALLINT:
        return NANOARROW_TYPE_INT16;
    case SQL_INTEGER:
        return NANOARROW_TYPE_INT32;
    case SQL_BIGINT:
        return NANOARROW_TYPE_INT64;
    case SQL_REAL:
        return NANOARROW_TYPE_FLOAT;
    case SQL_FLOAT:
    case SQL_DOUBLE:
        return NANOARROW_TYPE_DOUBLE;
    case SQL_NUMERIC:
    case SQL_DECIMAL:
        /* DECIMAL/NUMERIC are surfaced as utf8 in phase 4 to avoid the
         * precision/scale gymnastics; phase 4d revisits this with proper
         * decimal128 once the rest of the column types are stable.    */
        if (is_fallback) *is_fallback = 1;
        return NANOARROW_TYPE_STRING;
    case SQL_CHAR:
    case SQL_VARCHAR:
        return NANOARROW_TYPE_STRING;
    case SQL_LONGVARCHAR:
        return NANOARROW_TYPE_LARGE_STRING;
    case SQL_BINARY:
    case SQL_VARBINARY:
        return NANOARROW_TYPE_BINARY;
    case SQL_LONGVARBINARY:
        return NANOARROW_TYPE_LARGE_BINARY;
    case SQL_DATE:
    case SQL_TYPE_DATE:
        return NANOARROW_TYPE_DATE32;
    case SQL_TIME:
    case SQL_TYPE_TIME:
        return NANOARROW_TYPE_TIME64;
    case SQL_TIMESTAMP:
    case SQL_TYPE_TIMESTAMP:
        return NANOARROW_TYPE_TIMESTAMP;
    default:
        /* Unknown / Virtuoso-specific (DV_IRI_ID etc) -- fall back to
         * utf8 so callers get a usable schema. Phase 8 specialises. */
        if (is_fallback) *is_fallback = 1;
        return NANOARROW_TYPE_STRING;
    }
}

/* ------------------------------------------------------------------ */
/*  virt_sql_to_c_type:                                                */
/*    The SQL_C_* target type to pass to SQLGetData for a given SQL    */
/*    column type. Choice is driven by how arrow_reader.c will append  */
/*    the value to the nanoarrow builder.                              */
/* ------------------------------------------------------------------ */

int
virt_sql_to_c_type (int sql_type)
{
    switch (sql_type) {
    case SQL_BIT:
    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_INTEGER:
        return SQL_C_SLONG;     /* 32-bit signed; widened on append */
    case SQL_BIGINT:
        return SQL_C_SBIGINT;
    case SQL_REAL:
        return SQL_C_FLOAT;
    case SQL_FLOAT:
    case SQL_DOUBLE:
        return SQL_C_DOUBLE;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return SQL_C_BINARY;
    case SQL_DATE:
    case SQL_TYPE_DATE:
        return SQL_C_TYPE_DATE;
    case SQL_TIME:
    case SQL_TYPE_TIME:
        return SQL_C_TYPE_TIME;
    case SQL_TIMESTAMP:
    case SQL_TYPE_TIMESTAMP:
        return SQL_C_TYPE_TIMESTAMP;
    default:
        /* String, decimal, unknown -- pull as text and let nanoarrow
         * dedup utf8.                                                 */
        return SQL_C_CHAR;
    }
}

/* ------------------------------------------------------------------ */
/*  virt_arrow_to_virtuoso_ddl: pick a Virtuoso SQL column type for an */
/*  Arrow type. Used by phase-7 bulk-ingest CREATE TABLE synthesis.    */
/*  Returns NULL for unsupported types (caller errors INVALID_DATA).   */
/* ------------------------------------------------------------------ */

const char *
virt_arrow_to_virtuoso_ddl (enum ArrowType atype)
{
    switch (atype) {
    case NANOARROW_TYPE_BOOL:           return "SMALLINT";
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:          return "SMALLINT";
    case NANOARROW_TYPE_INT32:          return "INTEGER";
    case NANOARROW_TYPE_INT64:          return "BIGINT";
    case NANOARROW_TYPE_FLOAT:          return "REAL";
    case NANOARROW_TYPE_DOUBLE:         return "DOUBLE PRECISION";
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:   return "VARCHAR";
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:   return "VARBINARY";
    case NANOARROW_TYPE_DATE32:         return "DATE";
    case NANOARROW_TYPE_TIME64:         return "TIME";
    case NANOARROW_TYPE_TIMESTAMP:      return "TIMESTAMP";
    default:                            return NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  virt_sql_type_name: human-readable label for diagnostics.          */
/* ------------------------------------------------------------------ */

const char *
virt_sql_type_name (int sql_type)
{
    switch (sql_type) {
    case SQL_BIT:               return "BIT";
    case SQL_TINYINT:           return "TINYINT";
    case SQL_SMALLINT:          return "SMALLINT";
    case SQL_INTEGER:           return "INTEGER";
    case SQL_BIGINT:            return "BIGINT";
    case SQL_REAL:              return "REAL";
    case SQL_FLOAT:             return "FLOAT";
    case SQL_DOUBLE:            return "DOUBLE";
    case SQL_NUMERIC:           return "NUMERIC";
    case SQL_DECIMAL:           return "DECIMAL";
    case SQL_CHAR:              return "CHAR";
    case SQL_VARCHAR:           return "VARCHAR";
    case SQL_LONGVARCHAR:       return "LONGVARCHAR";
    case SQL_BINARY:            return "BINARY";
    case SQL_VARBINARY:         return "VARBINARY";
    case SQL_LONGVARBINARY:     return "LONGVARBINARY";
    case SQL_DATE:
    case SQL_TYPE_DATE:         return "DATE";
    case SQL_TIME:
    case SQL_TYPE_TIME:         return "TIME";
    case SQL_TIMESTAMP:
    case SQL_TYPE_TIMESTAMP:    return "TIMESTAMP";
    default:                    return "UNKNOWN";
    }
}
