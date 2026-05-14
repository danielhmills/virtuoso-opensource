/*
 *  virtuoso_adbc.h
 *
 *  Private header for the Virtuoso ADBC driver. Defines the
 *  internal handle layout for AdbcDatabase and the helper API
 *  shared across translation units.
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the GNU GPL v2; see COPYING in the project root.
 */

#ifndef VIRTUOSO_ADBC_H
#define VIRTUOSO_ADBC_H

#include <stddef.h>
#include <stdint.h>

#include "adbc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIRT_ADBC_DRIVER_NAME    "ADBC Driver for Virtuoso"
#define VIRT_ADBC_DRIVER_VERSION "0.0.1-dev"

/* -------------------------------------------------------------------
 *  Option storage. A small linked list keyed by string; values are
 *  one of {string, bytes, int, double}.
 * ------------------------------------------------------------------- */

typedef enum {
    VIRT_ADBC_OPT_STRING = 1,
    VIRT_ADBC_OPT_BYTES,
    VIRT_ADBC_OPT_INT,
    VIRT_ADBC_OPT_DOUBLE
} virt_adbc_opt_kind_t;

typedef struct virt_adbc_option {
    struct virt_adbc_option *next;
    char                    *key;
    virt_adbc_opt_kind_t     kind;
    char                    *sval;     /* VIRT_ADBC_OPT_STRING */
    uint8_t                 *bval;     /* VIRT_ADBC_OPT_BYTES  */
    size_t                   blen;
    int64_t                  ival;     /* VIRT_ADBC_OPT_INT    */
    double                   dval;     /* VIRT_ADBC_OPT_DOUBLE */
} virt_adbc_option_t;

void virt_opt_free_all (virt_adbc_option_t **head);
AdbcStatusCode virt_opt_set_string (virt_adbc_option_t **head, const char *key,
                                    const char *value, struct AdbcError *err);
AdbcStatusCode virt_opt_set_bytes  (virt_adbc_option_t **head, const char *key,
                                    const uint8_t *value, size_t len,
                                    struct AdbcError *err);
AdbcStatusCode virt_opt_set_int    (virt_adbc_option_t **head, const char *key,
                                    int64_t value, struct AdbcError *err);
AdbcStatusCode virt_opt_set_double (virt_adbc_option_t **head, const char *key,
                                    double value, struct AdbcError *err);

const virt_adbc_option_t *virt_opt_find (virt_adbc_option_t *head,
                                         const char *key);

AdbcStatusCode virt_opt_get_string (virt_adbc_option_t *head, const char *key,
                                    char *out, size_t *len,
                                    struct AdbcError *err);
AdbcStatusCode virt_opt_get_bytes  (virt_adbc_option_t *head, const char *key,
                                    uint8_t *out, size_t *len,
                                    struct AdbcError *err);
AdbcStatusCode virt_opt_get_int    (virt_adbc_option_t *head, const char *key,
                                    int64_t *out, struct AdbcError *err);
AdbcStatusCode virt_opt_get_double (virt_adbc_option_t *head, const char *key,
                                    double *out, struct AdbcError *err);

/* -------------------------------------------------------------------
 *  Error helpers. Centralise AdbcError construction so that all
 *  callers report consistent SQLSTATE + vendor code + message.
 * ------------------------------------------------------------------- */

void virt_err_clear   (struct AdbcError *err);
void virt_err_release (struct AdbcError *err);

/* fmt is printf-style; sqlstate may be NULL (driver-internal errors). */
AdbcStatusCode virt_err_set (struct AdbcError *err, AdbcStatusCode rc,
                             const char *sqlstate, int32_t vendor_code,
                             const char *fmt, ...);

/* Capture the latest error from a Virtuoso ODBC handle (env/dbc/stmt)
 * into an AdbcError. Returns the rc value passed in so callers can do
 *   return virt_err_from_odbc(rc, henv, hdbc, hstmt, err);
 */
AdbcStatusCode virt_err_from_odbc (AdbcStatusCode default_rc, void *henv,
                                   void *hdbc, void *hstmt,
                                   struct AdbcError *err);

/* Map a SQLSTATE (e.g. "08001") to a coarse ADBC_STATUS_* code. */
AdbcStatusCode virt_sqlstate_to_adbc (const char *sqlstate);

/* Bind error-vtable function pointers (ErrorGetDetail*) from the
 * dispatch table. */
int            virt_err_detail_count (const struct AdbcError *err);
struct AdbcErrorDetail
               virt_err_detail_at    (const struct AdbcError *err, int idx);

/* -------------------------------------------------------------------
 *  Handle layouts.
 * ------------------------------------------------------------------- */

typedef struct VirtAdbcDatabase {
    virt_adbc_option_t *opts;     /* keys: uri, username, password,
                                   *       adbc.virtuoso.charset, ... */
    void               *henv;     /* virtodbc__SQLAllocEnv result        */
    int                 initialised;
} VirtAdbcDatabase;

/* -------------------------------------------------------------------
 *  Database vtable (database.c).
 * ------------------------------------------------------------------- */
AdbcStatusCode virt_db_new            (struct AdbcDatabase *db,
                                       struct AdbcError *err);
AdbcStatusCode virt_db_set_option     (struct AdbcDatabase *db, const char *key,
                                       const char *value, struct AdbcError *err);
AdbcStatusCode virt_db_set_option_bytes (struct AdbcDatabase *db, const char *key,
                                         const uint8_t *value, size_t len,
                                         struct AdbcError *err);
AdbcStatusCode virt_db_set_option_int (struct AdbcDatabase *db, const char *key,
                                       int64_t value, struct AdbcError *err);
AdbcStatusCode virt_db_set_option_double (struct AdbcDatabase *db, const char *key,
                                          double value, struct AdbcError *err);
AdbcStatusCode virt_db_get_option     (struct AdbcDatabase *db, const char *key,
                                       char *out, size_t *len,
                                       struct AdbcError *err);
AdbcStatusCode virt_db_get_option_bytes (struct AdbcDatabase *db, const char *key,
                                         uint8_t *out, size_t *len,
                                         struct AdbcError *err);
AdbcStatusCode virt_db_get_option_int (struct AdbcDatabase *db, const char *key,
                                       int64_t *out, struct AdbcError *err);
AdbcStatusCode virt_db_get_option_double (struct AdbcDatabase *db, const char *key,
                                          double *out, struct AdbcError *err);
AdbcStatusCode virt_db_init           (struct AdbcDatabase *db,
                                       struct AdbcError *err);
AdbcStatusCode virt_db_release        (struct AdbcDatabase *db,
                                       struct AdbcError *err);

/* -------------------------------------------------------------------
 *  Connection-string construction. Exposed so unit tests can verify
 *  the URI parser without needing a live server.
 *
 *  Builds an ODBC-style connection string from the option map of a
 *  database handle. *out is malloc()'d; caller must free.
 * ------------------------------------------------------------------- */
AdbcStatusCode virt_build_connstr (virt_adbc_option_t *db_opts,
                                   char **out, struct AdbcError *err);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUOSO_ADBC_H */
