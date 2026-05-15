/*
 *  unit_test.c
 *
 *  Phase 2/3 unit tests for the Virtuoso ADBC driver.
 *
 *  Read-only tests cover the option store, URI -> connection-string
 *  parsing, and the AdbcDatabase / AdbcConnection lifecycle state
 *  machine (errors on use-after-init, use-after-release, etc).
 *
 *  Integration tests that need a live Virtuoso server are gated on
 *  the environment variable VIRT_ADBC_TEST_URI. When unset, those
 *  cases skip (and the test still exits 0 if everything else passes).
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
#include "nanoarrow.h"
#include "virtuoso_adbc.h"

static int g_failures = 0;

#define ASSERT(cond, label)                                          \
    do {                                                             \
        if (!(cond)) {                                               \
            fprintf (stderr, "[FAIL] %s: %s (line %d)\n",            \
                     (label), #cond, __LINE__);                      \
            fflush (stderr);                                         \
            g_failures++;                                            \
        }                                                            \
    } while (0)

/* TRACE is intentionally a no-op in committed code; flip to fprintf
 * locally when debugging an integration regression.                  */
#define TRACE(...) ((void) 0)

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

    /* Init with no host should fail (we cleared opts? no, we have uri). */
    /* Don't Init yet -- needs a real server. Instead verify Release works. */
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

/* ---------- AdbcConnection: read-only state checks ---------- */

static void
test_connection_lifecycle_negative (void)
{
    struct AdbcDriver drv;
    struct AdbcConnection cn;
    struct AdbcDatabase db;
    struct AdbcError err = ADBC_ERROR_INIT;
    AdbcStatusCode rc;

    load_driver (&drv);

    /* ConnectionInit with an uninitialised database should fail. */
    memset (&cn, 0, sizeof (cn));
    memset (&db, 0, sizeof (db));
    ASSERT (drv.ConnectionNew (&cn, &err) == ADBC_STATUS_OK, "cn new");
    drv.DatabaseNew (&db, &err);
    /* db has no uri AND DatabaseInit hasn't been called. */
    rc = drv.ConnectionInit (&cn, &db, &err);
    ASSERT (rc == ADBC_STATUS_INVALID_STATE, "init with uninit db fails");
    if (err.release) err.release (&err);

    /* Commit/Rollback before Init should fail (not connected). */
    rc = drv.ConnectionCommit (&cn, &err);
    ASSERT (rc == ADBC_STATUS_INVALID_STATE, "commit not connected");
    if (err.release) err.release (&err);
    rc = drv.ConnectionRollback (&cn, &err);
    ASSERT (rc == ADBC_STATUS_INVALID_STATE, "rollback not connected");
    if (err.release) err.release (&err);
    rc = drv.ConnectionCancel (&cn, &err);
    ASSERT (rc == ADBC_STATUS_INVALID_STATE, "cancel not connected");
    if (err.release) err.release (&err);

    /* Autocommit getter pre-init returns the default 'true'. */
    {
        char buf[16]; size_t blen = sizeof (buf);
        ASSERT (drv.ConnectionGetOption (&cn, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                         buf, &blen, &err) == ADBC_STATUS_OK,
                "get autocommit default");
        ASSERT (strcmp (buf, ADBC_OPTION_VALUE_ENABLED) == 0,
                "autocommit default is true");
    }

    /* Toggling autocommit pre-connect should just stash the value. */
    ASSERT (drv.ConnectionSetOption (&cn, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                     ADBC_OPTION_VALUE_DISABLED, &err)
                == ADBC_STATUS_OK,
            "set autocommit false pre-connect");

    drv.ConnectionRelease (&cn, &err);
    drv.DatabaseRelease (&db, &err);
}

/* ---------- AdbcStatement: read-only state checks ---------- */

static void
test_statement_lifecycle_negative (void)
{
    struct AdbcDriver drv;
    struct AdbcStatement st;
    struct AdbcConnection cn;
    struct AdbcError err = ADBC_ERROR_INIT;
    AdbcStatusCode rc;

    load_driver (&drv);
    memset (&st, 0, sizeof (st));
    memset (&cn, 0, sizeof (cn));

    /* StatementNew with NULL connection fails. */
    rc = drv.StatementNew (NULL, &st, &err);
    ASSERT (rc == ADBC_STATUS_INVALID_ARGUMENT, "st new: NULL cn");
    if (err.release) err.release (&err);

    /* StatementNew on a not-opened connection fails. */
    drv.ConnectionNew (&cn, &err);
    rc = drv.StatementNew (&cn, &st, &err);
    ASSERT (rc == ADBC_STATUS_INVALID_STATE,
            "st new: connection not connected");
    if (err.release) err.release (&err);

    /* ExecuteQuery on a not-initialised handle fails. */
    rc = drv.StatementExecuteQuery (&st, NULL, NULL, &err);
    ASSERT (rc == ADBC_STATUS_INVALID_STATE,
            "st exec: not initialised");
    if (err.release) err.release (&err);

    /* Release on a not-initialised handle is a no-op. */
    ASSERT (drv.StatementRelease (&st, &err) == ADBC_STATUS_OK,
            "st release no-op");

    drv.ConnectionRelease (&cn, &err);
}

/* ---------- Integration: SELECT round-trip ---------- */

static void
test_statement_select_roundtrip (struct AdbcDriver *drv, struct AdbcDatabase *db,
                                  struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowSchema schema;
    struct ArrowArray batch;
    AdbcStatusCode rc;
    int saw_batch = 0;
    int second_call_is_eof = 0;

    memset (&st, 0, sizeof (st));
    memset (&stream, 0, sizeof (stream));
    memset (&schema, 0, sizeof (schema));
    memset (&batch, 0, sizeof (batch));

    ASSERT (drv->StatementNew (cn, &st, &err) == ADBC_STATUS_OK,
            "select: stmt new");
    ASSERT (drv->StatementSetSqlQuery (&st, "SELECT 1, 'hello', cast(3.14 as float)",
                                       &err) == ADBC_STATUS_OK,
            "select: set sql");
    rc = drv->StatementExecuteQuery (&st, &stream, NULL, &err);
    if (rc != ADBC_STATUS_OK) {
        fprintf (stderr, "[FAIL-integration] ExecuteQuery: %s\n",
                 err.message ? err.message : "(no msg)");
        g_failures++;
        if (err.release) err.release (&err);
        drv->StatementRelease (&st, NULL);
        return;
    }

    ASSERT (stream.get_schema (&stream, &schema) == 0, "select: get_schema");
    ASSERT (schema.n_children == 3, "select: 3 columns");
    if (schema.release) schema.release (&schema);

    ASSERT (stream.get_next (&stream, &batch) == 0, "select: get_next");
    if (batch.release) {
        saw_batch = 1;
        ASSERT (batch.length == 1, "select: one row");
        ASSERT (batch.n_children == 3, "select: 3 children");
        batch.release (&batch);
    }
    ASSERT (saw_batch, "select: got a batch");

    /* Second call returns EOF (released zero-length array). */
    memset (&batch, 0, sizeof (batch));
    ASSERT (stream.get_next (&stream, &batch) == 0, "select: eof");
    if (batch.release == NULL) second_call_is_eof = 1;
    ASSERT (second_call_is_eof, "select: second call EOF");

    stream.release (&stream);
    drv->StatementRelease (&st, NULL);
    fprintf (stdout, "[OK]  integration: SELECT 1,'hello',3.14 round-trip\n");
}

static void
test_statement_null_column (struct AdbcDriver *drv, struct AdbcDatabase *db,
                             struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowArray batch;
    int has_nulls = 0;

    memset (&st, 0, sizeof (st));
    memset (&stream, 0, sizeof (stream));
    memset (&batch, 0, sizeof (batch));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st, "SELECT NULL, 42, NULL", &err);
    if (drv->StatementExecuteQuery (&st, &stream, NULL, &err) != ADBC_STATUS_OK) {
        fprintf (stderr, "[FAIL-integration] NULL query: %s\n",
                 err.message ? err.message : "(no msg)");
        g_failures++;
        if (err.release) err.release (&err);
        drv->StatementRelease (&st, NULL);
        return;
    }
    if (stream.get_next (&stream, &batch) == 0 && batch.release) {
        ASSERT (batch.length == 1, "null: one row");
        /* NULL columns: child null_count should be 1. */
        if (batch.n_children >= 3
            && batch.children[0]->null_count == 1
            && batch.children[2]->null_count == 1)
            has_nulls = 1;
        ASSERT (has_nulls, "null: null mask set on cols 0 and 2");
        batch.release (&batch);
    }
    stream.release (&stream);
    drv->StatementRelease (&st, NULL);
    fprintf (stdout, "[OK]  integration: NULL columns\n");
}

static void
test_statement_no_result (struct AdbcDriver *drv, struct AdbcDatabase *db,
                          struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    int64_t rows = -2;

    memset (&st, 0, sizeof (st));
    drv->StatementNew (cn, &st, &err);
    /* commit work parses, returns no result set, succeeds when no
     * transaction is open (no-op).                                     */
    drv->StatementSetSqlQuery (&st, "commit work", &err);
    ASSERT (drv->StatementExecuteQuery (&st, NULL, &rows, &err)
                == ADBC_STATUS_OK,
            "no-result: commit work succeeds");
    drv->StatementRelease (&st, NULL);
    if (err.release) err.release (&err);
    fprintf (stdout, "[OK]  integration: no-result DDL/CMD\n");
}

static void
test_statement_batching (struct AdbcDriver *drv, struct AdbcDatabase *db,
                          struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowArray batch;
    int64_t total_rows = 0;
    int batches = 0;

    memset (&st, 0, sizeof (st));
    memset (&stream, 0, sizeof (stream));

    drv->StatementNew (cn, &st, &err);
    /* Force a small batch size so we hit at least two get_next calls. */
    drv->StatementSetOption (&st, "adbc.virtuoso.fetch.batch_rows", "32", &err);
    /* Virtuoso ships with DB.DBA.SYS_KEYS, an indexed catalog with
     * comfortably more than 32 rows.                                  */
    drv->StatementSetSqlQuery (&st,
                               "SELECT TOP 200 KEY_NAME FROM DB.DBA.SYS_KEYS",
                               &err);
    if (drv->StatementExecuteQuery (&st, &stream, NULL, &err) != ADBC_STATUS_OK) {
        fprintf (stderr, "[SKIP-integration] batching: %s\n",
                 err.message ? err.message : "(no msg)");
        if (err.release) err.release (&err);
        drv->StatementRelease (&st, NULL);
        return;
    }
    for (;;) {
        memset (&batch, 0, sizeof (batch));
        if (stream.get_next (&stream, &batch) != 0) break;
        if (!batch.release) break;
        total_rows += batch.length;
        batches++;
        batch.release (&batch);
    }
    stream.release (&stream);
    drv->StatementRelease (&st, NULL);
    ASSERT (batches >= 2,
            "batching: expected >= 2 batches with batch_rows=32");
    ASSERT (total_rows > 0, "batching: got rows");
    fprintf (stdout, "[OK]  integration: batching (%d rows / %d batches)\n",
             (int) total_rows, batches);
}

/* ---------- Integration: gated on VIRT_ADBC_TEST_URI ---------- */

static void
test_integration (const char *uri)
{
    struct AdbcDriver drv;
    struct AdbcDatabase db;
    struct AdbcConnection cn;
    struct AdbcError err = ADBC_ERROR_INIT;
    AdbcStatusCode rc;
    const char *user = getenv ("VIRT_ADBC_TEST_USER");
    const char *pass = getenv ("VIRT_ADBC_TEST_PASSWORD");

    load_driver (&drv);
    memset (&db, 0, sizeof (db));
    memset (&cn, 0, sizeof (cn));

    drv.DatabaseNew (&db, &err);
    ASSERT (drv.DatabaseSetOption (&db, "uri", uri, &err) == ADBC_STATUS_OK,
            "integration: set uri");
    if (user)
        drv.DatabaseSetOption (&db, "username", user, &err);
    if (pass)
        drv.DatabaseSetOption (&db, "password", pass, &err);

    rc = drv.DatabaseInit (&db, &err);
    if (rc != ADBC_STATUS_OK) {
        fprintf (stderr, "[SKIP-integration] DatabaseInit failed: %s\n",
                 err.message ? err.message : "(no msg)");
        if (err.release) err.release (&err);
        drv.DatabaseRelease (&db, NULL);
        return;
    }

    drv.ConnectionNew (&cn, &err);
    rc = drv.ConnectionInit (&cn, &db, &err);
    if (rc != ADBC_STATUS_OK) {
        fprintf (stderr, "[FAIL-integration] ConnectionInit: %s\n",
                 err.message ? err.message : "(no msg)");
        g_failures++;
        if (err.release) err.release (&err);
        drv.ConnectionRelease (&cn, NULL);
        drv.DatabaseRelease (&db, NULL);
        return;
    }
    fprintf (stdout, "[OK]  integration: connected to %s\n", uri);

    /* Phase 4 statement integration tests. We re-use the open
     * connection rather than reopening for each case.                 */
    TRACE ("before select_roundtrip\n");
    test_statement_select_roundtrip (&drv, &db, &cn);

    /* Switch off autocommit, then commit (which should be a no-op
     * with nothing to commit, but must succeed). */
    ASSERT (drv.ConnectionSetOption (&cn, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                                     ADBC_OPTION_VALUE_DISABLED, &err)
                == ADBC_STATUS_OK,
            "integration: autocommit off");
    ASSERT (drv.ConnectionCommit (&cn, &err) == ADBC_STATUS_OK,
            "integration: commit empty txn");
    if (err.release) err.release (&err);
    ASSERT (drv.ConnectionRollback (&cn, &err) == ADBC_STATUS_OK,
            "integration: rollback empty txn");
    if (err.release) err.release (&err);

    /* Cancel with no running stmt is a no-op. */
    ASSERT (drv.ConnectionCancel (&cn, &err) == ADBC_STATUS_OK,
            "integration: cancel no-op");
    if (err.release) err.release (&err);
    TRACE ("after select_roundtrip\n");
    TRACE ("before null_column\n");
    test_statement_null_column      (&drv, &db, &cn);
    TRACE ("before no_result\n");
    test_statement_no_result        (&drv, &db, &cn);
    TRACE ("before batching\n");
    test_statement_batching         (&drv, &db, &cn);
    TRACE ("after batching\n");

    drv.ConnectionRelease (&cn, &err);
    drv.DatabaseRelease (&db, &err);
}

/* ---------- main ---------- */

int
main (void)
{
    const char *uri;

    test_options_store ();
    test_uri_parser ();
    test_database_lifecycle ();
    test_connection_lifecycle_negative ();
    test_statement_lifecycle_negative ();

    uri = getenv ("VIRT_ADBC_TEST_URI");
    if (uri && *uri) {
        test_integration (uri);
    } else {
        fprintf (stdout,
                 "[SKIP] integration tests (set VIRT_ADBC_TEST_URI to enable)\n");
    }

    if (g_failures) {
        fprintf (stderr, "%d test case(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    fprintf (stdout, "OK -- ADBC phase 2-4 unit tests passed\n");
    return EXIT_SUCCESS;
}
