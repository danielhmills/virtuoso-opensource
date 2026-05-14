/*
 *  smoke_test.c
 *
 *  Phase 1 smoke test for the Virtuoso ADBC driver.
 *  Calls AdbcDriverInit directly (linked, not dlopen) and verifies
 *  that the driver returns ADBC_STATUS_OK for both ADBC 1.0.0 and
 *  1.1.0, and ADBC_STATUS_NOT_IMPLEMENTED for an unknown version.
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the GNU GPL v2; see COPYING in the project root.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adbc.h"

extern AdbcStatusCode AdbcDriverInit (int version, void *driver,
                                      struct AdbcError *error);

static int
case_invalid_null_driver (void)
{
  struct AdbcError err;
  AdbcStatusCode rc;

  memset (&err, 0, sizeof (err));
  rc = AdbcDriverInit (ADBC_VERSION_1_1_0, NULL, &err);
  if (rc != ADBC_STATUS_INVALID_ARGUMENT)
    {
      fprintf (stderr, "[FAIL] NULL driver: expected INVALID_ARGUMENT, got %d\n",
               (int) rc);
      return 1;
    }
  return 0;
}

static int
case_unknown_version (void)
{
  struct AdbcDriver drv;
  struct AdbcError err;
  AdbcStatusCode rc;

  memset (&drv, 0, sizeof (drv));
  memset (&err, 0, sizeof (err));
  rc = AdbcDriverInit (0xdead, &drv, &err);
  if (rc != ADBC_STATUS_NOT_IMPLEMENTED)
    {
      fprintf (stderr, "[FAIL] unknown version: expected NOT_IMPLEMENTED, got %d\n",
               (int) rc);
      return 1;
    }
  return 0;
}

static int
case_version (int version, const char *label)
{
  struct AdbcDriver drv;
  struct AdbcError err;
  AdbcStatusCode rc;

  memset (&drv, 0, sizeof (drv));
  memset (&err, 0, sizeof (err));
  rc = AdbcDriverInit (version, &drv, &err);
  if (rc != ADBC_STATUS_OK)
    {
      fprintf (stderr, "[FAIL] %s: expected OK, got %d\n", label, (int) rc);
      return 1;
    }
  /*
   *  Phase 1: all dispatch slots must still be NULL. A populated slot at
   *  this stage would mean a phase-2+ change leaked in without updating
   *  the smoke test, so fail loudly.
   */
  if (drv.DatabaseNew != NULL
      || drv.ConnectionNew != NULL
      || drv.StatementNew != NULL)
    {
      fprintf (stderr,
               "[FAIL] %s: dispatch slot populated in phase 1\n", label);
      return 1;
    }
  return 0;
}

int
main (void)
{
  int failed = 0;

  failed += case_invalid_null_driver ();
  failed += case_unknown_version ();
  failed += case_version (ADBC_VERSION_1_0_0, "ADBC 1.0.0");
  failed += case_version (ADBC_VERSION_1_1_0, "ADBC 1.1.0");

  if (failed)
    {
      fprintf (stderr, "%d smoke test case(s) failed\n", failed);
      return EXIT_FAILURE;
    }
  fprintf (stdout, "OK -- ADBC phase 1 smoke test passed\n");
  return EXIT_SUCCESS;
}
