/*
 *  adbc_entry.c
 *
 *  ADBC driver dispatch-table entrypoint for the Virtuoso ADBC driver.
 *
 *  Phase 1 stubbed every slot. Phases 2-5 populate the database,
 *  connection, and statement vtables (including Prepare, Bind, and
 *  BindStream) plus the error-detail hooks.
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the GNU GPL v2; see COPYING in the project root.
 */

#include <string.h>

#include "virtuoso_adbc.h"

#if defined(_WIN32)
#  define VIRT_ADBC_EXPORT __declspec (dllexport)
#else
#  define VIRT_ADBC_EXPORT __attribute__ ((visibility ("default")))
#endif

VIRT_ADBC_EXPORT
AdbcStatusCode
AdbcDriverInit (int version, void *raw_driver, struct AdbcError *error)
{
    struct AdbcDriver *driver;
    size_t sz;

    (void) error;

    if (raw_driver == NULL)
        return ADBC_STATUS_INVALID_ARGUMENT;

    switch (version) {
    case ADBC_VERSION_1_1_0:
        sz = ADBC_DRIVER_1_1_0_SIZE;
        break;
    case ADBC_VERSION_1_0_0:
        sz = ADBC_DRIVER_1_0_0_SIZE;
        break;
    default:
        return ADBC_STATUS_NOT_IMPLEMENTED;
    }

    driver = (struct AdbcDriver *) raw_driver;
    memset (driver, 0, sz);

    /* ----- AdbcDatabase (phase 2) ----- */
    driver->DatabaseNew       = virt_db_new;
    driver->DatabaseSetOption = virt_db_set_option;
    driver->DatabaseInit      = virt_db_init;
    driver->DatabaseRelease   = virt_db_release;

    /* ----- AdbcConnection (phase 3) ----- */
    driver->ConnectionNew       = virt_cn_new;
    driver->ConnectionSetOption = virt_cn_set_option;
    driver->ConnectionInit      = virt_cn_init;
    driver->ConnectionRelease   = virt_cn_release;
    driver->ConnectionCommit    = virt_cn_commit;
    driver->ConnectionRollback  = virt_cn_rollback;

    /* ----- AdbcStatement (phase 4) ----- */
    driver->StatementNew          = virt_st_new;
    driver->StatementRelease      = virt_st_release;
    driver->StatementSetSqlQuery  = virt_st_set_sql_query;
    driver->StatementSetOption    = virt_st_set_option;
    driver->StatementExecuteQuery = virt_st_execute_query;

    /* ----- AdbcStatement: prepared + parameter binding (phase 5) ----- */
    driver->StatementPrepare             = virt_st_prepare;
    driver->StatementBind                = virt_st_bind;
    driver->StatementBindStream          = virt_st_bind_stream;
    driver->StatementGetParameterSchema  = virt_st_get_parameter_schema;

    if (version >= ADBC_VERSION_1_1_0) {
        /* ----- ADBC 1.1.0 error detail ----- */
        driver->ErrorGetDetailCount = virt_err_detail_count;
        driver->ErrorGetDetail      = virt_err_detail_at;

        /* ----- Database typed options ----- */
        driver->DatabaseSetOptionBytes  = virt_db_set_option_bytes;
        driver->DatabaseSetOptionInt    = virt_db_set_option_int;
        driver->DatabaseSetOptionDouble = virt_db_set_option_double;
        driver->DatabaseGetOption       = virt_db_get_option;
        driver->DatabaseGetOptionBytes  = virt_db_get_option_bytes;
        driver->DatabaseGetOptionInt    = virt_db_get_option_int;
        driver->DatabaseGetOptionDouble = virt_db_get_option_double;

        /* ----- Connection typed options + cancel ----- */
        driver->ConnectionCancel             = virt_cn_cancel;
        driver->ConnectionSetOptionBytes     = virt_cn_set_option_bytes;
        driver->ConnectionSetOptionInt       = virt_cn_set_option_int;
        driver->ConnectionSetOptionDouble    = virt_cn_set_option_double;
        driver->ConnectionGetOption          = virt_cn_get_option;
        driver->ConnectionGetOptionBytes     = virt_cn_get_option_bytes;
        driver->ConnectionGetOptionInt       = virt_cn_get_option_int;
        driver->ConnectionGetOptionDouble    = virt_cn_get_option_double;

        /* ----- Statement typed options (phase 4) ----- */
        driver->StatementSetOptionBytes      = virt_st_set_option_bytes;
        driver->StatementSetOptionInt        = virt_st_set_option_int;
        driver->StatementSetOptionDouble     = virt_st_set_option_double;
        driver->StatementGetOption           = virt_st_get_option;
        driver->StatementGetOptionBytes      = virt_st_get_option_bytes;
        driver->StatementGetOptionInt        = virt_st_get_option_int;
        driver->StatementGetOptionDouble     = virt_st_get_option_double;
    }

    /* Bind/Prepare/GetParameterSchema slots remain NULL -- phase 5. */
    return ADBC_STATUS_OK;
}
