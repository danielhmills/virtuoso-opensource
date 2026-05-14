/*
 *  adbc_entry.c
 *
 *  ADBC driver dispatch-table entrypoint for the Virtuoso ADBC driver.
 *
 *  Phase 1 stubbed every slot. Phase 2 wires up the AdbcDatabase
 *  vtable plus the ADBC 1.1.0 typed Database Get/Set option family
 *  and the error-detail hooks. Connection and statement slots remain
 *  NULL until phases 3 and 4.
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
    }

    /* Connection and statement slots remain NULL -- phases 3 and 4. */
    return ADBC_STATUS_OK;
}
