/*
 * Copyright (c) 2009, 2010, 2011, 2012, 2013, 2016, 2017 Nicira, Inc.
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
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "column.h"
#include "command-line.h"
#include "compiler.h"
#include "dirs.h"
#include "openvswitch/dynamic-string.h"
#include "fatal-signal.h"
#include "file.h"
#include "lockfile.h"
#include "log.h"
#include "openvswitch/json.h"
#include "ovsdb.h"
#include "ovsdb-data.h"
#include "ovsdb-error.h"
#include "ovsdb-parser.h"
#include "raft.h"
#include "socket-util.h"
#include "table.h"
#include "timeval.h"
#include "util.h"
#include "openvswitch/vlog.h"

/* -m, --more: Verbosity level for "show-log" command output. */
static int show_log_verbosity;

/* --cid: Cluster ID for "join-cluster" command. */
static struct uuid cid;

static const struct ovs_cmdl_command *get_all_commands(void);

OVS_NO_RETURN static void usage(void);
static void parse_options(int argc, char *argv[]);

static const char *default_db(void);
static const char *default_schema(void);

int
main(int argc, char *argv[])
{
    struct ovs_cmdl_context ctx = { .argc = 0, };
    set_program_name(argv[0]);
    parse_options(argc, argv);
    fatal_ignore_sigpipe();
    fatal_signal_init();
    ctx.argc = argc - optind;
    ctx.argv = argv + optind;
    ovs_cmdl_run_command(&ctx, get_all_commands());
    return 0;
}

static void
parse_options(int argc, char *argv[])
{
    enum {
        OPT_CID = UCHAR_MAX + 1,
    };
    static const struct option long_options[] = {
        {"more", no_argument, NULL, 'm'},
        {"cid", required_argument, NULL, OPT_CID},
        {"verbose", optional_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {"option", no_argument, NULL, 'o'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };
    char *short_options = ovs_cmdl_long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'm':
            show_log_verbosity++;
            break;

        case OPT_CID:
            if (!uuid_from_string(&cid, optarg) || uuid_is_zero(&cid)) {
                ovs_fatal(0, "%s: not a valid UUID", optarg);
            }
            break;

        case 'h':
            usage();

        case 'o':
            ovs_cmdl_print_options(long_options);
            exit(EXIT_SUCCESS);

        case 'V':
            ovs_print_version(0, 0);
            exit(EXIT_SUCCESS);

        case 'v':
            vlog_set_verbosity(optarg);
            break;

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);
}

static void
usage(void)
{
    printf("%s: Open vSwitch database management utility\n"
           "usage: %s [OPTIONS] COMMAND [ARG...]\n"
           "  create [DB [SCHEMA]]    create DB with the given SCHEMA\n"
           "  create-cluster DB CONTENTS LOCAL\n"
           "    create clustered DB with given CONTENTS and LOCAL address\n"
           "  [--cid=UUID] join-cluster DB NAME LOCAL REMOTE...\n"
           "    join clustered DB with given NAME and LOCAL and REMOTE addrs\n"
           "  compact [DB [DST]]      compact DB in-place (or to DST)\n"
           "  convert [DB [SCHEMA [DST]]]   convert DB to SCHEMA (to DST)\n"
           "  db-name [DB]            report name of schema used by DB\n"
           "  db-version [DB]         report version of schema used by DB\n"
           "  db-cksum [DB]           report checksum of schema used by DB\n"
           "  schema-name [SCHEMA]    report SCHEMA's name\n"
           "  schema-version [SCHEMA] report SCHEMA's schema version\n"
           "  schema-cksum [SCHEMA]   report SCHEMA's checksum\n"
           "  query [DB] TRNS         execute read-only transaction on DB\n"
           "  transact [DB] TRNS      execute read/write transaction on DB\n"
           "  [-m]... show-log [DB]   print DB's log entries\n"
           "The default DB is %s.\n"
           "The default SCHEMA is %s.\n",
           program_name, program_name, default_db(), default_schema());
    vlog_usage();
    printf("\nOther options:\n"
           "  -m, --more                  increase show-log verbosity\n"
           "  -h, --help                  display this help message\n"
           "  -V, --version               display version information\n");
    exit(EXIT_SUCCESS);
}

static const char *
default_db(void)
{
    static char *db;
    if (!db) {
        db = xasprintf("%s/conf.db", ovs_dbdir());
    }
    return db;
}

static const char *
default_schema(void)
{
    static char *schema;
    if (!schema) {
        schema = xasprintf("%s/vswitch.ovsschema", ovs_pkgdatadir());
    }
    return schema;
}

static struct json *
parse_json(const char *s)
{
    struct json *json = json_from_string(s);
    if (json->type == JSON_STRING) {
        ovs_fatal(0, "\"%s\": %s", s, json->u.string);
    }
    return json;
}

static void
print_and_free_json(struct json *json)
{
    char *string = json_to_string(json, JSSF_SORT);
    json_destroy(json);
    puts(string);
    free(string);
}

static void
check_ovsdb_error(struct ovsdb_error *error)
{
    if (error) {
        ovs_fatal(0, "%s", ovsdb_error_to_string(error));
    }
}

static void
do_create(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argc >= 2 ? ctx->argv[1] : default_db();
    const char *schema_file_name = ctx->argc >= 3 ? ctx->argv[2] : default_schema();
    struct ovsdb_schema *schema;
    struct ovsdb_log *log;
    struct json *json;

    /* Read schema from file and convert to JSON. */
    check_ovsdb_error(ovsdb_schema_from_file(schema_file_name, &schema));
    json = ovsdb_schema_to_json(schema);
    ovsdb_schema_destroy(schema);

    /* Create database file. */
    check_ovsdb_error(ovsdb_log_open(db_file_name, OVSDB_MAGIC,
                                     OVSDB_LOG_CREATE_EXCL, -1, &log));
    check_ovsdb_error(ovsdb_log_write(log, json));
    check_ovsdb_error(ovsdb_log_commit(log));
    ovsdb_log_close(log);

    json_destroy(json);
}

static void
do_create_cluster(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argv[1];
    const char *schema_file_name = ctx->argv[2];
    const char *local = ctx->argv[3];

    /* Read schema from file and convert to JSON. */
    /* XXX add support for creating from a standalone database
     * rather than a schema */
    struct ovsdb_schema *schema;
    check_ovsdb_error(ovsdb_schema_from_file(schema_file_name, &schema));
    char *name = xstrdup(schema->name);
    struct json *schema_json = ovsdb_schema_to_json(schema);
    ovsdb_schema_destroy(schema);

    /* Generate snapshot and convert to string. */
    struct json *data = json_object_create();
    struct json *snapshot = json_array_create_2(schema_json, data);

    /* Create database file. */
    check_ovsdb_error(raft_create_cluster(db_file_name, name,
                                          local, snapshot));
    free(name);
    json_destroy(snapshot);
}

static void
do_join_cluster(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argv[1];
    const char *name = ctx->argv[2];
    const char *local = ctx->argv[3];

    /* Check for a plausible 'name'. */
    if (!ovsdb_parser_is_id(name)) {
        ovs_fatal(0, "%s: not a valid schema name (use \"schema-name\" "
                  "command to find the correct name)", name);
    }

    /* Create database file. */
    check_ovsdb_error(raft_join_cluster(db_file_name, name, local,
                                        &ctx->argv[4], ctx->argc - 4,
                                        uuid_is_zero(&cid) ? NULL : &cid));
}

static void
compact_or_convert(const char *src_name_, const char *dst_name_,
                   const struct ovsdb_schema *new_schema,
                   const char *comment)
{

    struct ovsdb_file *file;
    struct ovsdb *db;

    /* We dereference symlinks for source and destination names.  In the
     * in-place case this ensures that, if the source name is a symlink, we
     * replace its target instead of replacing the symlink by a regular file.
     * In the non-in-place, this has the same effect for the destination
     * name. */
    char *src_name = follow_symlinks(src_name_);
    check_ovsdb_error(ovsdb_file_open(src_name, new_schema, true,
                                      dst_name_ == NULL, &db, &file));

    if (!dst_name_) {
        check_ovsdb_error(ovsdb_file_compact(file));
    } else {
        char *dst_name = follow_symlinks(dst_name_);
        check_ovsdb_error(ovsdb_file_save_copy(dst_name, comment, db));
        free(dst_name);
    }

    ovsdb_destroy(db);
    free(src_name);
}

static void
do_compact(struct ovs_cmdl_context *ctx)
{
    const char *db = ctx->argc >= 2 ? ctx->argv[1] : default_db();
    const char *target = ctx->argc >= 3 ? ctx->argv[2] : NULL;

    compact_or_convert(db, target, NULL, "compacted by ovsdb-tool "VERSION);
}

static void
do_convert(struct ovs_cmdl_context *ctx)
{
    const char *db = ctx->argc >= 2 ? ctx->argv[1] : default_db();
    const char *schema = ctx->argc >= 3 ? ctx->argv[2] : default_schema();
    const char *target = ctx->argc >= 4 ? ctx->argv[3] : NULL;
    struct ovsdb_schema *new_schema;

    check_ovsdb_error(ovsdb_schema_from_file(schema, &new_schema));
    compact_or_convert(db, target, new_schema,
                       "converted by ovsdb-tool "VERSION);
    ovsdb_schema_destroy(new_schema);
}

static void
do_needs_conversion(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argc >= 2 ? ctx->argv[1] : default_db();
    const char *schema_file_name = ctx->argc >= 3 ? ctx->argv[2] : default_schema();
    struct ovsdb_schema *schema1, *schema2;

    check_ovsdb_error(ovsdb_file_read_schema(db_file_name, &schema1));
    check_ovsdb_error(ovsdb_schema_from_file(schema_file_name, &schema2));
    puts(ovsdb_schema_equal(schema1, schema2) ? "no" : "yes");
    ovsdb_schema_destroy(schema1);
    ovsdb_schema_destroy(schema2);
}

static void
do_db_name(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argc >= 2 ? ctx->argv[1] : default_db();
    struct ovsdb_schema *schema;

    check_ovsdb_error(ovsdb_file_read_schema(db_file_name, &schema));
    puts(schema->name);
    ovsdb_schema_destroy(schema);
}

static void
do_db_version(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argc >= 2 ? ctx->argv[1] : default_db();
    struct ovsdb_schema *schema;

    check_ovsdb_error(ovsdb_file_read_schema(db_file_name, &schema));
    puts(schema->version);
    ovsdb_schema_destroy(schema);
}

static void
do_db_cksum(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argc >= 2 ? ctx->argv[1] : default_db();
    struct ovsdb_schema *schema;

    check_ovsdb_error(ovsdb_file_read_schema(db_file_name, &schema));
    puts(schema->cksum);
    ovsdb_schema_destroy(schema);
}

static void
do_db_cid(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argv[1];
    struct raft_metadata md;

    check_ovsdb_error(raft_read_metadata(db_file_name, &md));
    if (uuid_is_zero(&md.cid)) {
        fprintf(stderr, "%s: cluster ID not yet known\n", db_file_name);
        exit(2);
    }
    printf(UUID_FMT"\n", UUID_ARGS(&md.cid));
    raft_metadata_destroy(&md);
}

static void
do_db_sid(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argv[1];
    struct raft_metadata md;

    check_ovsdb_error(raft_read_metadata(db_file_name, &md));
    printf(UUID_FMT"\n", UUID_ARGS(&md.sid));
    raft_metadata_destroy(&md);
}

static void
do_db_local_address(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argv[1];
    struct raft_metadata md;

    check_ovsdb_error(raft_read_metadata(db_file_name, &md));
    puts(md.local);
    raft_metadata_destroy(&md);
}

static void
do_schema_name(struct ovs_cmdl_context *ctx)
{
    const char *schema_file_name = ctx->argc >= 2 ? ctx->argv[1] : default_schema();
    struct ovsdb_schema *schema;

    check_ovsdb_error(ovsdb_schema_from_file(schema_file_name, &schema));
    puts(schema->name);
    ovsdb_schema_destroy(schema);
}

static void
do_schema_version(struct ovs_cmdl_context *ctx)
{
    const char *schema_file_name = ctx->argc >= 2 ? ctx->argv[1] : default_schema();
    struct ovsdb_schema *schema;

    check_ovsdb_error(ovsdb_schema_from_file(schema_file_name, &schema));
    puts(schema->version);
    ovsdb_schema_destroy(schema);
}

static void
do_schema_cksum(struct ovs_cmdl_context *ctx)
{
    const char *schema_file_name = ctx->argc >= 2 ? ctx->argv[1] : default_schema();
    struct ovsdb_schema *schema;

    check_ovsdb_error(ovsdb_schema_from_file(schema_file_name, &schema));
    puts(schema->cksum);
    ovsdb_schema_destroy(schema);
}

static void
transact(bool read_only, int argc, char *argv[])
{
    const char *db_file_name = argc >= 3 ? argv[1] : default_db();
    const char *transaction = argv[argc - 1];
    struct json *request, *result;
    struct ovsdb *db;

    struct ovsdb_file *file;
    check_ovsdb_error(ovsdb_file_open(db_file_name, NULL, read_only,
                                      !read_only, &db, &file));

    request = parse_json(transaction);
    result = ovsdb_execute(db, NULL, request, false, 0, NULL);
    json_destroy(request);

    print_and_free_json(result);
    ovsdb_destroy(db);
}

static void
do_query(struct ovs_cmdl_context *ctx)
{
    transact(true, ctx->argc, ctx->argv);
}

static void
do_transact(struct ovs_cmdl_context *ctx)
{
    transact(false, ctx->argc, ctx->argv);
}

static void
print_db_changes(struct shash *tables, struct shash *names,
                 const struct ovsdb_schema *schema)
{
    struct shash_node *n1;

    SHASH_FOR_EACH (n1, tables) {
        const char *table = n1->name;
        struct ovsdb_table_schema *table_schema;
        struct json *rows = n1->data;
        struct shash_node *n2;

        if (n1->name[0] == '_' || rows->type != JSON_OBJECT) {
            continue;
        }

        table_schema = shash_find_data(&schema->tables, table);
        SHASH_FOR_EACH (n2, json_object(rows)) {
            const char *row_uuid = n2->name;
            struct json *columns = n2->data;
            struct shash_node *n3;
            char *old_name, *new_name;
            bool free_new_name = false;

            old_name = new_name = shash_find_data(names, row_uuid);
            if (columns->type == JSON_OBJECT) {
                struct json *new_name_json;

                new_name_json = shash_find_data(json_object(columns), "name");
                if (new_name_json) {
                    new_name = json_to_string(new_name_json, JSSF_SORT);
                    free_new_name = true;
                }
            }

            printf("\ttable %s", table);

            if (!old_name) {
                if (new_name) {
                    printf(" insert row %s (%.8s):\n", new_name, row_uuid);
                } else {
                    printf(" insert row %.8s:\n", row_uuid);
                }
            } else {
                printf(" row %s (%.8s):\n", old_name, row_uuid);
            }

            if (columns->type == JSON_OBJECT) {
                if (show_log_verbosity > 1) {
                    SHASH_FOR_EACH (n3, json_object(columns)) {
                        const char *column = n3->name;
                        const struct ovsdb_column *column_schema;
                        struct json *value = n3->data;
                        char *value_string = NULL;

                        column_schema =
                            (table_schema
                             ? shash_find_data(&table_schema->columns, column)
                             : NULL);
                        if (column_schema) {
                            const struct ovsdb_type *type;
                            struct ovsdb_error *error;
                            struct ovsdb_datum datum;

                            type = &column_schema->type;
                            error = ovsdb_datum_from_json(&datum, type,
                                                          value, NULL);
                            if (!error) {
                                struct ds s;

                                ds_init(&s);
                                ovsdb_datum_to_string(&datum, type, &s);
                                value_string = ds_steal_cstr(&s);
                            } else {
                                ovsdb_error_destroy(error);
                            }
                        }
                        if (!value_string) {
                            value_string = json_to_string(value, JSSF_SORT);
                        }
                        printf("\t\t%s=%s\n", column, value_string);
                        free(value_string);
                    }
                }
                if (!old_name
                    || (new_name != old_name && strcmp(old_name, new_name))) {
                    if (old_name) {
                        shash_delete(names, shash_find(names, row_uuid));
                        free(old_name);
                    }
                    shash_add(names, row_uuid, (new_name
                                                ? xstrdup(new_name)
                                                : xmemdup0(row_uuid, 8)));
                }
            } else if (columns->type == JSON_NULL) {
                struct shash_node *node;

                printf("\t\tdelete row\n");
                node = shash_find(names, row_uuid);
                if (node) {
                    shash_delete(names, node);
                }
                free(old_name);
            }

            if (free_new_name) {
                free(new_name);
            }
        }
    }
}

static void
do_show_log_standalone(struct ovsdb_log *log)
{
    struct shash names;
    struct ovsdb_schema *schema;
    unsigned int i;

    shash_init(&names);
    schema = NULL;
    for (i = 0; ; i++) {
        struct json *json;

        check_ovsdb_error(ovsdb_log_read(log, &json));
        if (!json) {
            break;
        }

        printf("record %u:", i);
        if (i == 0) {
            check_ovsdb_error(ovsdb_schema_from_json(json, &schema));
            printf(" \"%s\" schema, version=\"%s\", cksum=\"%s\"\n",
                   schema->name, schema->version, schema->cksum);
        } else if (json->type == JSON_OBJECT) {
            struct json *date, *comment;

            date = shash_find_data(json_object(json), "_date");
            if (date && date->type == JSON_INTEGER) {
                long long int t = json_integer(date);
                char *s;

                if (t < INT32_MAX) {
                    /* Older versions of ovsdb wrote timestamps in seconds. */
                    t *= 1000;
                }

                s = xastrftime_msec(" %Y-%m-%d %H:%M:%S.###", t, true);
                fputs(s, stdout);
                free(s);
            }

            comment = shash_find_data(json_object(json), "_comment");
            if (comment && comment->type == JSON_STRING) {
                printf(" \"%s\"", json_string(comment));
            }

            if (i > 0 && show_log_verbosity > 0) {
                putchar('\n');
                print_db_changes(json_object(json), &names, schema);
            }
        }
        json_destroy(json);
        putchar('\n');
    }

    ovsdb_schema_destroy(schema);
    /* XXX free 'names'. */
}

static void
print_member(const struct shash *object, const char *name)
{
    const struct json *value = shash_find_data(object, name);
    if (!value) {
        return;
    }

    char *s = json_to_string(value, JSSF_SORT);
    printf("\t%s: %s\n", name, s);
    free(s);
}

static void
print_uuid(const struct shash *object, const char *name)
{
    const struct json *value = shash_find_data(object, name);
    if (!value) {
        return;
    }

    printf("\t%s: ", name);
    if (value->type == JSON_STRING) {
        printf("%.4s\n", value->u.string);
    } else {
        printf("***invalid*\n");
    }
}

static void
print_servers(const struct shash *object, const char *name)
{
    const struct json *value = shash_find_data(object, name);
    if (!value) {
        return;
    }

    printf("\t%s: ", name);
    if (value->type != JSON_OBJECT) {
        printf("***invalid %s***\n", name);
    }

    const struct shash_node *node;
    int i = 0;
    SHASH_FOR_EACH (node, json_object(value)) {
        if (i++ > 0) {
            printf(", ");
        }
        printf("%.4s(", node->name);

        const struct json *address = node->data;
        if (address->type != JSON_STRING) {
            printf("***invalid***");
        } else {
            fputs(address->u.string, stdout);
        }

        printf(")");
    }
    printf("\n");
}

static void
print_data(const struct shash *object, const char *name)
{
    const struct json *data = shash_find_data(object, name);
    if (!data) {
        return;
    }

    if (data->type != JSON_ARRAY || json_array(data)->n != 2) {
        printf("\t***invalid data***\n");
        return;
    }

    const struct json *schema_json = json_array(data)->elems[0];
    if (schema_json->type != JSON_NULL) {
        struct ovsdb_schema *schema;

        check_ovsdb_error(ovsdb_schema_from_json(schema_json, &schema));
        printf("\tschema: \"%s\", version=\"%s\", cksum=\"%s\"\n",
               schema->name, schema->version, schema->cksum);
        ovsdb_schema_destroy(schema);
    }

    char *s = json_to_string(json_array(data)->elems[1], JSSF_SORT);
    printf("\t%s: %s\n", name, s);
    free(s);
}

static void
do_show_log_cluster(struct ovsdb_log *log)
{
    struct shash names;
    struct ovsdb_schema *schema;
    unsigned int i;

    shash_init(&names);
    schema = NULL;
    for (i = 0; ; i++) {
        struct json *json;
        check_ovsdb_error(ovsdb_log_read(log, &json));
        if (!json) {
            break;
        }

        struct shash *object = json_object(json);

        printf("record %u:\n", i);
        if (i == 0) {
            print_member(object, "name");
            print_member(object, "address");
            print_uuid(object, "server_id");
            print_uuid(object, "cluster_id");

            print_servers(object, "prev_servers");
            print_member(object, "prev_term");
            print_member(object, "prev_index");
            print_data(object, "prev_data");

            print_member(object, "remotes");
        } else {
            print_member(object, "term");
            print_member(object, "index");
            print_data(object, "data");
            print_servers(object, "servers");
            print_uuid(object, "vote");
        }
        json_destroy(json);
        putchar('\n');
    }

    ovsdb_schema_destroy(schema);
    /* XXX free 'names'. */
}

static void
do_show_log(struct ovs_cmdl_context *ctx)
{
    const char *db_file_name = ctx->argc >= 2 ? ctx->argv[1] : default_db();
    struct ovsdb_log *log;

    check_ovsdb_error(ovsdb_log_open(db_file_name, OVSDB_MAGIC"|"RAFT_MAGIC,
                                     OVSDB_LOG_READ_ONLY, -1, &log));
    if (!strcmp(ovsdb_log_get_magic(log), OVSDB_MAGIC)) {
        do_show_log_standalone(log);
    } else {
        do_show_log_cluster(log);
    }
    ovsdb_log_close(log);
}

static void
do_help(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    usage();
}

static void
do_list_commands(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
     ovs_cmdl_print_commands(get_all_commands());
}

static const struct ovs_cmdl_command all_commands[] = {
    { "create", "[db [schema]]", 0, 2, do_create, OVS_RW },
    { "create-cluster", "db contents local", 3, 3, do_create_cluster, OVS_RW },
    { "join-cluster", "db name local remote...", 4, INT_MAX, do_join_cluster,
      OVS_RW },
    { "compact", "[db [dst]]", 0, 2, do_compact, OVS_RW },
    { "convert", "[db [schema [dst]]]", 0, 3, do_convert, OVS_RW },
    { "needs-conversion", NULL, 0, 2, do_needs_conversion, OVS_RO },
    { "db-name", "[db]",  0, 1, do_db_name, OVS_RO },
    { "db-version", "[db]",  0, 1, do_db_version, OVS_RO },
    { "db-cksum", "[db]", 0, 1, do_db_cksum, OVS_RO },
    { "db-cid", "db", 1, 1, do_db_cid, OVS_RO },
    { "db-sid", "db", 1, 1, do_db_sid, OVS_RO },
    { "db-local-address", "db", 1, 1, do_db_local_address, OVS_RO },
    { "schema-name", "[schema]", 0, 1, do_schema_name, OVS_RO },
    { "schema-version", "[schema]", 0, 1, do_schema_version, OVS_RO },
    { "schema-cksum", "[schema]", 0, 1, do_schema_cksum, OVS_RO },
    { "query", "[db] trns", 1, 2, do_query, OVS_RO },
    { "transact", "[db] trns", 1, 2, do_transact, OVS_RO },
    { "show-log", "[db]", 0, 1, do_show_log, OVS_RO },
    { "help", NULL, 0, INT_MAX, do_help, OVS_RO },
    { "list-commands", NULL, 0, INT_MAX, do_list_commands, OVS_RO },
    { NULL, NULL, 0, 0, NULL, OVS_RO },
};

static const struct ovs_cmdl_command *get_all_commands(void)
{
    return all_commands;
}
