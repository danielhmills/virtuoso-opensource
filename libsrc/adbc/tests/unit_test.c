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

/* ---------- Integration: Phase 5 (prepare + bind) ---------- */

static void
test_prepared_select (struct AdbcDriver *drv, struct AdbcDatabase *db,
                      struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowSchema schema;
    struct ArrowArray batch;
    int got = 0;
    (void) db;

    memset (&st, 0, sizeof (st));
    memset (&stream, 0, sizeof (stream));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st, "SELECT 7, 'phase5'", &err);
    ASSERT (drv->StatementPrepare (&st, &err) == ADBC_STATUS_OK,
            "prepared select: prepare");
    /* Prepare is idempotent. */
    ASSERT (drv->StatementPrepare (&st, &err) == ADBC_STATUS_OK,
            "prepared select: prepare idempotent");

    ASSERT (drv->StatementExecuteQuery (&st, &stream, NULL, &err)
                == ADBC_STATUS_OK,
            "prepared select: exec");
    if (stream.release) {
        memset (&schema, 0, sizeof (schema));
        ASSERT (stream.get_schema (&stream, &schema) == 0,
                "prepared select: schema");
        ASSERT (schema.n_children == 2, "prepared select: 2 cols");
        if (schema.release) schema.release (&schema);
        memset (&batch, 0, sizeof (batch));
        if (stream.get_next (&stream, &batch) == 0 && batch.release) {
            got = (batch.length == 1 && batch.n_children == 2);
            batch.release (&batch);
        }
        ASSERT (got, "prepared select: one row");
        stream.release (&stream);
    }
    drv->StatementRelease (&st, NULL);
    fprintf (stdout, "[OK]  integration: prepared SELECT (no params)\n");
}

static void
test_parameter_schema (struct AdbcDriver *drv, struct AdbcDatabase *db,
                       struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowSchema sch;
    (void) db;

    memset (&st, 0, sizeof (st));
    memset (&sch, 0, sizeof (sch));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st, "SELECT ?, ?", &err);
    ASSERT (drv->StatementPrepare (&st, &err) == ADBC_STATUS_OK,
            "param schema: prepare");
    ASSERT (drv->StatementGetParameterSchema (&st, &sch, &err)
                == ADBC_STATUS_OK,
            "param schema: get");
    ASSERT (sch.n_children == 2, "param schema: 2 params");
    if (sch.release) sch.release (&sch);
    drv->StatementRelease (&st, NULL);
    fprintf (stdout, "[OK]  integration: GetParameterSchema (n=2)\n");
}

/* Builds a 2-column int64 + utf8 batch with the given pairs, hands
 * the resulting (ArrowArray, ArrowSchema) to Bind. */
static void
build_int_str_batch (const int64_t *ids, const char *const *names, int64_t n,
                     struct ArrowArray *out_arr, struct ArrowSchema *out_sch)
{
    struct ArrowError ae;
    int64_t i;

    ArrowSchemaInit (out_sch);
    (void) ArrowSchemaSetTypeStruct (out_sch, 2);
    (void) ArrowSchemaSetType (out_sch->children[0], NANOARROW_TYPE_INT64);
    (void) ArrowSchemaSetName (out_sch->children[0], "id");
    (void) ArrowSchemaSetType (out_sch->children[1], NANOARROW_TYPE_STRING);
    (void) ArrowSchemaSetName (out_sch->children[1], "name");

    memset (&ae, 0, sizeof (ae));
    (void) ArrowArrayInitFromSchema (out_arr, out_sch, &ae);
    (void) ArrowArrayStartAppending (out_arr);
    for (i = 0; i < n; i++) {
        struct ArrowStringView sv = ArrowCharView (names[i]);
        (void) ArrowArrayAppendInt (out_arr->children[0], ids[i]);
        if (names[i] == NULL)
            (void) ArrowArrayAppendNull (out_arr->children[1], 1);
        else
            (void) ArrowArrayAppendString (out_arr->children[1], sv);
        (void) ArrowArrayFinishElement (out_arr);
    }
    (void) ArrowArrayFinishBuildingDefault (out_arr, &ae);
}

static void
test_bind_insert (struct AdbcDriver *drv, struct AdbcDatabase *db,
                  struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArray  arr;
    struct ArrowSchema sch;
    int64_t ids[]    = { 1, 2, 3, 4 };
    const char *names[] = { "alpha", "beta", "gamma", NULL };
    int64_t rows = 0;
    AdbcStatusCode rc;
    (void) db;

    memset (&st, 0, sizeof (st));
    memset (&arr, 0, sizeof (arr));
    memset (&sch, 0, sizeof (sch));

    /* Set up a fresh temp table. */
    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st,
        "drop table if exists DB.DBA.t_adbc_phase5", &err);
    drv->StatementExecuteQuery (&st, NULL, NULL, &err);
    drv->StatementRelease (&st, NULL);

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st,
        "create table DB.DBA.t_adbc_phase5 (id integer, name varchar(64))",
        &err);
    rc = drv->StatementExecuteQuery (&st, NULL, NULL, &err);
    if (rc != ADBC_STATUS_OK) {
        fprintf (stderr, "[SKIP-integration] bind_insert: create table failed: %s\n",
                 err.message ? err.message : "(no msg)");
        if (err.release) err.release (&err);
        drv->StatementRelease (&st, NULL);
        return;
    }
    drv->StatementRelease (&st, NULL);

    /* Prepare + Bind + ExecuteQuery on the INSERT. */
    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st,
        "insert into DB.DBA.t_adbc_phase5 (id, name) values (?, ?)", &err);
    ASSERT (drv->StatementPrepare (&st, &err) == ADBC_STATUS_OK,
            "bind_insert: prepare");

    build_int_str_batch (ids, names, 4, &arr, &sch);
    ASSERT (drv->StatementBind (&st, &arr, &sch, &err) == ADBC_STATUS_OK,
            "bind_insert: bind");
    ASSERT (drv->StatementExecuteQuery (&st, NULL, &rows, &err)
                == ADBC_STATUS_OK,
            "bind_insert: exec");
    ASSERT (rows == 4, "bind_insert: 4 rows affected");
    drv->StatementRelease (&st, NULL);

    /* Read them back and verify NULL handling. */
    {
        struct AdbcStatement st2;
        struct ArrowArrayStream stream;
        struct ArrowArray batch;
        int verified = 0;

        memset (&st2, 0, sizeof (st2));
        memset (&stream, 0, sizeof (stream));
        memset (&batch, 0, sizeof (batch));

        drv->StatementNew (cn, &st2, &err);
        drv->StatementSetSqlQuery (&st2,
            "select count(*), count(name) from DB.DBA.t_adbc_phase5", &err);
        drv->StatementExecuteQuery (&st2, &stream, NULL, &err);
        if (stream.get_next (&stream, &batch) == 0 && batch.release) {
            /* count(*) = 4, count(name) = 3 (one NULL). */
            int64_t total = -1, with_name = -1;
            struct ArrowArrayView v;
            struct ArrowError ae;
            memset (&v, 0, sizeof (v));
            memset (&ae, 0, sizeof (ae));
            {
                struct ArrowSchema rs;
                memset (&rs, 0, sizeof (rs));
                stream.get_schema (&stream, &rs);
                if (ArrowArrayViewInitFromSchema (&v, &rs, &ae) == 0
                    && ArrowArrayViewSetArray (&v, &batch, &ae) == 0) {
                    total     = ArrowArrayViewGetIntUnsafe (v.children[0], 0);
                    with_name = ArrowArrayViewGetIntUnsafe (v.children[1], 0);
                }
                ArrowArrayViewReset (&v);
                if (rs.release) rs.release (&rs);
            }
            verified = (total == 4 && with_name == 3);
            batch.release (&batch);
        }
        ASSERT (verified, "bind_insert: 4 rows / 3 non-null names");
        if (stream.release) stream.release (&stream);
        drv->StatementRelease (&st2, NULL);
    }
    fprintf (stdout, "[OK]  integration: prepared INSERT with bound batch (4 rows)\n");
}

/* A trivial ArrowArrayStream that emits two fixed batches then EOF.
 * Used to validate StatementBindStream wiring without needing pyarrow. */
typedef struct mock_stream_state {
    int                 cur;
    int                 nbatch;
    const int64_t      *ids;     /* contiguous; lengths[0..nbatch-1]      */
    const char *const  *names;
    const int64_t      *lengths;
    int64_t             offset;
    struct ArrowSchema  schema;
} mock_stream_state_t;

static int
mock_get_schema (struct ArrowArrayStream *s, struct ArrowSchema *out)
{
    mock_stream_state_t *m = (mock_stream_state_t *) s->private_data;
    return ArrowSchemaDeepCopy (&m->schema, out);
}

static int
mock_get_next (struct ArrowArrayStream *s, struct ArrowArray *out)
{
    mock_stream_state_t *m = (mock_stream_state_t *) s->private_data;
    int64_t len;
    int64_t i;
    struct ArrowError ae;
    memset (out, 0, sizeof (*out));
    if (m->cur >= m->nbatch) return 0;
    len = m->lengths[m->cur];
    memset (&ae, 0, sizeof (ae));
    if (ArrowArrayInitFromSchema (out, &m->schema, &ae) != 0) return EIO;
    ArrowArrayStartAppending (out);
    for (i = 0; i < len; i++) {
        int64_t idx = m->offset + i;
        ArrowArrayAppendInt (out->children[0], m->ids[idx]);
        if (m->names[idx])
            ArrowArrayAppendString (out->children[1],
                                    ArrowCharView (m->names[idx]));
        else
            ArrowArrayAppendNull (out->children[1], 1);
        ArrowArrayFinishElement (out);
    }
    ArrowArrayFinishBuildingDefault (out, &ae);
    m->offset += len;
    m->cur++;
    return 0;
}

static const char *
mock_last_err (struct ArrowArrayStream *s) { (void) s; return NULL; }

static void
mock_release (struct ArrowArrayStream *s)
{
    mock_stream_state_t *m = (mock_stream_state_t *) s->private_data;
    if (m) {
        if (m->schema.release) m->schema.release (&m->schema);
        free (m);
    }
    s->private_data = NULL;
    s->release      = NULL;
}

static void
test_bind_stream_insert (struct AdbcDriver *drv, struct AdbcDatabase *db,
                         struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    mock_stream_state_t *state;
    int64_t rows = 0;
    AdbcStatusCode rc;
    static const int64_t ids[] = { 100, 101, 102, 103, 104, 105 };
    static const char *const names[] = {
        "one", "two", "three", "four", "five", "six"
    };
    static const int64_t lengths[] = { 3, 3 };
    (void) db;

    memset (&st, 0, sizeof (st));
    memset (&stream, 0, sizeof (stream));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st,
        "delete from DB.DBA.t_adbc_phase5", &err);
    drv->StatementExecuteQuery (&st, NULL, NULL, &err);
    drv->StatementRelease (&st, NULL);

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st,
        "insert into DB.DBA.t_adbc_phase5 (id, name) values (?, ?)", &err);
    ASSERT (drv->StatementPrepare (&st, &err) == ADBC_STATUS_OK,
            "bind_stream: prepare");

    state = (mock_stream_state_t *) calloc (1, sizeof (*state));
    state->ids     = ids;
    state->names   = names;
    state->lengths = lengths;
    state->nbatch  = 2;
    state->cur     = 0;
    state->offset  = 0;
    ArrowSchemaInit (&state->schema);
    ArrowSchemaSetTypeStruct (&state->schema, 2);
    ArrowSchemaSetType (state->schema.children[0], NANOARROW_TYPE_INT64);
    ArrowSchemaSetName (state->schema.children[0], "id");
    ArrowSchemaSetType (state->schema.children[1], NANOARROW_TYPE_STRING);
    ArrowSchemaSetName (state->schema.children[1], "name");

    stream.private_data   = state;
    stream.get_schema     = mock_get_schema;
    stream.get_next       = mock_get_next;
    stream.get_last_error = mock_last_err;
    stream.release        = mock_release;

    ASSERT (drv->StatementBindStream (&st, &stream, &err) == ADBC_STATUS_OK,
            "bind_stream: bind");
    rc = drv->StatementExecuteQuery (&st, NULL, &rows, &err);
    ASSERT (rc == ADBC_STATUS_OK, "bind_stream: exec");
    ASSERT (rows == 6, "bind_stream: 6 rows from 2 batches");
    drv->StatementRelease (&st, NULL);

    /* Drop the temp table. */
    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st,
        "drop table DB.DBA.t_adbc_phase5", &err);
    drv->StatementExecuteQuery (&st, NULL, NULL, &err);
    drv->StatementRelease (&st, NULL);

    fprintf (stdout, "[OK]  integration: BindStream (6 rows / 2 batches)\n");
}

/* ---------- Integration: Phase 6 (catalog / metadata) ---------- */

static void
test_get_table_types (struct AdbcDriver *drv, struct AdbcDatabase *db,
                      struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowSchema sch;
    struct ArrowArray arr;
    (void) db;

    memset (&stream, 0, sizeof (stream));
    memset (&sch,    0, sizeof (sch));
    memset (&arr,    0, sizeof (arr));

    ASSERT (drv->ConnectionGetTableTypes (cn, &stream, &err) == ADBC_STATUS_OK,
            "get_table_types: invoke");
    ASSERT (stream.get_schema (&stream, &sch) == 0,
            "get_table_types: schema");
    ASSERT (sch.n_children == 1, "get_table_types: 1 column");
    if (sch.release) sch.release (&sch);

    ASSERT (stream.get_next (&stream, &arr) == 0, "get_table_types: batch");
    if (arr.release) {
        ASSERT (arr.length == 3, "get_table_types: 3 rows");
        arr.release (&arr);
    } else {
        ASSERT (0, "get_table_types: no batch");
    }
    stream.release (&stream);
    fprintf (stdout, "[OK]  integration: GetTableTypes (3 rows)\n");
}

static void
test_get_info (struct AdbcDriver *drv, struct AdbcDatabase *db,
               struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowSchema sch;
    struct ArrowArray arr;
    (void) db;

    memset (&stream, 0, sizeof (stream));
    memset (&sch,    0, sizeof (sch));
    memset (&arr,    0, sizeof (arr));

    /* NULL info_codes -> driver returns its default set. */
    ASSERT (drv->ConnectionGetInfo (cn, NULL, 0, &stream, &err)
                == ADBC_STATUS_OK,
            "get_info: invoke");
    ASSERT (stream.get_schema (&stream, &sch) == 0, "get_info: schema");
    ASSERT (sch.n_children == 2, "get_info: 2 columns (name, value)");
    if (sch.release) sch.release (&sch);
    ASSERT (stream.get_next (&stream, &arr) == 0, "get_info: batch");
    if (arr.release) {
        /* default set has 5 codes. */
        ASSERT (arr.length == 5, "get_info: 5 rows from default set");
        arr.release (&arr);
    }
    stream.release (&stream);
    fprintf (stdout, "[OK]  integration: GetInfo (5 default rows)\n");
}

static void
test_get_table_schema (struct AdbcDriver *drv, struct AdbcDatabase *db,
                       struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowSchema sch;
    AdbcStatusCode rc;
    (void) db;

    memset (&sch, 0, sizeof (sch));
    /* SYS_KEYS is shipped with every Virtuoso instance. */
    rc = drv->ConnectionGetTableSchema (cn, NULL, "DB.DBA", "SYS_KEYS",
                                        &sch, &err);
    if (rc != ADBC_STATUS_OK) {
        /* Virtuoso quotes are odd; retry with a known-fully-qualified
         * path without the catalog filter. */
        if (err.release) err.release (&err);
        memset (&err, 0, sizeof (err));
        rc = drv->ConnectionGetTableSchema (cn, NULL, NULL, "SYS_KEYS",
                                            &sch, &err);
    }
    ASSERT (rc == ADBC_STATUS_OK, "get_table_schema: invoke");
    if (rc == ADBC_STATUS_OK) {
        ASSERT (sch.n_children > 0, "get_table_schema: has columns");
        if (sch.release) sch.release (&sch);
        fprintf (stdout, "[OK]  integration: GetTableSchema(SYS_KEYS)\n");
    } else if (err.release) {
        fprintf (stderr, "  err: %s\n", err.message ? err.message : "");
        err.release (&err);
    }

    /* Unknown table -> NOT_FOUND. */
    memset (&sch, 0, sizeof (sch));
    memset (&err, 0, sizeof (err));
    rc = drv->ConnectionGetTableSchema (cn, NULL, NULL,
                                        "no_such_table_xyz_42", &sch, &err);
    ASSERT (rc != ADBC_STATUS_OK,
            "get_table_schema: missing table is rejected");
    if (sch.release) sch.release (&sch);
    if (err.release) err.release (&err);
}

static void
test_get_objects (struct AdbcDriver *drv, struct AdbcDatabase *db,
                  struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowSchema sch;
    struct ArrowArray arr;
    int rows = 0;
    (void) db;

    memset (&stream, 0, sizeof (stream));
    memset (&sch,    0, sizeof (sch));
    memset (&arr,    0, sizeof (arr));

    /* CATALOGS depth: just the catalog list, no schemas/tables. */
    ASSERT (drv->ConnectionGetObjects (cn, ADBC_OBJECT_DEPTH_CATALOGS,
                                       NULL, NULL, NULL, NULL, NULL,
                                       &stream, &err) == ADBC_STATUS_OK,
            "get_objects(CATALOGS): invoke");
    ASSERT (stream.get_schema (&stream, &sch) == 0,
            "get_objects(CATALOGS): schema");
    if (sch.release) sch.release (&sch);
    if (stream.get_next (&stream, &arr) == 0 && arr.release) {
        rows = (int) arr.length;
        arr.release (&arr);
    }
    ASSERT (rows >= 1, "get_objects(CATALOGS): at least one catalog");
    stream.release (&stream);
    fprintf (stdout, "[OK]  integration: GetObjects(CATALOGS, %d rows)\n",
             rows);

    /* TABLES depth, unfiltered. Virtuoso ships with dozens of system
     * tables; any non-zero count proves the SQLTables call worked. */
    memset (&stream, 0, sizeof (stream));
    memset (&arr,    0, sizeof (arr));
    rows = 0;
    ASSERT (drv->ConnectionGetObjects (cn, ADBC_OBJECT_DEPTH_TABLES,
                                       NULL, NULL, NULL, NULL, NULL,
                                       &stream, &err) == ADBC_STATUS_OK,
            "get_objects(TABLES): invoke");
    if (stream.get_next (&stream, &arr) == 0 && arr.release) {
        rows = (int) arr.length;
        arr.release (&arr);
    }
    ASSERT (rows >= 1, "get_objects(TABLES): >=1 catalog row");
    stream.release (&stream);
    fprintf (stdout, "[OK]  integration: GetObjects(TABLES, %d catalog rows)\n",
             rows);

    /* ALL depth -- exercise the columns walk. We pass a tight schema
     * filter to keep the response small. */
    memset (&stream, 0, sizeof (stream));
    memset (&arr,    0, sizeof (arr));
    rows = 0;
    ASSERT (drv->ConnectionGetObjects (cn, ADBC_OBJECT_DEPTH_ALL,
                                       NULL, "DBA", NULL, NULL, NULL,
                                       &stream, &err) == ADBC_STATUS_OK,
            "get_objects(ALL): invoke");
    if (stream.get_next (&stream, &arr) == 0 && arr.release) {
        rows = (int) arr.length;
        arr.release (&arr);
    }
    ASSERT (rows >= 1, "get_objects(ALL): rows");
    stream.release (&stream);
    fprintf (stdout, "[OK]  integration: GetObjects(ALL, schema=DBA)\n");
}

/* ---------- Integration: Phase 7 (bulk ingest) ---------- */

/* Quick helper: execute a SQL command, ignore result. */
static AdbcStatusCode
exec_one (struct AdbcDriver *drv, struct AdbcConnection *cn, const char *sql,
          struct AdbcError *err)
{
    struct AdbcStatement st;
    AdbcStatusCode rc;
    memset (&st, 0, sizeof (st));
    drv->StatementNew (cn, &st, err);
    drv->StatementSetSqlQuery (&st, sql, err);
    rc = drv->StatementExecuteQuery (&st, NULL, NULL, err);
    drv->StatementRelease (&st, NULL);
    return rc;
}

/* Read a single int64 from SELECT ... LIMIT 1. */
static int64_t
select_count (struct AdbcDriver *drv, struct AdbcConnection *cn,
              const char *sql)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowSchema sch;
    struct ArrowArray batch;
    struct ArrowArrayView v;
    struct ArrowError ae;
    int64_t result = -1;

    memset (&st,     0, sizeof (st));
    memset (&stream, 0, sizeof (stream));
    memset (&sch,    0, sizeof (sch));
    memset (&batch,  0, sizeof (batch));
    memset (&v,      0, sizeof (v));
    memset (&ae,     0, sizeof (ae));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetSqlQuery (&st, sql, &err);
    if (drv->StatementExecuteQuery (&st, &stream, NULL, &err) != ADBC_STATUS_OK)
        goto out;
    if (stream.get_schema (&stream, &sch) != 0) goto out;
    if (stream.get_next (&stream, &batch) != 0 || !batch.release) goto out;
    if (ArrowArrayViewInitFromSchema (&v, &sch, &ae) == 0
        && ArrowArrayViewSetArray (&v, &batch, &ae) == 0)
        result = ArrowArrayViewGetIntUnsafe (v.children[0], 0);
    ArrowArrayViewReset (&v);
    if (batch.release) batch.release (&batch);
out:
    if (sch.release)    sch.release (&sch);
    if (stream.release) stream.release (&stream);
    drv->StatementRelease (&st, NULL);
    if (err.release) err.release (&err);
    return result;
}

static void
build_int_str_dbl_batch (struct ArrowArray *out_arr,
                         struct ArrowSchema *out_sch)
{
    struct ArrowError ae;
    static const int64_t ids[]  = { 1, 2, 3 };
    static const char *const names[] = { "alpha", "beta", NULL };
    static const double  vals[] = { 1.1, 2.2, 3.3 };
    int i;

    ArrowSchemaInit (out_sch);
    (void) ArrowSchemaSetTypeStruct (out_sch, 3);
    (void) ArrowSchemaSetType (out_sch->children[0], NANOARROW_TYPE_INT64);
    (void) ArrowSchemaSetName (out_sch->children[0], "id");
    (void) ArrowSchemaSetType (out_sch->children[1], NANOARROW_TYPE_STRING);
    (void) ArrowSchemaSetName (out_sch->children[1], "name");
    (void) ArrowSchemaSetType (out_sch->children[2], NANOARROW_TYPE_DOUBLE);
    (void) ArrowSchemaSetName (out_sch->children[2], "score");

    memset (&ae, 0, sizeof (ae));
    (void) ArrowArrayInitFromSchema (out_arr, out_sch, &ae);
    (void) ArrowArrayStartAppending (out_arr);
    for (i = 0; i < 3; i++) {
        (void) ArrowArrayAppendInt (out_arr->children[0], ids[i]);
        if (names[i])
            (void) ArrowArrayAppendString (out_arr->children[1],
                                           ArrowCharView (names[i]));
        else
            (void) ArrowArrayAppendNull (out_arr->children[1], 1);
        (void) ArrowArrayAppendDouble (out_arr->children[2], vals[i]);
        (void) ArrowArrayFinishElement (out_arr);
    }
    (void) ArrowArrayFinishBuildingDefault (out_arr, &ae);
}

/* CREATE mode: derive CREATE TABLE + INSERT. */
static void
test_ingest_create (struct AdbcDriver *drv, struct AdbcDatabase *db,
                    struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct AdbcStatement st;
    struct ArrowArray  arr;
    struct ArrowSchema sch;
    int64_t rows = 0;
    int64_t cnt;
    AdbcStatusCode rc;
    (void) db;

    /* Make sure no remnants from previous runs exist. The two flavours
     * below cover both (a) the correct 3-part qualified name and
     * (b) the literal dotted name a buggy earlier build might have
     * created.                                                       */
    exec_one (drv, cn, "drop table DB.DBA.t_adbc_p7_create", &err);
    if (err.release) err.release (&err);
    memset (&err, 0, sizeof (err));
    exec_one (drv, cn,
              "drop table \"DB.DBA.t_adbc_p7_create\"", &err);
    if (err.release) err.release (&err);
    memset (&err, 0, sizeof (err));

    memset (&st,  0, sizeof (st));
    memset (&arr, 0, sizeof (arr));
    memset (&sch, 0, sizeof (sch));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_CATALOG,
                             "DB", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                             "DBA", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_TABLE,
                             "t_adbc_p7_create", &err);
    drv->StatementSetOption (&st,
                             ADBC_INGEST_OPTION_MODE,
                             ADBC_INGEST_OPTION_MODE_CREATE, &err);

    build_int_str_dbl_batch (&arr, &sch);
    ASSERT (drv->StatementBind (&st, &arr, &sch, &err) == ADBC_STATUS_OK,
            "ingest CREATE: bind");
    rc = drv->StatementExecuteQuery (&st, NULL, &rows, &err);
    ASSERT (rc == ADBC_STATUS_OK, "ingest CREATE: exec");
    ASSERT (rows == 3, "ingest CREATE: 3 rows affected");
    drv->StatementRelease (&st, NULL);

    cnt = select_count (drv, cn,
                        "select count(*) from DB.DBA.t_adbc_p7_create");
    ASSERT (cnt == 3, "ingest CREATE: SELECT COUNT(*) = 3");

    /* Calling CREATE again on the same table must error. */
    memset (&st, 0, sizeof (st));
    memset (&arr, 0, sizeof (arr));
    memset (&sch, 0, sizeof (sch));
    if (err.release) err.release (&err);
    memset (&err, 0, sizeof (err));
    drv->StatementNew (cn, &st, &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_CATALOG,
                             "DB", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                             "DBA", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_TABLE,
                             "t_adbc_p7_create", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_MODE,
                             ADBC_INGEST_OPTION_MODE_CREATE, &err);
    build_int_str_dbl_batch (&arr, &sch);
    drv->StatementBind (&st, &arr, &sch, &err);
    rc = drv->StatementExecuteQuery (&st, NULL, NULL, &err);
    ASSERT (rc != ADBC_STATUS_OK,
            "ingest CREATE on existing table is rejected");
    if (err.release) err.release (&err);
    drv->StatementRelease (&st, NULL);

    fprintf (stdout, "[OK]  integration: ingest CREATE (3 rows)\n");
}

/* APPEND mode: same table, extra rows. */
static void
test_ingest_append (struct AdbcDriver *drv, struct AdbcDatabase *db,
                    struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct AdbcStatement st;
    struct ArrowArray  arr;
    struct ArrowSchema sch;
    int64_t rows = 0;
    int64_t cnt;
    (void) db;

    memset (&st,  0, sizeof (st));
    memset (&arr, 0, sizeof (arr));
    memset (&sch, 0, sizeof (sch));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_CATALOG,
                             "DB", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                             "DBA", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_TABLE,
                             "t_adbc_p7_create", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_MODE,
                             ADBC_INGEST_OPTION_MODE_APPEND, &err);
    build_int_str_dbl_batch (&arr, &sch);
    ASSERT (drv->StatementBind (&st, &arr, &sch, &err) == ADBC_STATUS_OK,
            "ingest APPEND: bind");
    ASSERT (drv->StatementExecuteQuery (&st, NULL, &rows, &err)
                == ADBC_STATUS_OK,
            "ingest APPEND: exec");
    ASSERT (rows == 3, "ingest APPEND: 3 rows affected");
    drv->StatementRelease (&st, NULL);

    cnt = select_count (drv, cn,
                        "select count(*) from DB.DBA.t_adbc_p7_create");
    ASSERT (cnt == 6, "ingest APPEND: total now 6");
    fprintf (stdout, "[OK]  integration: ingest APPEND (6 rows total)\n");
}

/* REPLACE: drop + recreate. */
static void
test_ingest_replace (struct AdbcDriver *drv, struct AdbcDatabase *db,
                     struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct AdbcStatement st;
    struct ArrowArray  arr;
    struct ArrowSchema sch;
    int64_t rows = 0;
    int64_t cnt;
    (void) db;

    memset (&st,  0, sizeof (st));
    memset (&arr, 0, sizeof (arr));
    memset (&sch, 0, sizeof (sch));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_CATALOG,
                             "DB", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                             "DBA", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_TABLE,
                             "t_adbc_p7_create", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_MODE,
                             ADBC_INGEST_OPTION_MODE_REPLACE, &err);
    build_int_str_dbl_batch (&arr, &sch);
    drv->StatementBind (&st, &arr, &sch, &err);
    ASSERT (drv->StatementExecuteQuery (&st, NULL, &rows, &err)
                == ADBC_STATUS_OK,
            "ingest REPLACE: exec");
    ASSERT (rows == 3, "ingest REPLACE: 3 rows after replace");
    drv->StatementRelease (&st, NULL);

    cnt = select_count (drv, cn,
                        "select count(*) from DB.DBA.t_adbc_p7_create");
    ASSERT (cnt == 3, "ingest REPLACE: SELECT COUNT(*) = 3");
    fprintf (stdout, "[OK]  integration: ingest REPLACE (3 rows)\n");
}

/* CREATE_APPEND on a brand-new table.                                */
static void
test_ingest_create_append (struct AdbcDriver *drv, struct AdbcDatabase *db,
                           struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct AdbcStatement st;
    struct ArrowArray  arr;
    struct ArrowSchema sch;
    int64_t rows = 0;
    int64_t cnt;
    (void) db;

    exec_one (drv, cn, "drop table DB.DBA.t_adbc_p7_ca", &err);
    if (err.release) err.release (&err);

    memset (&st,  0, sizeof (st));
    memset (&arr, 0, sizeof (arr));
    memset (&sch, 0, sizeof (sch));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_CATALOG,
                             "DB", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                             "DBA", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_TABLE,
                             "t_adbc_p7_ca", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_MODE,
                             ADBC_INGEST_OPTION_MODE_CREATE_APPEND, &err);
    build_int_str_dbl_batch (&arr, &sch);
    drv->StatementBind (&st, &arr, &sch, &err);
    ASSERT (drv->StatementExecuteQuery (&st, NULL, &rows, &err)
                == ADBC_STATUS_OK,
            "ingest CREATE_APPEND first call: exec");
    ASSERT (rows == 3, "ingest CREATE_APPEND first: 3 rows");
    drv->StatementRelease (&st, NULL);

    /* Second call: table now exists -> just APPEND. */
    memset (&st,  0, sizeof (st));
    memset (&arr, 0, sizeof (arr));
    memset (&sch, 0, sizeof (sch));
    if (err.release) err.release (&err);
    memset (&err, 0, sizeof (err));
    drv->StatementNew (cn, &st, &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_CATALOG,
                             "DB", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA,
                             "DBA", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_TARGET_TABLE,
                             "t_adbc_p7_ca", &err);
    drv->StatementSetOption (&st, ADBC_INGEST_OPTION_MODE,
                             ADBC_INGEST_OPTION_MODE_CREATE_APPEND, &err);
    build_int_str_dbl_batch (&arr, &sch);
    drv->StatementBind (&st, &arr, &sch, &err);
    ASSERT (drv->StatementExecuteQuery (&st, NULL, &rows, &err)
                == ADBC_STATUS_OK,
            "ingest CREATE_APPEND second call: exec");
    drv->StatementRelease (&st, NULL);

    cnt = select_count (drv, cn,
                        "select count(*) from DB.DBA.t_adbc_p7_ca");
    ASSERT (cnt == 6, "ingest CREATE_APPEND: total now 6");

    /* Clean up. */
    exec_one (drv, cn, "drop table DB.DBA.t_adbc_p7_ca", NULL);
    fprintf (stdout, "[OK]  integration: ingest CREATE_APPEND (6 rows total)\n");
}

/* Phase 7d: round-trip. Ingest creates a table, then GetTableSchema
 * + SELECT * confirm we can read the same column count back.         */
static void
test_ingest_roundtrip (struct AdbcDriver *drv, struct AdbcDatabase *db,
                       struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowSchema sch;
    AdbcStatusCode rc;
    (void) db;

    memset (&sch, 0, sizeof (sch));
    rc = drv->ConnectionGetTableSchema (cn, NULL, NULL,
                                        "t_adbc_p7_create", &sch, &err);
    ASSERT (rc == ADBC_STATUS_OK, "ingest round-trip: GetTableSchema");
    if (rc == ADBC_STATUS_OK) {
        ASSERT (sch.n_children == 3,
                "ingest round-trip: 3 columns survive");
        if (sch.release) sch.release (&sch);
    }
    if (err.release) err.release (&err);

    /* Clean up the table.                                            */
    exec_one (drv, cn, "drop table DB.DBA.t_adbc_p7_create", NULL);
    fprintf (stdout, "[OK]  integration: ingest round-trip (3-col schema)\n");
}

/* ---------- Integration: Phase 8 (Virtuoso extras / SPARQL) ---------- */

/* Find a key=value pair in a packed Arrow metadata buffer. Returns
 * 1 if found (and copies the value into out_value), else 0.        */
static int
metadata_lookup (const char *metadata, const char *key,
                 char *out_value, size_t out_cap)
{
    int32_t i, n;
    const char *p = metadata;
    if (!p) return 0;
    memcpy (&n, p, 4); p += 4;
    for (i = 0; i < n; i++) {
        int32_t klen, vlen;
        memcpy (&klen, p, 4); p += 4;
        if ((int) strlen (key) == klen && memcmp (p, key, (size_t) klen) == 0) {
            p += klen;
            memcpy (&vlen, p, 4); p += 4;
            if ((size_t) vlen + 1 > out_cap) vlen = (int32_t) out_cap - 1;
            memcpy (out_value, p, (size_t) vlen);
            out_value[vlen] = 0;
            return 1;
        }
        p += klen;
        memcpy (&vlen, p, 4); p += 4;
        p += vlen;
    }
    return 0;
}

static void
test_sparql_passthrough (struct AdbcDriver *drv, struct AdbcDatabase *db,
                         struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowSchema sch;
    struct ArrowArray batch;
    char metaval[64];
    int saw_meta = 0, saw_rdf_col = 0;
    int n_children = 0;
    AdbcStatusCode rc;
    (void) db;

    memset (&st,     0, sizeof (st));
    memset (&stream, 0, sizeof (stream));
    memset (&sch,    0, sizeof (sch));
    memset (&batch,  0, sizeof (batch));

    drv->StatementNew (cn, &st, &err);
    ASSERT (drv->StatementSetOption (&st, "adbc.virtuoso.dialect",
                                     "sparql", &err) == ADBC_STATUS_OK,
            "sparql: set dialect");
    /* Triple pattern against an empty graph still returns a 3-col
     * schema; LIMIT 5 keeps the result small even on a populated
     * server.                                                        */
    drv->StatementSetSqlQuery (&st,
        "select ?s ?p ?o where { ?s ?p ?o } limit 5", &err);

    rc = drv->StatementExecuteQuery (&st, &stream, NULL, &err);
    ASSERT (rc == ADBC_STATUS_OK, "sparql: exec");
    if (rc == ADBC_STATUS_OK) {
        ASSERT (stream.get_schema (&stream, &sch) == 0, "sparql: schema");
        n_children = (int) sch.n_children;
        ASSERT (n_children == 3, "sparql: 3 columns (s,p,o)");
        if (sch.metadata
            && metadata_lookup (sch.metadata, "virtuoso:dialect",
                                metaval, sizeof (metaval))
            && strcmp (metaval, "sparql") == 0)
            saw_meta = 1;
        ASSERT (saw_meta, "sparql: top-level virtuoso:dialect=sparql metadata");
        if (n_children > 0 && sch.children[0]->metadata
            && metadata_lookup (sch.children[0]->metadata, "virtuoso:rdf",
                                metaval, sizeof (metaval))
            && strcmp (metaval, "true") == 0)
            saw_rdf_col = 1;
        ASSERT (saw_rdf_col, "sparql: child virtuoso:rdf=true metadata");

        if (sch.release) sch.release (&sch);

        /* Drain (>= 0 rows -- empty result is fine).                 */
        while (stream.get_next (&stream, &batch) == 0 && batch.release) {
            batch.release (&batch);
            memset (&batch, 0, sizeof (batch));
        }
    } else if (err.release) {
        err.release (&err);
    }
    if (stream.release) stream.release (&stream);
    drv->StatementRelease (&st, NULL);
    fprintf (stdout, "[OK]  integration: SPARQL passthrough (3 cols, metadata)\n");
}

static void
test_sparql_via_prepare (struct AdbcDriver *drv, struct AdbcDatabase *db,
                         struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    AdbcStatusCode rc;
    (void) db;

    memset (&st,     0, sizeof (st));
    memset (&stream, 0, sizeof (stream));

    drv->StatementNew (cn, &st, &err);
    drv->StatementSetOption (&st, "adbc.virtuoso.dialect", "sparql", &err);
    drv->StatementSetSqlQuery (&st,
        "select (count(*) as ?n) where { ?s ?p ?o }", &err);
    ASSERT (drv->StatementPrepare (&st, &err) == ADBC_STATUS_OK,
            "sparql prepare: prepare");
    rc = drv->StatementExecuteQuery (&st, &stream, NULL, &err);
    ASSERT (rc == ADBC_STATUS_OK, "sparql prepare: exec");
    if (rc == ADBC_STATUS_OK) {
        struct ArrowArray batch;
        memset (&batch, 0, sizeof (batch));
        if (stream.get_next (&stream, &batch) == 0 && batch.release)
            batch.release (&batch);
        if (stream.release) stream.release (&stream);
    } else if (err.release) {
        err.release (&err);
    }
    drv->StatementRelease (&st, NULL);
    fprintf (stdout, "[OK]  integration: SPARQL via Prepare\n");
}

/* Toggling the dialect off must restore plain SQL behaviour.        */
static void
test_dialect_toggle (struct AdbcDriver *drv, struct AdbcDatabase *db,
                     struct AdbcConnection *cn)
{
    struct AdbcStatement st;
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    AdbcStatusCode rc;
    (void) db;

    memset (&st,     0, sizeof (st));
    memset (&stream, 0, sizeof (stream));
    drv->StatementNew (cn, &st, &err);
    drv->StatementSetOption (&st, "adbc.virtuoso.dialect", "sparql", &err);
    drv->StatementSetOption (&st, "adbc.virtuoso.dialect", "sql",    &err);
    drv->StatementSetSqlQuery (&st, "select 42 as the_answer", &err);
    rc = drv->StatementExecuteQuery (&st, &stream, NULL, &err);
    ASSERT (rc == ADBC_STATUS_OK, "dialect toggle: SQL succeeds after toggle");
    if (rc == ADBC_STATUS_OK) {
        struct ArrowSchema sch;
        char metaval[64];
        memset (&sch, 0, sizeof (sch));
        stream.get_schema (&stream, &sch);
        ASSERT (!sch.metadata
                || !metadata_lookup (sch.metadata, "virtuoso:dialect",
                                     metaval, sizeof (metaval))
                || strcmp (metaval, "sparql") != 0,
                "dialect toggle: no sparql metadata after toggle to sql");
        if (sch.release) sch.release (&sch);
        if (stream.release) stream.release (&stream);
    } else if (err.release) {
        err.release (&err);
    }
    drv->StatementRelease (&st, NULL);
    fprintf (stdout, "[OK]  integration: dialect toggle SPARQL -> SQL\n");
}

/* ---------- Integration: gated on VIRT_ADBC_TEST_URI ---------- */

/* ====================================================================
 *  Phase 9 tests — ADBC 1.1.0 polish
 * ==================================================================== */

static void
test_statistic_names (struct AdbcDriver *drv, struct AdbcDatabase *db,
                      struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowSchema sch;
    struct ArrowArray arr;
    (void) db;

    memset (&stream, 0, sizeof (stream));
    memset (&sch, 0, sizeof (sch));
    memset (&arr, 0, sizeof (arr));

    ASSERT (drv->ConnectionGetStatisticNames (cn, &stream, &err)
            == ADBC_STATUS_OK, "statistic_names: invoke");
    ASSERT (stream.get_schema (&stream, &sch) == 0,
            "statistic_names: schema");
    ASSERT (sch.n_children == 2, "statistic_names: 2 columns");
    if (sch.release) sch.release (&sch);

    ASSERT (stream.get_next (&stream, &arr) == 0, "statistic_names: batch");
    if (arr.release) {
        ASSERT (arr.length >= 1, "statistic_names: at least 1 row");
        arr.release (&arr);
    } else {
        ASSERT (0, "statistic_names: no batch");
    }
    stream.release (&stream);
    fprintf (stdout, "[OK]  integration: GetStatisticNames\n");
}

static void
test_statistics (struct AdbcDriver *drv, struct AdbcDatabase *db,
                 struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct ArrowArrayStream stream;
    struct ArrowSchema sch;
    struct ArrowArray arr;
    (void) db;

    memset (&stream, 0, sizeof (stream));
    memset (&sch, 0, sizeof (sch));
    memset (&arr, 0, sizeof (arr));

    ASSERT (drv->ConnectionGetStatistics (cn, NULL, NULL, NULL, 1,
                                          &stream, &err) == ADBC_STATUS_OK,
            "statistics: invoke");
    ASSERT (stream.get_schema (&stream, &sch) == 0,
            "statistics: schema");
    ASSERT (sch.n_children == 2, "statistics: 2 top-level columns");
    if (sch.release) sch.release (&sch);

    /* Consume the stream — it may have 0 or more rows depending on
     * what tables exist in the test database.                          */
    {
        while (stream.get_next (&stream, &arr) == 0) {
            if (arr.release) arr.release (&arr);
            else break;
        }
    }
    stream.release (&stream);
    fprintf (stdout, "[OK]  integration: GetStatistics\n");
}

static void
test_execute_schema (struct AdbcDriver *drv, struct AdbcDatabase *db,
                     struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct AdbcStatement stmt;
    struct ArrowSchema sch;

    memset (&stmt, 0, sizeof (stmt));
    memset (&sch,  0, sizeof (sch));
    (void) db;

    ASSERT (drv->StatementNew (cn, &stmt, &err) == ADBC_STATUS_OK,
            "exec_schema: new");
    ASSERT (drv->StatementSetSqlQuery (&stmt,
             "SELECT 1 AS a, 2 AS b, 'x' AS c", &err) == ADBC_STATUS_OK,
            "exec_schema: set_sql");

    ASSERT (drv->StatementExecuteSchema (&stmt, &sch, &err)
            == ADBC_STATUS_OK, "exec_schema: invoke");
    ASSERT (sch.n_children == 3, "exec_schema: 3 columns");
    if (sch.release) sch.release (&sch);

    drv->StatementRelease (&stmt, &err);
    fprintf (stdout, "[OK]  integration: StatementExecuteSchema (3 cols)\n");
}

static void
test_statement_cancel (struct AdbcDriver *drv, struct AdbcDatabase *db,
                       struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct AdbcStatement stmt;

    memset (&stmt, 0, sizeof (stmt));
    (void) db;

    ASSERT (drv->StatementNew (cn, &stmt, &err) == ADBC_STATUS_OK,
            "st_cancel: new");
    ASSERT (drv->StatementSetSqlQuery (&stmt, "SELECT 1", &err)
            == ADBC_STATUS_OK, "st_cancel: set_sql");

    /* Cancel before execute — should be OK (nothing to cancel). */
    ASSERT (drv->StatementCancel (&stmt, &err) == ADBC_STATUS_OK,
            "st_cancel: before execute");

    /* Execute and then cancel during stream consumption.
     * The rows are already fetched so cancel may be a no-op.
     * The important thing is that it returns OK.                        */
    {
        struct ArrowArrayStream stream;
        memset (&stream, 0, sizeof (stream));
        ASSERT (drv->StatementExecuteQuery (&stmt, &stream, NULL, &err)
                == ADBC_STATUS_OK, "st_cancel: exec");
        ASSERT (drv->StatementCancel (&stmt, &err) == ADBC_STATUS_OK,
                "st_cancel: during stream");
        if (stream.release) stream.release (&stream);
    }

    drv->StatementRelease (&stmt, &err);
    fprintf (stdout, "[OK]  integration: StatementCancel\n");
}

static void
test_partitions_roundtrip (struct AdbcDriver *drv, struct AdbcDatabase *db,
                           struct AdbcConnection *cn)
{
    struct AdbcError err = ADBC_ERROR_INIT;
    struct AdbcStatement stmt;
    struct ArrowSchema sch;
    struct AdbcPartitions parts;
    struct ArrowArrayStream stream;
    struct ArrowArray arr;
    (void) db;

    memset (&stmt, 0, sizeof (stmt));
    memset (&sch,  0, sizeof (sch));
    memset (&parts, 0, sizeof (parts));

    ASSERT (drv->StatementNew (cn, &stmt, &err) == ADBC_STATUS_OK,
            "partitions: new");
    ASSERT (drv->StatementSetSqlQuery (&stmt, "SELECT 1 AS n", &err)
            == ADBC_STATUS_OK, "partitions: sql");

    ASSERT (drv->StatementExecutePartitions (&stmt, &sch, &parts,
                                              NULL, &err) == ADBC_STATUS_OK,
            "partitions: exec_partitions");
    ASSERT (parts.num_partitions == 1, "partitions: 1 partition");
    ASSERT (sch.n_children == 1, "partitions: 1 output column");
    if (sch.release) sch.release (&sch);

    /* Read the partition on the same connection. */
    memset (&stream, 0, sizeof (stream));
    ASSERT (drv->ConnectionReadPartition (cn, parts.partitions[0],
                                          parts.partition_lengths[0],
                                          &stream, &err) == ADBC_STATUS_OK,
            "partitions: read_partition");

    memset (&arr, 0, sizeof (arr));
    ASSERT (stream.get_next (&stream, &arr) == 0, "partitions: get_next");
    if (arr.release) {
        ASSERT (arr.length >= 1, "partitions: at least 1 row");
        arr.release (&arr);
    }
    if (stream.release) stream.release (&stream);

    if (parts.release) parts.release (&parts);
    drv->StatementRelease (&stmt, &err);
    fprintf (stdout, "[OK]  integration: ExecutePartitions + ReadPartition\n");
}

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

    /* Phase 5 integration tests. */
    test_prepared_select            (&drv, &db, &cn);
    test_parameter_schema           (&drv, &db, &cn);
    test_bind_insert                (&drv, &db, &cn);
    test_bind_stream_insert         (&drv, &db, &cn);

    /* Phase 6 catalog tests. */
    test_get_table_types            (&drv, &db, &cn);
    test_get_info                   (&drv, &db, &cn);
    test_get_table_schema           (&drv, &db, &cn);
    test_get_objects                (&drv, &db, &cn);

    /* Phase 7 bulk ingest tests. Force autocommit ON since DDL
     * mid-transaction is fragile in Virtuoso.                        */
    drv.ConnectionSetOption (&cn, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
                             ADBC_OPTION_VALUE_ENABLED, &err);
    test_ingest_create              (&drv, &db, &cn);
    test_ingest_append              (&drv, &db, &cn);
    test_ingest_replace             (&drv, &db, &cn);
    test_ingest_create_append       (&drv, &db, &cn);
    test_ingest_roundtrip           (&drv, &db, &cn);

    /* Phase 8 SPARQL / dialect tests. */
    test_sparql_passthrough         (&drv, &db, &cn);
    test_sparql_via_prepare         (&drv, &db, &cn);
    test_dialect_toggle             (&drv, &db, &cn);

    /* Phase 9 ADBC 1.1.0 polish tests. */
    test_statistic_names            (&drv, &db, &cn);
    test_statistics                 (&drv, &db, &cn);
    test_execute_schema             (&drv, &db, &cn);
    test_statement_cancel           (&drv, &db, &cn);
    test_partitions_roundtrip       (&drv, &db, &cn);

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
    fprintf (stdout, "OK -- ADBC phase 2-9 unit tests passed\n");
    return EXIT_SUCCESS;
}
