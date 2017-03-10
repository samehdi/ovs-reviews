/* Copyright (c) 2009, 2010, 2011, 2012, 2013, 2016, 2017 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "file.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "bitmap.h"
#include "column.h"
#include "log.h"
#include "openvswitch/json.h"
#include "lockfile.h"
#include "ovsdb.h"
#include "ovsdb-error.h"
#include "row.h"
#include "socket-util.h"
#include "table.h"
#include "timeval.h"
#include "transaction.h"
#include "uuid.h"
#include "util.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(ovsdb_file);

/* Minimum number of milliseconds between database compactions. */
#define COMPACT_MIN_MSEC        (10 * 60 * 1000) /* 10 minutes. */

/* Minimum number of milliseconds between trying to compact the database if
 * compacting fails. */
#define COMPACT_RETRY_MSEC      (60 * 1000)      /* 1 minute. */

/* A transaction being converted to JSON for writing to a file. */
struct ovsdb_file_txn {
    struct json *json;          /* JSON for the whole transaction. */
    struct json *table_json;    /* JSON for 'table''s transaction. */
    struct ovsdb_table *table;  /* Table described in 'table_json'.  */
};

static void ovsdb_file_txn_init(struct ovsdb_file_txn *);
static void ovsdb_file_txn_add_row(struct ovsdb_file_txn *,
                                   const struct ovsdb_row *old,
                                   const struct ovsdb_row *new,
                                   const unsigned long int *changed);
static struct ovsdb_error *ovsdb_file_txn_commit(struct json *,
                                                 const char *comment,
                                                 bool durable,
                                                 struct ovsdb_log *);

static struct ovsdb_error *ovsdb_file_txn_from_json(
    struct ovsdb *, const struct json *, bool converting, struct ovsdb_txn **);
static struct ovsdb_error *ovsdb_file_create(struct ovsdb *,
                                             struct ovsdb_log *,
                                             unsigned int n_transactions,
                                             off_t snapshot_size,
                                             struct ovsdb_file **filep);

static struct ovsdb_error *
ovsdb_file_open_log(const char *file_name, enum ovsdb_log_open_mode open_mode,
                    int locking, struct ovsdb_log **logp,
                    struct ovsdb_schema **schemap)
{
    struct ovsdb_schema *schema = NULL;
    struct ovsdb_log *log = NULL;
    struct ovsdb_error *error;
    struct json *json = NULL;

    ovs_assert(logp || schemap);

    error = ovsdb_log_open(file_name, OVSDB_MAGIC, open_mode, locking, &log);
    if (error) {
        goto error;
    }

    error = ovsdb_log_read(log, &json);
    if (error) {
        goto error;
    } else if (!json) {
        error = ovsdb_io_error(EOF, "%s: database file contains no schema",
                               file_name);
        goto error;
    }

    if (schemap) {
        error = ovsdb_schema_from_json(json, &schema);
        if (error) {
            error = ovsdb_wrap_error(error,
                                     "failed to parse \"%s\" as ovsdb schema",
                                     file_name);
            goto error;
        }
    }
    json_destroy(json);

    if (logp) {
        *logp = log;
    } else {
        ovsdb_log_close(log);
    }
    if (schemap) {
        *schemap = schema;
    }
    return NULL;

error:
    ovsdb_log_close(log);
    json_destroy(json);
    if (logp) {
        *logp = NULL;
    }
    if (schemap) {
        *schemap = NULL;
    }
    return error;
}

/* Opens database 'file_name' and stores a pointer to the new database in
 * '*dbp'.
 *
 * If 'schema' is nonnull, then it is used to interpret the data in
 * 'file_name', ignoring the schema actually stored in the file.  Data in the
 * file for tables or columns that do not exist in 'schema' are ignored, but
 * the ovsdb file format must otherwise be observed, including column
 * constraints.  Theis feature can be useful for upgrading or downgrading
 * databases to "almost-compatible" formats.
 *
 * If 'read_only' is true, the database will opened for reading only, otherwise
 * for reading and writing.
 *
 * Whether the file will be locked using lockfile_lock() depends on 'locking':
 * use true to lock it, false not to lock it, or -1 to lock it only if the file
 * is being opened for read/write access.
 *
 * If 'filep' is nonnull, then on success sets '*filep' to an ovsdb_file that
 * represents the open file.  This ovsdb_file persists until '*dbp' is
 * destroyed.
 *
 * On success, returns NULL.  On failure, returns an ovsdb_error (which the
 * caller must destroy) and sets '*dbp' and '*filep' to NULL. */
struct ovsdb_error * OVS_WARN_UNUSED_RESULT
ovsdb_file_open(const char *file_name,
                const struct ovsdb_schema *alternate_schema,
                bool read_only, int locking, struct ovsdb **dbp,
                struct ovsdb_file **filep)
{
    enum ovsdb_log_open_mode open_mode;
    struct ovsdb_schema *schema = NULL;
    struct ovsdb_error *error;
    struct ovsdb_log *log;
    struct json *json;
    struct ovsdb *db = NULL;

    open_mode = read_only ? OVSDB_LOG_READ_ONLY : OVSDB_LOG_READ_WRITE;
    error = ovsdb_file_open_log(file_name, open_mode, locking, &log,
                                alternate_schema ? NULL : &schema);
    if (error) {
        goto error;
    }

    db = ovsdb_create(schema ? schema : ovsdb_schema_clone(alternate_schema));

    /* When a log gets big, we compact it into a new log that initially has
     * only a single transaction that represents the entire state of the
     * database.  Thus, we consider the first transaction in the database to be
     * the snapshot.  We measure its size to later influence the minimum log
     * size before compacting again.
     *
     * The schema precedes the snapshot in the log; we could compensate for its
     * size, but it's just not that important. */
    off_t snapshot_size = 0;
    unsigned int n_transactions = 0;
    while ((error = ovsdb_log_read(log, &json)) == NULL && json) {
        struct ovsdb_txn *txn;

        error = ovsdb_file_txn_from_json(db, json, alternate_schema != NULL,
                                         &txn);
        json_destroy(json);
        if (error) {
            ovsdb_log_unread(log);
            break;
        }

        n_transactions++;
        error = ovsdb_txn_commit(txn, false);
        if (error) {
            ovsdb_log_unread(log);
            break;
        }

        if (n_transactions == 1) {
            snapshot_size = ovsdb_log_get_offset(log);
        }
    }
    if (error) {
        /* Log error but otherwise ignore it.  Probably the database just got
         * truncated due to power failure etc. and we should use its current
         * contents. */
        char *msg = ovsdb_error_to_string_free(error);
        VLOG_ERR("%s", msg);
        free(msg);
    }

    if (filep) {
        error = ovsdb_file_create(db, log, n_transactions,
                                  snapshot_size, filep);
        if (error) {
            goto error;
        }
        db->file = *filep;
    } else {
        ovsdb_log_close(log);
    }

    *dbp = db;
    return NULL;

error:
    *dbp = NULL;
    if (filep) {
        *filep = NULL;
    }
    ovsdb_destroy(db);
    ovsdb_log_close(log);
    return error;
}

static struct ovsdb_error *
ovsdb_file_update_row_from_json(struct ovsdb_row *row, bool converting,
                                const struct json *json)
{
    struct ovsdb_table_schema *schema = row->table->schema;
    struct ovsdb_error *error;
    struct shash_node *node;

    if (json->type != JSON_OBJECT) {
        return ovsdb_syntax_error(json, NULL, "row must be JSON object");
    }

    SHASH_FOR_EACH (node, json_object(json)) {
        const char *column_name = node->name;
        const struct ovsdb_column *column;
        struct ovsdb_datum datum;

        column = ovsdb_table_schema_get_column(schema, column_name);
        if (!column) {
            if (converting) {
                continue;
            }
            return ovsdb_syntax_error(json, "unknown column",
                                      "No column %s in table %s.",
                                      column_name, schema->name);
        }

        error = ovsdb_datum_from_json(&datum, &column->type, node->data, NULL);
        if (error) {
            return error;
        }
        ovsdb_datum_swap(&row->fields[column->index], &datum);
        ovsdb_datum_destroy(&datum, &column->type);
    }

    return NULL;
}

static struct ovsdb_error *
ovsdb_file_txn_row_from_json(struct ovsdb_txn *txn, struct ovsdb_table *table,
                             bool converting,
                             const struct uuid *row_uuid, struct json *json)
{
    const struct ovsdb_row *row = ovsdb_table_get_row(table, row_uuid);
    if (json->type == JSON_NULL) {
        if (!row) {
            return ovsdb_syntax_error(NULL, NULL, "transaction deletes "
                                      "row "UUID_FMT" that does not exist",
                                      UUID_ARGS(row_uuid));
        }
        ovsdb_txn_row_delete(txn, row);
        return NULL;
    } else if (row) {
        return ovsdb_file_update_row_from_json(ovsdb_txn_row_modify(txn, row),
                                               converting, json);
    } else {
        struct ovsdb_error *error;
        struct ovsdb_row *new;

        new = ovsdb_row_create(table);
        *ovsdb_row_get_uuid_rw(new) = *row_uuid;
        error = ovsdb_file_update_row_from_json(new, converting, json);
        if (error) {
            ovsdb_row_destroy(new);
        } else {
            ovsdb_txn_row_insert(txn, new);
        }
        return error;
    }
}

static struct ovsdb_error *
ovsdb_file_txn_table_from_json(struct ovsdb_txn *txn,
                               struct ovsdb_table *table,
                               bool converting, struct json *json)
{
    struct shash_node *node;

    if (json->type != JSON_OBJECT) {
        return ovsdb_syntax_error(json, NULL, "object expected");
    }

    SHASH_FOR_EACH (node, json->u.object) {
        const char *uuid_string = node->name;
        struct json *txn_row_json = node->data;
        struct ovsdb_error *error;
        struct uuid row_uuid;

        if (!uuid_from_string(&row_uuid, uuid_string)) {
            return ovsdb_syntax_error(json, NULL, "\"%s\" is not a valid UUID",
                                      uuid_string);
        }

        error = ovsdb_file_txn_row_from_json(txn, table, converting,
                                             &row_uuid, txn_row_json);
        if (error) {
            return error;
        }
    }

    return NULL;
}

/* Converts 'json' to an ovsdb_txn for 'db', storing the new transaction in
 * '*txnp'.  Returns NULL if successful, otherwise an error.
 *
 * If 'converting' is true, then unknown table and column names are ignored
 * (which can ease upgrading and downgrading schemas); otherwise, they are
 * treated as errors. */
static struct ovsdb_error *
ovsdb_file_txn_from_json(struct ovsdb *db, const struct json *json,
                         bool converting, struct ovsdb_txn **txnp)
{
    struct ovsdb_error *error;
    struct shash_node *node;
    struct ovsdb_txn *txn;

    *txnp = NULL;

    if (json->type != JSON_OBJECT) {
        return ovsdb_syntax_error(json, NULL, "object expected");
    }

    txn = ovsdb_txn_create(db);
    SHASH_FOR_EACH (node, json->u.object) {
        const char *table_name = node->name;
        struct json *node_json = node->data;
        struct ovsdb_table *table;

        table = shash_find_data(&db->tables, table_name);
        if (!table) {
            if (!strcmp(table_name, "_date")
                && node_json->type == JSON_INTEGER) {
                continue;
            } else if (!strcmp(table_name, "_comment") || converting) {
                continue;
            }

            error = ovsdb_syntax_error(json, "unknown table",
                                       "No table named %s.", table_name);
            goto error;
        }

        error = ovsdb_file_txn_table_from_json(txn, table, converting,
                                               node_json);
        if (error) {
            goto error;
        }
    }
    *txnp = txn;
    return NULL;

error:
    ovsdb_txn_abort(txn);
    return error;
}

static struct ovsdb_error *
ovsdb_file_save_copy__(struct ovsdb_log *log, const char *comment,
                       const struct ovsdb *db)
{
    const struct shash_node *node;
    struct ovsdb_file_txn ftxn;
    struct ovsdb_error *error;
    struct json *json;

    /* Write schema. */
    json = ovsdb_schema_to_json(db->schema);
    error = ovsdb_log_write(log, json);
    json_destroy(json);
    if (error) {
        return error;
    }

    /* Write data. */
    ovsdb_file_txn_init(&ftxn);
    SHASH_FOR_EACH (node, &db->tables) {
        const struct ovsdb_table *table = node->data;
        const struct ovsdb_row *row;

        HMAP_FOR_EACH (row, hmap_node, &table->rows) {
            ovsdb_file_txn_add_row(&ftxn, NULL, row, NULL);
        }
    }
    error = ovsdb_file_txn_commit(ftxn.json, comment, true, log);

    return error;
}

/* Saves a snapshot of 'db''s current contents as 'file_name'.  If 'comment' is
 * nonnull, then it is added along with the data contents and can be viewed
 * with "ovsdb-tool show-log". */
struct ovsdb_error *
ovsdb_file_save_copy(const char *file_name, const char *comment,
                     const struct ovsdb *db)
{
    struct ovsdb_log *log = NULL;
    struct ovsdb_error *error;

    error = ovsdb_log_open(file_name, OVSDB_MAGIC,
                           OVSDB_LOG_CREATE_EXCL, true, &log);
    if (!error) {
        error = ovsdb_file_save_copy__(log, comment, db);
        if (!error) {
            error = ovsdb_log_commit(log);
        }
        if (error) {
            unlink(file_name);
        }
        ovsdb_log_close(log);
    }
    return error;
}

/* Opens database 'file_name', reads its schema, and closes it.  On success,
 * stores the schema into '*schemap' and returns NULL; the caller then owns the
 * schema.  On failure, returns an ovsdb_error (which the caller must destroy)
 * and sets '*dbp' to NULL. */
struct ovsdb_error *
ovsdb_file_read_schema(const char *file_name, struct ovsdb_schema **schemap)
{
    ovs_assert(schemap != NULL);
    return ovsdb_file_open_log(file_name, OVSDB_LOG_READ_ONLY, false,
                               NULL, schemap);
}

struct ovsdb_file {
    struct ovsdb *db;
    struct ovsdb_log *log;
    long long int last_compact;
    long long int next_compact;
    unsigned int n_transactions;
    off_t snapshot_size;
};

static struct ovsdb_error *
ovsdb_file_create(struct ovsdb *db, struct ovsdb_log *log,
                  unsigned int n_transactions, off_t snapshot_size,
                  struct ovsdb_file **filep)
{
    struct ovsdb_file *file;

    file = xmalloc(sizeof *file);
    file->db = db;
    file->log = log;
    file->last_compact = time_wall_msec();
    file->next_compact = file->last_compact + COMPACT_MIN_MSEC;
    file->snapshot_size = snapshot_size;
    file->n_transactions = n_transactions;

    *filep = file;
    return NULL;
}

static bool
ovsdb_file_change_cb(const struct ovsdb_row *old,
                     const struct ovsdb_row *new,
                     const unsigned long int *changed,
                     void *ftxn_)
{
    struct ovsdb_file_txn *ftxn = ftxn_;
    ovsdb_file_txn_add_row(ftxn, old, new, changed);
    return true;
}

struct ovsdb_error *
ovsdb_file_commit(struct ovsdb_file *file,
                  const struct ovsdb_txn *txn, bool durable)
{
    struct ovsdb_file_txn ftxn;
    struct ovsdb_error *error;

    ovsdb_file_txn_init(&ftxn);
    ovsdb_txn_for_each_change(txn, ovsdb_file_change_cb, &ftxn);
    if (!ftxn.json) {
        /* Nothing to commit. */
        return NULL;
    }

    error = ovsdb_file_txn_commit(ftxn.json, ovsdb_txn_get_comment(txn),
                                  durable, file->log);
    if (error) {
        return error;
    }
    file->n_transactions++;

    /* If it has been at least COMPACT_MIN_MSEC ms since the last time we
     * compacted (or at least COMPACT_RETRY_MSEC ms since the last time we
     * tried), and if there are at least 100 transactions in the database, and
     * if the database is at least 10 MB, and the database is at least 4x the
     * size of the previous snapshot, then compact the database. */
    off_t log_size = ovsdb_log_get_offset(file->log);
    if (time_msec() >= file->next_compact
        && file->n_transactions >= 100
        && log_size >= 10 * 1024 * 1024
        && log_size / 4 >= file->snapshot_size)
    {
        error = ovsdb_file_compact(file);
        if (error) {
            char *s = ovsdb_error_to_string_free(error);
            VLOG_WARN("%s: compacting database failed (%s), retrying in "
                      "%d seconds", ovsdb_log_get_name(file->log),
                      s, COMPACT_RETRY_MSEC / 1000);
            free(s);

            file->next_compact = time_msec() + COMPACT_RETRY_MSEC;
        }
    }

    return NULL;
}

struct ovsdb_error *
ovsdb_file_compact(struct ovsdb_file *file)
{
    struct ovsdb_log *new_log = NULL;
    struct ovsdb_error *error;
    char *comment = NULL;

    comment = xasprintf("compacting database online "
                        "(%.3f seconds old, %u transactions, %llu bytes)",
                        (time_wall_msec() - file->last_compact) / 1000.0,
                        file->n_transactions,
                        (unsigned long long) ovsdb_log_get_offset(file->log));

    error = ovsdb_log_replace_start(file->log, &new_log);
    if (error) {
        goto exit;
    }

    error = ovsdb_file_save_copy__(new_log, comment, file->db);
    if (error) {
        goto exit;
    }

    error = ovsdb_log_replace_commit(file->log, new_log);

exit:
    if (!error) {
        file->last_compact = time_wall_msec();
        file->next_compact = file->last_compact + COMPACT_MIN_MSEC;
        file->n_transactions = 1;
    }

    free(comment);

    return error;
}

void
ovsdb_file_destroy(struct ovsdb_file *file)
{
    ovsdb_log_close(file->log);
    free(file);
}

static void
ovsdb_file_txn_init(struct ovsdb_file_txn *ftxn)
{
    ftxn->json = NULL;
    ftxn->table_json = NULL;
    ftxn->table = NULL;
}

static void
ovsdb_file_txn_add_row(struct ovsdb_file_txn *ftxn,
                       const struct ovsdb_row *old,
                       const struct ovsdb_row *new,
                       const unsigned long int *changed)
{
    struct json *row;

    if (!new) {
        row = json_null_create();
    } else {
        struct shash_node *node;

        row = old ? NULL : json_object_create();
        SHASH_FOR_EACH (node, &new->table->schema->columns) {
            const struct ovsdb_column *column = node->data;
            const struct ovsdb_type *type = &column->type;
            unsigned int idx = column->index;

            if (idx != OVSDB_COL_UUID && column->persistent
                && (old
                    ? bitmap_is_set(changed, idx)
                    : !ovsdb_datum_is_default(&new->fields[idx], type)))
            {
                if (!row) {
                    row = json_object_create();
                }
                json_object_put(row, column->name,
                                ovsdb_datum_to_json(&new->fields[idx], type));
            }
        }
    }

    if (row) {
        struct ovsdb_table *table = new ? new->table : old->table;
        char uuid[UUID_LEN + 1];

        if (table != ftxn->table) {
            /* Create JSON object for transaction overall. */
            if (!ftxn->json) {
                ftxn->json = json_object_create();
            }

            /* Create JSON object for transaction on this table. */
            ftxn->table_json = json_object_create();
            ftxn->table = table;
            json_object_put(ftxn->json, table->schema->name, ftxn->table_json);
        }

        /* Add row to transaction for this table. */
        snprintf(uuid, sizeof uuid,
                 UUID_FMT, UUID_ARGS(ovsdb_row_get_uuid(new ? new : old)));
        json_object_put(ftxn->table_json, uuid, row);
    }
}

static struct ovsdb_error *
ovsdb_file_txn_commit(struct json *json, const char *comment,
                      bool durable, struct ovsdb_log *log)
{
    struct ovsdb_error *error;

    if (!json) {
        json = json_object_create();
    }
    if (comment) {
        json_object_put_string(json, "_comment", comment);
    }
    json_object_put(json, "_date", json_integer_create(time_wall_msec()));

    error = ovsdb_log_write(log, json);
    json_destroy(json);
    if (error) {
        return ovsdb_wrap_error(error, "writing transaction failed");
    }

    if (durable) {
        error = ovsdb_log_commit(log);
        if (error) {
            return ovsdb_wrap_error(error, "committing transaction failed");
        }
    }

    return NULL;
}
