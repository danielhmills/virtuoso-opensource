/*
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  This project is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; only version 2 of the License, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 *  error.c
 *
 *  AdbcError construction, release, and SQLSTATE-to-status mapping for
 *  the Virtuoso ADBC driver.
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the GNU GPL v2; see COPYING in the project root.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql.h"
#include "sqlext.h"

#include "virtuoso_adbc.h"

/* ------------------------------------------------------------------ */
/* AdbcError release                                                  */
/* ------------------------------------------------------------------ */

static void
virt_err_release_cb (struct AdbcError *err)
{
    if (!err)
        return;
    if (err->message) {
        free (err->message);
        err->message = NULL;
    }
    err->vendor_code = 0;
    memset (err->sqlstate, 0, sizeof (err->sqlstate));
    err->release = NULL;
}

void
virt_err_clear (struct AdbcError *err)
{
    if (!err)
        return;
    memset (err, 0, sizeof (*err));
}

void
virt_err_release (struct AdbcError *err)
{
    if (err && err->release)
        err->release (err);
}

/* ------------------------------------------------------------------ */
/* SQLSTATE -> ADBC status code                                       */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_sqlstate_to_adbc (const char *sqlstate)
{
    if (!sqlstate || sqlstate[0] == '\0')
        return ADBC_STATUS_INTERNAL;

    /* Class is the first two characters of the SQLSTATE. */
    if (sqlstate[0] == '0' && sqlstate[1] == '8')
        return ADBC_STATUS_IO;             /* connection exception   */
    if (sqlstate[0] == '2' && sqlstate[1] == '2')
        return ADBC_STATUS_INVALID_DATA;   /* data exception         */
    if (sqlstate[0] == '2' && sqlstate[1] == '3')
        return ADBC_STATUS_ALREADY_EXISTS; /* integrity violation    */
    if (sqlstate[0] == '2' && sqlstate[1] == '8')
        return ADBC_STATUS_UNAUTHENTICATED;
    if (sqlstate[0] == '4' && sqlstate[1] == '0')
        return ADBC_STATUS_IO;             /* transaction rollback   */
    if (sqlstate[0] == '4' && sqlstate[1] == '2')
        return ADBC_STATUS_INVALID_ARGUMENT; /* syntax/access rule   */
    if (memcmp (sqlstate, "HY008", 5) == 0)
        return ADBC_STATUS_CANCELLED;
    if (memcmp (sqlstate, "HYT", 3) == 0)
        return ADBC_STATUS_TIMEOUT;
    if (memcmp (sqlstate, "HYC", 3) == 0)
        return ADBC_STATUS_NOT_IMPLEMENTED;
    return ADBC_STATUS_INTERNAL;
}

/* ------------------------------------------------------------------ */
/* virt_err_set: printf-style error construction                       */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_err_set (struct AdbcError *err, AdbcStatusCode rc,
              const char *sqlstate, int32_t vendor_code,
              const char *fmt, ...)
{
    va_list ap;
    int len;
    char *buf = NULL;

    if (!err)
        return rc;

    /* If error was previously set, release it first. */
    if (err->release)
        err->release (err);

    memset (err->sqlstate, 0, sizeof (err->sqlstate));
    if (sqlstate) {
        size_t n = strlen (sqlstate);
        if (n > 5) n = 5;
        memcpy (err->sqlstate, sqlstate, n);
    }
    err->vendor_code = vendor_code;

    if (fmt) {
        va_start (ap, fmt);
        len = vsnprintf (NULL, 0, fmt, ap);
        va_end (ap);
        if (len < 0)
            len = 0;
        buf = (char *) malloc ((size_t) len + 1);
        if (buf) {
            va_start (ap, fmt);
            vsnprintf (buf, (size_t) len + 1, fmt, ap);
            va_end (ap);
        }
    }
    err->message = buf;
    err->release = virt_err_release_cb;
    return rc;
}

/* ------------------------------------------------------------------ */
/* virt_err_from_odbc: pull diagnostic from a Virtuoso CLI handle      */
/* ------------------------------------------------------------------ */

extern SQLRETURN SQL_API virtodbc__SQLError (SQLHENV henv, SQLHDBC hdbc,
                                             SQLHSTMT hstmt,
                                             SQLCHAR *szSqlState,
                                             SQLINTEGER *pfNativeError,
                                             SQLCHAR *szErrorMsg,
                                             SQLSMALLINT cbErrorMsgMax,
                                             SQLSMALLINT *pcbErrorMsg);

AdbcStatusCode
virt_err_from_odbc (AdbcStatusCode default_rc, void *henv, void *hdbc,
                    void *hstmt, struct AdbcError *err)
{
    SQLCHAR sqlstate[6] = { 0 };
    SQLCHAR msg[1024];
    SQLINTEGER native = 0;
    SQLSMALLINT msglen = 0;
    SQLRETURN sr;
    AdbcStatusCode rc;

    if (!err)
        return default_rc;

    msg[0] = '\0';
    sr = virtodbc__SQLError ((SQLHENV) henv, (SQLHDBC) hdbc, (SQLHSTMT) hstmt,
                             sqlstate, &native, msg, sizeof (msg) - 1, &msglen);
    if (sr != SQL_SUCCESS && sr != SQL_SUCCESS_WITH_INFO) {
        /* No diagnostic available; emit a generic message. */
        return virt_err_set (err, default_rc, NULL, 0,
                             "Virtuoso reported an unspecified error (rc=%d)",
                             (int) sr);
    }

    rc = virt_sqlstate_to_adbc ((const char *) sqlstate);
    if (rc == ADBC_STATUS_INTERNAL && default_rc != ADBC_STATUS_OK)
        rc = default_rc;

    return virt_err_set (err, rc, (const char *) sqlstate, (int32_t) native,
                         "%s", (const char *) msg);
}

/* ------------------------------------------------------------------ */
/* ADBC 1.1.0 ErrorGetDetail* hooks (driver-level).                   */
/*                                                                    */
/* We do not currently attach structured details, so these always     */
/* return zero/empty. The slots are still populated in the dispatch   */
/* table so the driver manager does not synthesise NOT_IMPLEMENTED.   */
/* ------------------------------------------------------------------ */

int
virt_err_detail_count (const struct AdbcError *err)
{
    (void) err;
    return 0;
}

struct AdbcErrorDetail
virt_err_detail_at (const struct AdbcError *err, int idx)
{
    struct AdbcErrorDetail d = { NULL, NULL, 0 };
    (void) err;
    (void) idx;
    return d;
}
