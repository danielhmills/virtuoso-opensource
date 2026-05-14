/*
 *  smoke_test.c
 *
 *  Smoke test for the Virtuoso ADBC driver dispatch table.
 *  Verifies that AdbcDriverInit accepts ADBC 1.0.0 and 1.1.0,
 *  rejects NULL drivers and unknown versions, and wires the
 *  correct set of slots for the current phase.
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
   *  After phase 2, AdbcDatabase slots must be wired; AdbcConnection
   *  and AdbcStatement slots remain NULL until phases 3 and 4. The
   *  assertions below are intentionally strict so any wiring that
   *  lands without updating this test fails loudly.
   */
  if (drv.DatabaseNew == NULL || drv.DatabaseInit == NULL
      || drv.DatabaseRelease == NULL || drv.DatabaseSetOption == NULL)
    {
      fprintf (stderr,
               "[FAIL] %s: required AdbcDatabase slot is NULL\n", label);
      return 1;
    }
  if (drv.ConnectionNew != NULL || drv.StatementNew != NULL)
    {
      fprintf (stderr,
               "[FAIL] %s: connection/statement slot populated before phase 3\n",
               label);
      return 1;
    }
  if (version >= ADBC_VERSION_1_1_0
      && (drv.DatabaseSetOptionInt == NULL
          || drv.DatabaseGetOption == NULL
          || drv.ErrorGetDetailCount == NULL))
    {
      fprintf (stderr,
               "[FAIL] %s: ADBC 1.1.0 database/error slot is NULL\n", label);
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
  fprintf (stdout, "OK -- ADBC smoke test passed\n");
  return EXIT_SUCCESS;
}
