/*
 *  virtuoso_adbc.h
 *
 *  Private header for the Virtuoso ADBC driver. Defines the
 *  internal handle layout for AdbcDatabase / AdbcConnection /
 *  AdbcStatement and the helper API shared across translation units.
 *
 *  Phase 4 adds VirtAdbcStatement plus the SQL-type -> Arrow-type
 *  mapping helpers and the ArrowArrayStream factory.
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

/* Phase 9c: single-partition descriptor format used by
 * StatementExecutePartitions / ConnectionReadPartition.            */
#define VIRT_PARTITION_MAGIC    "VIRT"
#define VIRT_PARTITION_VERSION  1
#define VIRT_PARTITION_HDR_LEN  12

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
                                   *       adbc.virtuoso.charset,
                                   *       adbc.connection.timeout_s, ... */
    void               *henv;     /* virtodbc__SQLAllocEnv result        */
    int                 initialised;
} VirtAdbcDatabase;

typedef struct VirtAdbcConnection {
    VirtAdbcDatabase   *db;
    virt_adbc_option_t *opts;     /* per-connection knobs                */
    void               *hdbc;     /* virtodbc__SQLAllocConnect result    */
    int                 connected;
    int                 autocommit;
    int                 read_only;
    int                 in_txn;   /* set on first write under !autocommit */
    void               *current_hstmt; /* updated by statement.c, cleared
                                        * on completion; read by Cancel  */
    /* Phase 3 docs the contract as "serialised access" per ADBC, but we
     * defensively guard the handle with a mutex so concurrent calls
     * (notably Cancel from another thread) are safe.                    */
    void               *mu;       /* mutex_allocate() -- opaque to ADBC  */
} VirtAdbcConnection;

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
 *  Connection vtable (connection.c).
 * ------------------------------------------------------------------- */
AdbcStatusCode virt_cn_new            (struct AdbcConnection *cn,
                                       struct AdbcError *err);
AdbcStatusCode virt_cn_set_option     (struct AdbcConnection *cn, const char *key,
                                       const char *value, struct AdbcError *err);
AdbcStatusCode virt_cn_set_option_bytes (struct AdbcConnection *cn, const char *key,
                                         const uint8_t *value, size_t len,
                                         struct AdbcError *err);
AdbcStatusCode virt_cn_set_option_int (struct AdbcConnection *cn, const char *key,
                                       int64_t value, struct AdbcError *err);
AdbcStatusCode virt_cn_set_option_double (struct AdbcConnection *cn, const char *key,
                                          double value, struct AdbcError *err);
AdbcStatusCode virt_cn_get_option     (struct AdbcConnection *cn, const char *key,
                                       char *out, size_t *len,
                                       struct AdbcError *err);
AdbcStatusCode virt_cn_get_option_bytes (struct AdbcConnection *cn, const char *key,
                                         uint8_t *out, size_t *len,
                                         struct AdbcError *err);
AdbcStatusCode virt_cn_get_option_int (struct AdbcConnection *cn, const char *key,
                                       int64_t *out, struct AdbcError *err);
AdbcStatusCode virt_cn_get_option_double (struct AdbcConnection *cn, const char *key,
                                          double *out, struct AdbcError *err);
AdbcStatusCode virt_cn_init           (struct AdbcConnection *cn,
                                       struct AdbcDatabase *db,
                                       struct AdbcError *err);
AdbcStatusCode virt_cn_release        (struct AdbcConnection *cn,
                                       struct AdbcError *err);
AdbcStatusCode virt_cn_commit         (struct AdbcConnection *cn,
                                       struct AdbcError *err);
AdbcStatusCode virt_cn_rollback       (struct AdbcConnection *cn,
                                       struct AdbcError *err);
AdbcStatusCode virt_cn_cancel         (struct AdbcConnection *cn,
                                       struct AdbcError *err);

/* Phase 6: catalog / metadata.                                       */
AdbcStatusCode virt_cn_get_table_types (struct AdbcConnection *cn,
                                        struct ArrowArrayStream *out,
                                        struct AdbcError *err);
AdbcStatusCode virt_cn_get_info (struct AdbcConnection *cn,
                                 const uint32_t *info_codes,
                                 size_t info_codes_length,
                                 struct ArrowArrayStream *out,
                                 struct AdbcError *err);
AdbcStatusCode virt_cn_get_table_schema (struct AdbcConnection *cn,
                                         const char *catalog,
                                         const char *db_schema,
                                         const char *table_name,
                                         struct ArrowSchema *out,
                                         struct AdbcError *err);
AdbcStatusCode virt_cn_get_objects (struct AdbcConnection *cn, int depth,
                                    const char *catalog,
                                    const char *db_schema,
                                    const char *table_name,
                                    const char **table_type,
                                    const char *column_name,
                                    struct ArrowArrayStream *out,
                                    struct AdbcError *err);

/* -------------------------------------------------------------------
 *  Statement handle and vtable (statement.c).
 * ------------------------------------------------------------------- */

/* Forward decl -- definition in arrow_writer.c.                       */
typedef struct VirtAdbcWriter VirtAdbcWriter;

/* Phase 7: bulk-ingest modes (numeric form of the spec strings).     */
typedef enum {
    VIRT_INGEST_NONE          = 0,
    VIRT_INGEST_CREATE,
    VIRT_INGEST_APPEND,
    VIRT_INGEST_REPLACE,
    VIRT_INGEST_CREATE_APPEND
} virt_ingest_mode_t;

typedef struct VirtAdbcStatement {
    VirtAdbcConnection *cn;
    char               *sql;       /* set by SetSqlQuery; freed in Release */
    /* When prepared, hstmt holds the prepared CLI statement -- shared by
     * every Execute that runs against it. When un-prepared, hstmt is NULL
     * and ExecuteQuery uses SQLExecDirect on a fresh transient HSTMT.    */
    void               *hstmt;
    int                 prepared;
    int64_t             batch_rows;
    virt_adbc_option_t *opts;
    /* Phase 5 bind state.                                              */
    struct ArrowSchema  bound_schema;
    struct ArrowArray   bound_batch;
    int                 has_bound_batch;
    struct ArrowArrayStream bound_stream;
    int                 has_bound_stream;
    VirtAdbcWriter     *writer;     /* lazily-built on first bound Execute */
    /* Phase 7 bulk-ingest state.                                       */
    virt_ingest_mode_t  ingest_mode;
    char               *ingest_target_table;
    char               *ingest_target_catalog;
    char               *ingest_target_db_schema;
    int                 ingest_temporary;
    /* Phase 8: Virtuoso domain extras. dialect = 0 (SQL, default) or
     * 1 (SPARQL: ExecuteQuery prepends "sparql " before sending the
     * statement text on the wire, and the resulting Arrow schema
     * carries virtuoso:dialect=sparql metadata).                       */
    int                 sparql_dialect;
    /* Phase 9d: StatementCancel — updated under cn->mu before any
     * blocking SQL call; read by virt_st_cancel from another thread.   */
    void               *pending_hstmt;
} VirtAdbcStatement;

AdbcStatusCode virt_st_new (struct AdbcConnection *cn,
                            struct AdbcStatement *st, struct AdbcError *err);
AdbcStatusCode virt_st_release (struct AdbcStatement *st,
                                struct AdbcError *err);
AdbcStatusCode virt_st_set_sql_query (struct AdbcStatement *st,
                                      const char *query,
                                      struct AdbcError *err);
AdbcStatusCode virt_st_set_option (struct AdbcStatement *st, const char *key,
                                   const char *value, struct AdbcError *err);
AdbcStatusCode virt_st_set_option_bytes (struct AdbcStatement *st,
                                         const char *key, const uint8_t *value,
                                         size_t len, struct AdbcError *err);
AdbcStatusCode virt_st_set_option_int (struct AdbcStatement *st,
                                       const char *key, int64_t value,
                                       struct AdbcError *err);
AdbcStatusCode virt_st_set_option_double (struct AdbcStatement *st,
                                          const char *key, double value,
                                          struct AdbcError *err);
AdbcStatusCode virt_st_get_option (struct AdbcStatement *st, const char *key,
                                   char *out, size_t *len,
                                   struct AdbcError *err);
AdbcStatusCode virt_st_get_option_bytes (struct AdbcStatement *st,
                                         const char *key, uint8_t *out,
                                         size_t *len, struct AdbcError *err);
AdbcStatusCode virt_st_get_option_int (struct AdbcStatement *st,
                                       const char *key, int64_t *out,
                                       struct AdbcError *err);
AdbcStatusCode virt_st_get_option_double (struct AdbcStatement *st,
                                          const char *key, double *out,
                                          struct AdbcError *err);
AdbcStatusCode virt_st_execute_query (struct AdbcStatement *st,
                                      struct ArrowArrayStream *out,
                                      int64_t *rows_affected,
                                      struct AdbcError *err);

/* Phase 9: ADBC 1.1.0 polish.                                         */
AdbcStatusCode virt_st_cancel           (struct AdbcStatement *st,
                                         struct AdbcError *err);
AdbcStatusCode virt_st_execute_schema   (struct AdbcStatement *st,
                                         struct ArrowSchema *out,
                                         struct AdbcError *err);
AdbcStatusCode virt_st_execute_partitions (struct AdbcStatement *st,
                                           struct ArrowSchema *schema,
                                           struct AdbcPartitions *partitions,
                                           int64_t *rows_affected,
                                           struct AdbcError *err);

/* Phase 9: Connection-level 1.1.0 additions.                           */
AdbcStatusCode virt_cn_get_statistics (struct AdbcConnection *cn,
                                       const char *catalog,
                                       const char *db_schema,
                                       const char *table_name,
                                       char approximate,
                                       struct ArrowArrayStream *out,
                                       struct AdbcError *err);
AdbcStatusCode virt_cn_get_statistic_names (struct AdbcConnection *cn,
                                            struct ArrowArrayStream *out,
                                            struct AdbcError *err);
AdbcStatusCode virt_cn_read_partition (struct AdbcConnection *cn,
                                       const uint8_t *serialized,
                                       size_t length,
                                       struct ArrowArrayStream *out,
                                       struct AdbcError *err);

/* Phase 5: prepared statements and parameter binding.                */
AdbcStatusCode virt_st_prepare (struct AdbcStatement *st,
                                struct AdbcError *err);
AdbcStatusCode virt_st_bind (struct AdbcStatement *st,
                             struct ArrowArray *values,
                             struct ArrowSchema *schema,
                             struct AdbcError *err);
AdbcStatusCode virt_st_bind_stream (struct AdbcStatement *st,
                                    struct ArrowArrayStream *stream,
                                    struct AdbcError *err);
AdbcStatusCode virt_st_get_parameter_schema (struct AdbcStatement *st,
                                             struct ArrowSchema *schema,
                                             struct AdbcError *err);

/* Writer factory (arrow_writer.c).                                   */
AdbcStatusCode virt_writer_create (void *hstmt, struct ArrowSchema *bind_schema,
                                   VirtAdbcWriter **out_writer,
                                   struct AdbcError *err);
void           virt_writer_destroy (VirtAdbcWriter *w);
AdbcStatusCode virt_writer_execute_batch (VirtAdbcWriter *w,
                                          struct ArrowArray *batch,
                                          int64_t *rows_affected_inout,
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
