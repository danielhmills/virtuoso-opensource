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
 *  options.c
 *
 *  Generic typed-option storage shared by AdbcDatabase, AdbcConnection,
 *  and (later) AdbcStatement. Keys are strings; values may be string,
 *  bytes, int64, or double, matching the four ADBC 1.1.0 typed
 *  Set/GetOption variants.
 *
 *  Implementation is a small singly-linked list. ADBC options are
 *  typically a handful of keys per handle, so the list-vs-hashmap
 *  trade-off is fine.
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2026 OpenLink Software
 *
 *  Licensed under the GNU GPL v2; see COPYING in the project root.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "virtuoso_adbc.h"

static virt_adbc_option_t *
opt_find_mut (virt_adbc_option_t *head, const char *key)
{
    while (head) {
        if (strcmp (head->key, key) == 0)
            return head;
        head = head->next;
    }
    return NULL;
}

const virt_adbc_option_t *
virt_opt_find (virt_adbc_option_t *head, const char *key)
{
    return opt_find_mut (head, key);
}

static void
opt_reset_value (virt_adbc_option_t *opt)
{
    if (!opt) return;
    if (opt->sval) { free (opt->sval); opt->sval = NULL; }
    if (opt->bval) { free (opt->bval); opt->bval = NULL; }
    opt->blen = 0;
    opt->ival = 0;
    opt->dval = 0.0;
}

static virt_adbc_option_t *
opt_get_or_create (virt_adbc_option_t **head, const char *key,
                   struct AdbcError *err)
{
    virt_adbc_option_t *opt;

    opt = opt_find_mut (*head, key);
    if (opt) {
        opt_reset_value (opt);
        return opt;
    }

    opt = (virt_adbc_option_t *) calloc (1, sizeof (*opt));
    if (!opt) {
        virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                      "out of memory allocating option '%s'", key);
        return NULL;
    }
    opt->key = strdup (key);
    if (!opt->key) {
        free (opt);
        virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                      "out of memory storing option key '%s'", key);
        return NULL;
    }
    opt->next = *head;
    *head = opt;
    return opt;
}

void
virt_opt_free_all (virt_adbc_option_t **head)
{
    virt_adbc_option_t *cur, *next;

    if (!head)
        return;
    for (cur = *head; cur; cur = next) {
        next = cur->next;
        opt_reset_value (cur);
        free (cur->key);
        free (cur);
    }
    *head = NULL;
}

/* ------------------------------------------------------------------ */
/* Setters                                                            */
/* ------------------------------------------------------------------ */

AdbcStatusCode
virt_opt_set_string (virt_adbc_option_t **head, const char *key,
                     const char *value, struct AdbcError *err)
{
    virt_adbc_option_t *opt;

    if (!head || !key)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "virt_opt_set_string: NULL key");

    opt = opt_get_or_create (head, key, err);
    if (!opt)
        return ADBC_STATUS_INTERNAL;

    opt->kind = VIRT_ADBC_OPT_STRING;
    if (value) {
        opt->sval = strdup (value);
        if (!opt->sval)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "out of memory storing option '%s'", key);
    }
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_opt_set_bytes (virt_adbc_option_t **head, const char *key,
                    const uint8_t *value, size_t len, struct AdbcError *err)
{
    virt_adbc_option_t *opt;

    if (!head || !key)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "virt_opt_set_bytes: NULL key");

    opt = opt_get_or_create (head, key, err);
    if (!opt)
        return ADBC_STATUS_INTERNAL;

    opt->kind = VIRT_ADBC_OPT_BYTES;
    if (len > 0) {
        opt->bval = (uint8_t *) malloc (len);
        if (!opt->bval)
            return virt_err_set (err, ADBC_STATUS_INTERNAL, NULL, 0,
                                 "out of memory storing bytes for '%s'", key);
        memcpy (opt->bval, value, len);
        opt->blen = len;
    }
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_opt_set_int (virt_adbc_option_t **head, const char *key, int64_t value,
                  struct AdbcError *err)
{
    virt_adbc_option_t *opt;

    if (!head || !key)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "virt_opt_set_int: NULL key");

    opt = opt_get_or_create (head, key, err);
    if (!opt)
        return ADBC_STATUS_INTERNAL;

    opt->kind = VIRT_ADBC_OPT_INT;
    opt->ival = value;
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_opt_set_double (virt_adbc_option_t **head, const char *key, double value,
                     struct AdbcError *err)
{
    virt_adbc_option_t *opt;

    if (!head || !key)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "virt_opt_set_double: NULL key");

    opt = opt_get_or_create (head, key, err);
    if (!opt)
        return ADBC_STATUS_INTERNAL;

    opt->kind = VIRT_ADBC_OPT_DOUBLE;
    opt->dval = value;
    return ADBC_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Getters. Follow the ADBC convention for string/bytes: if the input
 * buffer is too small, return the required size in *len and
 * ADBC_STATUS_OK with the buffer truncated (null-terminated for
 * strings). Pass *len = 0 with out = NULL to query the size first.
 * ------------------------------------------------------------------ */

AdbcStatusCode
virt_opt_get_string (virt_adbc_option_t *head, const char *key,
                     char *out, size_t *len, struct AdbcError *err)
{
    const virt_adbc_option_t *opt;
    size_t need;

    if (!key || !len)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "virt_opt_get_string: NULL key/len");

    opt = virt_opt_find (head, key);
    if (!opt)
        return virt_err_set (err, ADBC_STATUS_NOT_FOUND, NULL, 0,
                             "option '%s' is not set", key);
    if (opt->kind != VIRT_ADBC_OPT_STRING)
        return virt_err_set (err, ADBC_STATUS_NOT_FOUND, NULL, 0,
                             "option '%s' is not a string", key);

    need = (opt->sval ? strlen (opt->sval) : 0) + 1;
    if (out && *len > 0) {
        size_t cp = need <= *len ? need : *len;
        if (opt->sval)
            memcpy (out, opt->sval, cp - 1);
        out[cp - 1] = '\0';
    }
    *len = need;
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_opt_get_bytes (virt_adbc_option_t *head, const char *key,
                    uint8_t *out, size_t *len, struct AdbcError *err)
{
    const virt_adbc_option_t *opt;

    if (!key || !len)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "virt_opt_get_bytes: NULL key/len");

    opt = virt_opt_find (head, key);
    if (!opt)
        return virt_err_set (err, ADBC_STATUS_NOT_FOUND, NULL, 0,
                             "option '%s' is not set", key);
    if (opt->kind != VIRT_ADBC_OPT_BYTES)
        return virt_err_set (err, ADBC_STATUS_NOT_FOUND, NULL, 0,
                             "option '%s' is not bytes", key);

    if (out && *len >= opt->blen)
        memcpy (out, opt->bval, opt->blen);
    *len = opt->blen;
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_opt_get_int (virt_adbc_option_t *head, const char *key, int64_t *out,
                  struct AdbcError *err)
{
    const virt_adbc_option_t *opt;

    if (!key || !out)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "virt_opt_get_int: NULL key/out");

    opt = virt_opt_find (head, key);
    if (!opt)
        return virt_err_set (err, ADBC_STATUS_NOT_FOUND, NULL, 0,
                             "option '%s' is not set", key);
    if (opt->kind != VIRT_ADBC_OPT_INT)
        return virt_err_set (err, ADBC_STATUS_NOT_FOUND, NULL, 0,
                             "option '%s' is not an integer", key);
    *out = opt->ival;
    return ADBC_STATUS_OK;
}

AdbcStatusCode
virt_opt_get_double (virt_adbc_option_t *head, const char *key, double *out,
                     struct AdbcError *err)
{
    const virt_adbc_option_t *opt;

    if (!key || !out)
        return virt_err_set (err, ADBC_STATUS_INVALID_ARGUMENT, NULL, 0,
                             "virt_opt_get_double: NULL key/out");

    opt = virt_opt_find (head, key);
    if (!opt)
        return virt_err_set (err, ADBC_STATUS_NOT_FOUND, NULL, 0,
                             "option '%s' is not set", key);
    if (opt->kind != VIRT_ADBC_OPT_DOUBLE)
        return virt_err_set (err, ADBC_STATUS_NOT_FOUND, NULL, 0,
                             "option '%s' is not a double", key);
    *out = opt->dval;
    return ADBC_STATUS_OK;
}
