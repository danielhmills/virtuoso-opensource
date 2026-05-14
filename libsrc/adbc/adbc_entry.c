/*
 *  adbc_entry.c
 *
 *  ADBC driver dispatch-table entrypoint for the Virtuoso ADBC driver.
 *  Phase 1: returns ADBC_STATUS_OK, leaves all function pointers NULL.
 *  Subsequent phases populate database/connection/statement slots from
 *  the respective translation units.
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

/*
 *  Canonical ADBC entrypoint. The arrow-adbc driver manager looks up
 *  "AdbcDriverInit" by default; this symbol must be exported.
 */
VIRT_ADBC_EXPORT
AdbcStatusCode
AdbcDriverInit (int version, void *raw_driver, struct AdbcError *error)
{
  struct AdbcDriver *driver;
  size_t sz;

  (void) error;

  if (raw_driver == NULL)
    return ADBC_STATUS_INVALID_ARGUMENT;

  switch (version)
    {
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

  /*
   *  Function pointers remain NULL for now. The driver manager treats
   *  NULL slots as "not implemented" and synthesises ADBC_STATUS_NOT_IMPLEMENTED
   *  on the caller's behalf. Real implementations land in phases 2-9.
   */

  return ADBC_STATUS_OK;
}
