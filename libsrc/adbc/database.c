/*
 *  database.c
 *
 *  AdbcDatabase lifecycle for the Virtuoso ADBC driver.
 *
 *  Phase 2 surface:
 *    DatabaseNew         allocate VirtAdbcDatabase, store on db->private_data
 *    DatabaseSetOption*  store typed option (string/bytes/int/double)
 *    DatabaseGetOption*  retrieve typed option
 *    DatabaseInit        parse uri options, allocate a Virtuoso CLI env
 *    DatabaseRelease     free env + options
 *
 *  No actual SQL connection is opened here -- that is per-AdbcConnection.
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

#include "virtuoso_adbc.h"

extern SQLRETURN SQL_API virtodbc__SQLAllocEnv (SQLHENV *);
extern SQLRETURN SQL_API virtodbc__SQLFreeEnv  (SQLHENV);

/* ------------------------------------------------------------------ */
/* URI parser. Recognises:                                            */
/*   virtuoso://user:pass@host:port                                   */
/*   virtuoso://host:port                                             */
/*   //host:port                                                      */
/*   host:port                                                        */
/* On success, fills *out_host (malloc'd, "host:port" or "host"),     */
/* and optionally *out_user, *out_pass (malloc'd, may be NULL).       */
/* Caller frees with free().                                          */
/* ------------------------------------------------------------------ */

static AdbcStatusCode
parse_uri (const char *uri, char **out_host, char **out_user, char **out_pass,
           struct AdbcError *err)
{
    const char *p = uri, *at, *colon, *userpart, *hostpart;
    char *host = NULL, *user = NULL, *pass = NULL;

    *out_host = NULL;
    *out_user = NULL;
    *out_pass = NULL;

    if (!uri || !*uri)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "empty URI");

    /* Strip a scheme prefix if present (virtuoso:// or generic //). */
    {
        const char *colon_slash = strstr (p, "://");
        if (colon_slash)
            p = colon_slash + 3;
        else if (p[0] == '/' && p[1] == '/')
            p += 2;
    }

    at = strchr (p, '@');
    if (at) {
        userpart = p;
        hostpart = at + 1;
        colon = memchr (userpart, ':', (size_t) (at - userpart));
        if (colon) {
            user = strndup (userpart, (size_t) (colon - userpart));
            pass = strndup (colon + 1, (size_t) (at - (colon + 1)));
        } else {
            user = strndup (userpart, (size_t) (at - userpart));
        }
    } else {
        hostpart = p;
    }

    if (!*hostpart) {
        free (user); free (pass);
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "URI '%s' has no host", uri);
    }
    host = strdup (hostpart);
    if (!host) {
        free (user); free (pass);
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory parsing URI");
    }

    *out_host = host;
    *out_user = user;
    *out_pass = pass;
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* virt_build_connstr: turn the option map into a Virtuoso ODBC       */
/* connection string suitable for SQLDriverConnect:                   */
/*    "HOST=h:p;UID=u;PWD=p;CHARSET=...;"                             */
/* ------------------------------------------------------------------ */

static int
append_kv (char **buf, size_t *cap, size_t *off, const char *key,
           const char *value)
{
    size_t klen = strlen (key);
    size_t vlen = strlen (value);
    size_t need = *off + klen + 1 + vlen + 1 + 1;  /* k=v;\0 */
    if (need > *cap) {
        size_t nc = *cap ? *cap : 64;
        char *nb;
        while (nc < need)
            nc *= 2;
        nb = (char *) realloc (*buf, nc);
        if (!nb)
            return -1;
        *buf = nb;
        *cap = nc;
    }
    memcpy (*buf + *off, key, klen); *off += klen;
    (*buf)[(*off)++] = '=';
    memcpy (*buf + *off, value, vlen); *off += vlen;
    (*buf)[(*off)++] = ';';
    (*buf)[*off] = '\0';
    return 0;
}

AdbcStatusCode
virt_build_connstr (virt_adbc_option_t *opts, char **out,
                    struct AdbcError *err)
{
    const virt_adbc_option_t *o;
    char *host_from_uri = NULL, *user_from_uri = NULL, *pass_from_uri = NULL;
    const char *host = NULL, *user = NULL, *pass = NULL, *charset = NULL;
    char *buf = NULL;
    size_t cap = 0, off = 0;
    AdbcStatusCode rc;

    *out = NULL;

    /* uri (optional) splits into HOST/UID/PWD components. */
    o = virt_opt_find (opts, ADBC_OPTION_URI);
    if (o && o->kind == VIRT_ADBC_OPT_STRING && o->sval) {
        rc = parse_uri (o->sval, &host_from_uri, &user_from_uri,
                        &pass_from_uri, err);
        if (rc != ADBC_STATUS_OK)
            return rc;
        host = host_from_uri;
        user = user_from_uri;
        pass = pass_from_uri;
    }

    /* Explicit username/password override the URI's. */
    o = virt_opt_find (opts, ADBC_OPTION_USERNAME);
    if (o && o->kind == VIRT_ADBC_OPT_STRING && o->sval)
        user = o->sval;
    o = virt_opt_find (opts, ADBC_OPTION_PASSWORD);
    if (o && o->kind == VIRT_ADBC_OPT_STRING && o->sval)
        pass = o->sval;
    o = virt_opt_find (opts, "adbc.virtuoso.charset");
    if (o && o->kind == VIRT_ADBC_OPT_STRING && o->sval)
        charset = o->sval;

    if (!host || !*host) {
        free (host_from_uri); free (user_from_uri); free (pass_from_uri);
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "no host configured (set 'uri')");
    }

    if (append_kv (&buf, &cap, &off, "HOST", host) < 0)
        goto oom;
    if (user && append_kv (&buf, &cap, &off, "UID", user) < 0)
        goto oom;
    if (pass && append_kv (&buf, &cap, &off, "PWD", pass) < 0)
        goto oom;
    if (charset && append_kv (&buf, &cap, &off, "CHARSET", charset) < 0)
        goto oom;

    free (host_from_uri); free (user_from_uri); free (pass_from_uri);
    *out = buf;
    return ADBC_STATUS_OK;

oom:
    free (host_from_uri); free (user_from_uri); free (pass_from_uri);
    free (buf);
    return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                         "out of memory building connection string");
}

/* ------------------------------------------------------------------ */
/* AdbcDatabase entrypoints                                           */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_db_new (struct AdbcDatabase *db, struct AdbcError *err)
{
    VirtAdbcDatabase *self;

    if (!db)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "DatabaseNew: NULL database");
    if (db->private_data)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "DatabaseNew called twice on the same handle");

    self = (VirtAdbcDatabase *) calloc (1, sizeof (*self));
    if (!self)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "out of memory");
    db->private_data = self;
    return ADBC_STATUS_OK;
}

#define DB_SELF(db, err)                                                 \
    VirtAdbcDatabase *self;                                              \
    if (!(db) || !(db)->private_data)                                    \
        return virt_err_set ((err), ADBC_STATUS_INVALID_STATE, NULL, 0,  \
                             "database handle is not initialised");     \
    self = (VirtAdbcDatabase *) (db)->private_data

AdbcStatusCode
virt_db_set_option (struct AdbcDatabase *db, const char *key,
                    const char *value, struct AdbcError *err)
{
    DB_SELF (db, err);
    if (self->initialised)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "cannot set options after DatabaseInit");
    return virt_opt_set_string (&self->opts, key, value, err);
}

AdbcStatusCode
virt_db_set_option_bytes (struct AdbcDatabase *db, const char *key,
                          const uint8_t *value, size_t len,
                          struct AdbcError *err)
{
    DB_SELF (db, err);
    if (self->initialised)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "cannot set options after DatabaseInit");
    return virt_opt_set_bytes (&self->opts, key, value, len, err);
}

AdbcStatusCode
virt_db_set_option_int (struct AdbcDatabase *db, const char *key,
                        int64_t value, struct AdbcError *err)
{
    DB_SELF (db, err);
    if (self->initialised)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "cannot set options after DatabaseInit");
    return virt_opt_set_int (&self->opts, key, value, err);
}

AdbcStatusCode
virt_db_set_option_double (struct AdbcDatabase *db, const char *key,
                           double value, struct AdbcError *err)
{
    DB_SELF (db, err);
    if (self->initialised)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "cannot set options after DatabaseInit");
    return virt_opt_set_double (&self->opts, key, value, err);
}

AdbcStatusCode
virt_db_get_option (struct AdbcDatabase *db, const char *key,
                    char *out, size_t *len, struct AdbcError *err)
{
    DB_SELF (db, err);
    return virt_opt_get_string (self->opts, key, out, len, err);
}

AdbcStatusCode
virt_db_get_option_bytes (struct AdbcDatabase *db, const char *key,
                          uint8_t *out, size_t *len, struct AdbcError *err)
{
    DB_SELF (db, err);
    return virt_opt_get_bytes (self->opts, key, out, len, err);
}

AdbcStatusCode
virt_db_get_option_int (struct AdbcDatabase *db, const char *key,
                        int64_t *out, struct AdbcError *err)
{
    DB_SELF (db, err);
    return virt_opt_get_int (self->opts, key, out, err);
}

AdbcStatusCode
virt_db_get_option_double (struct AdbcDatabase *db, const char *key,
                           double *out, struct AdbcError *err)
{
    DB_SELF (db, err);
    return virt_opt_get_double (self->opts, key, out, err);
}

AdbcStatusCode
virt_db_init (struct AdbcDatabase *db, struct AdbcError *err)
{
    SQLHENV henv = SQL_NULL_HENV;
    SQLRETURN sr;
    DB_SELF (db, err);

    if (self->initialised)
        return virt_err_set (err, ADBC_STATUS_INVALID_STATE, NULL, 0,
                             "DatabaseInit called twice");

    /* Validate that a connection string can be built; this catches
     * missing-host misconfigurations before we allocate an env. */
    {
        char *probe = NULL;
        AdbcStatusCode rc = virt_build_connstr (self->opts, &probe, err);
        if (rc != ADBC_STATUS_OK)
            return rc;
        free (probe);
    }

    sr = virtodbc__SQLAllocEnv (&henv);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO)
        return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                             "virtodbc__SQLAllocEnv failed (rc=%d)", (int) sr);

    self->henv = (void *) henv;
    self->initialised = 1;
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_db_release (struct AdbcDatabase *db, struct AdbcError *err)
{
    VirtAdbcDatabase *self;

    if (!db)
        return ADBC_STATUS_OK;
    self = (VirtAdbcDatabase *) db->private_data;
    if (!self)
        return ADBC_STATUS_OK;

    if (self->henv) {
        virtodbc__SQLFreeEnv ((SQLHENV) self->henv);
        self->henv = NULL;
    }
    virt_opt_free_all (&self->opts);
    free (self);
    db->private_data = NULL;
    (void) err;
    return ADBC_STATUS_OK;
}
