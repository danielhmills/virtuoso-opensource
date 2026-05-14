/*
 *  unit_test.c
 *
 *  Phase 2 unit tests for the Virtuoso ADBC driver.
 *
 *  Covers the option store, URI -> connection-string parsing, and
 *  the AdbcDatabase lifecycle state machine (errors on use-after-init,
 *  use-after-release, missing host on init, etc).
 *
 *  None of these tests need a live Virtuoso server. Tests that open
 *  a real CLI connection land in phase 3.
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
#include "virtuoso_adbc.h"

static int g_failures = 0;

#define ASSERT(cond, label)                                          \
    do {                                                             \
        if (!(cond)) {                                               \
            fprintf (stderr, "[FAIL] %s: %s (line %d)\n",            \
                     (label), #cond, __LINE__);                      \
            g_failures++;                                            \
        }                                                            \
    } while (0)

extern AdbcStatusCode AdbcDriverInit (int version, void *driver,
                                      struct AdbcError *error);

/* ---------- helpers ---------- */

static void
load_driver (struct AdbcDriver *drv)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    AdbcStatusCode rc = AdbcDriverInit (ADBC_VERSION_1_1_0, drv, &err);
    if (rc != ADBC_STATUS_OK) {
        fprintf (stderr, "FATAL: AdbcDriverInit failed (rc=%d)\n", (int) rc);
        exit (1);
    }
    if (err.release) err.release (&err);
}

/* ---------- options storage ---------- */

static void
test_options_store (void)
{
    virt_adbc_option_t *head = NULL;
    struct AdbcError err = ADBC_ERROR_INIT;
    char  sbuf[64];
    size_t slen;
    int64_t ival;
    double  dval;
    AdbcStatusCode rc;

    /* set/get string */
    ASSERT (virt_opt_set_string (&head, "k1", "hello", &err) == ADBC_STATUS_OK,
            "set string");
    slen = sizeof (sbuf);
    ASSERT (virt_opt_get_string (head, "k1", sbuf, &slen, &err) == ADBC_STATUS_OK,
            "get string");
    ASSERT (strcmp (sbuf, "hello") == 0, "get string roundtrip");
    ASSERT (slen == strlen ("hello") + 1, "get string returns required size");

    /* size-probe: out=NULL, *len=0 -> populates *len */
    slen = 0;
    ASSERT (virt_opt_get_string (head, "k1", NULL, &slen, &err) == ADBC_STATUS_OK,
            "size probe");
    ASSERT (slen == strlen ("hello") + 1, "size probe value");

    /* overwrite is allowed and frees old value */
    ASSERT (virt_opt_set_string (&head, "k1", "goodbye", &err) == ADBC_STATUS_OK,
            "overwrite string");
    slen = sizeof (sbuf);
    virt_opt_get_string (head, "k1", sbuf, &slen, &err);
    ASSERT (strcmp (sbuf, "goodbye") == 0, "overwrite roundtrip");

    /* int + double */
    ASSERT (virt_opt_set_int (&head, "k2", 42, &err) == ADBC_STATUS_OK,
            "set int");
    ASSERT (virt_opt_get_int (head, "k2", &ival, &err) == ADBC_STATUS_OK,
            "get int");
    ASSERT (ival == 42, "int value");
    ASSERT (virt_opt_set_double (&head, "k3", 3.14, &err) == ADBC_STATUS_OK,
            "set double");
    ASSERT (virt_opt_get_double (head, "k3", &dval, &err) == ADBC_STATUS_OK,
            "get double");
    ASSERT (dval == 3.14, "double value");

    /* missing key returns NOT_FOUND */
    rc = virt_opt_get_string (head, "nope", sbuf, &slen, &err);
    ASSERT (rc == ADBC_STATUS_NOT_FOUND, "missing key");
    if (err.release) err.release (&err);

    /* type mismatch returns NOT_FOUND */
    slen = sizeof (sbuf);
    rc = virt_opt_get_string (head, "k2", sbuf, &slen, &err);
    ASSERT (rc == ADBC_STATUS_NOT_FOUND, "type mismatch");
    if (err.release) err.release (&err);

    virt_opt_free_all (&head);
    ASSERT (head == NULL, "free_all clears head");
}

/* ---------- URI -> connection string ---------- */

static void
test_uri_parser (void)
{
    virt_adbc_option_t *opts = NULL;
    struct AdbcError err = ADBC_ERROR_INIT;
    char *out = NULL;

    /* Full URI: scheme, user, pass, host:port */
    virt_opt_set_string (&opts, "uri",
                         "virtuoso://dba:secret@localhost:1111", &err);
    ASSERT (virt_build_connstr (opts, &out, &err) == ADBC_STATUS_OK,
            "build connstr from full URI");
    ASSERT (out && strstr (out, "HOST=localhost:1111;") != NULL,
            "connstr contains HOST=");
    ASSERT (out && strstr (out, "UID=dba;") != NULL,
            "connstr contains UID=");
    ASSERT (out && strstr (out, "PWD=secret;") != NULL,
            "connstr contains PWD=");
    free (out); out = NULL;

    /* Explicit options override URI components. */
    virt_opt_set_string (&opts, "username", "joe", &err);
    virt_opt_set_string (&opts, "password", "shhh", &err);
    ASSERT (virt_build_connstr (opts, &out, &err) == ADBC_STATUS_OK,
            "build connstr with override");
    ASSERT (out && strstr (out, "UID=joe;") != NULL,
            "explicit user wins");
    ASSERT (out && strstr (out, "PWD=shhh;") != NULL,
            "explicit pass wins");
    free (out); out = NULL;
    virt_opt_free_all (&opts);

    /* URI without scheme, no creds. */
    virt_opt_set_string (&opts, "uri", "192.168.1.5:1234", &err);
    ASSERT (virt_build_connstr (opts, &out, &err) == ADBC_STATUS_OK,
            "schemeless URI");
    ASSERT (out && strstr (out, "HOST=192.168.1.5:1234;") != NULL,
            "schemeless host");
    ASSERT (out && strstr (out, "UID=") == NULL,
            "no UID when no creds");
    free (out); out = NULL;
    virt_opt_free_all (&opts);

    /* No URI at all -> error. */
    {
        struct AdbcError err2 = ADBC_ERROR_INIT;
        AdbcStatusCode rc = virt_build_connstr (opts, &out, &err2);
        ASSERT (rc == ADBC_STATUS_INVALID_ARGUMENT, "missing uri error");
        if (err2.release) err2.release (&err2);
    }

    if (err.release) err.release (&err);
}

/* ---------- AdbcDatabase: read-only state checks ---------- */

static void
test_database_lifecycle (void)
{
    struct AdbcDriver drv;
    struct AdbcDatabase db;
    struct AdbcError err = ADBC_ERROR_INIT;
    AdbcStatusCode rc;

    load_driver (&drv);

    memset (&db, 0, sizeof (db));

    /* SetOption before New should fail (private_data is NULL). */
    rc = drv.DatabaseSetOption (&db, "uri", "anything", &err);
    ASSERT (rc == ADBC_STATUS_INVALID_STATE, "set before new fails");
    if (err.release) err.release (&err);

    /* New twice should fail. */
    ASSERT (drv.DatabaseNew (&db, &err) == ADBC_STATUS_OK, "first new");
    rc = drv.DatabaseNew (&db, &err);
    ASSERT (rc == ADBC_STATUS_INVALID_STATE, "second new fails");
    if (err.release) err.release (&err);

    /* SetOption works pre-Init. */
    ASSERT (drv.DatabaseSetOption (&db, "uri", "virtuoso://localhost:1111",
                                   &err) == ADBC_STATUS_OK,
            "set uri pre-init");

    /* GetOption returns it. */
    {
        char buf[128]; size_t blen = sizeof (buf);
        ASSERT (drv.DatabaseGetOption (&db, "uri", buf, &blen, &err)
                    == ADBC_STATUS_OK,
                "get uri pre-init");
        ASSERT (strcmp (buf, "virtuoso://localhost:1111") == 0,
                "get uri value");
    }

    /* Release works; private_data cleared. */
    ASSERT (drv.DatabaseRelease (&db, &err) == ADBC_STATUS_OK, "release ok");
    ASSERT (db.private_data == NULL, "release clears private_data");

    /* Release on already-released is no-op. */
    ASSERT (drv.DatabaseRelease (&db, &err) == ADBC_STATUS_OK,
            "double release no-op");

    /* Init without a configured uri returns INVALID_ARGUMENT. */
    memset (&db, 0, sizeof (db));
    drv.DatabaseNew (&db, &err);
    rc = drv.DatabaseInit (&db, &err);
    ASSERT (rc == ADBC_STATUS_INVALID_ARGUMENT, "init without host fails");
    if (err.release) err.release (&err);
    drv.DatabaseRelease (&db, &err);
}

/* ---------- main ---------- */

int
main (void)
{
    test_options_store ();
    test_uri_parser ();
    test_database_lifecycle ();

    if (g_failures) {
        fprintf (stderr, "%d test case(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    fprintf (stdout, "OK -- ADBC phase 2 unit tests passed\n");
    return EXIT_SUCCESS;
}
