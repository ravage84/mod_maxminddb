/* maxminddb module
 *
 * This module populates environment variable from a MaxMind DB database
 * using the requestor's IP address.
 *
 * Copyright 2014, MaxMind Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_log.h"
#include "ap_config.h"
#include "apr_strings.h"
#include "util_script.h"
#include <netdb.h>
#include <arpa/inet.h>
#include "maxminddb.h"
#include <string.h>
#include <alloca.h>
#include <inttypes.h>

#if defined (MAXMINDDB_DEBUG)
#define INFO(server_rec, ...)                                            \
    ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, server_rec, \
                 "[mod_maxminddb]: " __VA_ARGS__)
#else
#define INFO(server_rec, ...)
#endif

#define ERROR(server_rec, ...)                         \
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, server_rec, \
                 "[mod_maxminddb]: " __VA_ARGS__)

#ifdef UNUSED
#elif defined(__GNUC__)
#define  UNUSED(x) UNUSED_ ## x __attribute__((unused))
#else
#define UNUSED
#endif

typedef struct key_value_list_s {
    const char *path;
    const char *env_key;
    struct key_value_list_s *next;
    const char **names;
} key_value_list_s;

typedef struct maxminddb_database_list {
    struct maxminddb_database_list *nextdb;
    const char *disk_name;
    const char *nick_name;
    MMDB_s *mmdb;
    key_value_list_s *next;
} maxminddb_database_list;

typedef struct maxminddb_config {
    maxminddb_database_list *nextdb;
    int enabled;
} maxminddb_config;

module AP_MODULE_DECLARE_DATA maxminddb_module;

static void add_database(cmd_parms *cmd, maxminddb_config *conf,
                         const char *nickname, const char *filename);

static apr_status_t cleanup_database(void *mmdb);

static char * from_uint128(apr_pool_t *pool,
                           const MMDB_entry_data_s *result);

static int maxminddb_header_parser(request_rec *r,
                                   maxminddb_config *);

static void set_env_for_ip(request_rec *r, maxminddb_config *mmsrvcfg,
                           const char *ipaddr);

static void set_user_env(request_rec *r, maxminddb_config *mmsrvcfg,
                         const char *ipaddr);

/* create a disabled directory entry */
static void *create_dir_config(apr_pool_t *pool, char *UNUSED(context))
{

    maxminddb_config *conf = apr_pcalloc(pool, sizeof(maxminddb_config));

    conf->nextdb = NULL;
    conf->enabled = 0;

    return conf;
}

static void *merge_dir_config(apr_pool_t *UNUSED(pool),
                              void *UNUSED(parent), void *cur)
{
    return cur;
}

static int maxminddb_per_dir(request_rec *r)
{
    INFO(r->server, "maxminddb_per_dir ( enabled )");
    maxminddb_config *cfg =
        ap_get_module_config(r->per_dir_config, &maxminddb_module);
    if (!cfg || !cfg->enabled) {
        return DECLINED;
    }

    return maxminddb_header_parser(r, cfg);
}

static char *get_client_ip(request_rec *r)
{
# if AP_SERVER_MAJORVERSION_NUMBER == 2 && AP_SERVER_MINORVERSION_NUMBER == 4
    return r->useragent_ip;
# else
    return r->connection->remote_ip;
#endif
}

static int maxminddb_header_parser(request_rec *r,
                                   maxminddb_config *mmsrvcfg)
{
    char *ipaddr;

    ipaddr = get_client_ip(r);
    INFO(r->server, "maxminddb_header_parser %s", ipaddr);

    if (!mmsrvcfg || !mmsrvcfg->enabled) {
        return DECLINED;
    }

    set_env_for_ip(r, mmsrvcfg, ipaddr);
    return OK;
}

static void set_env_for_ip(request_rec *r, maxminddb_config *mmsrvcfg,
                           const char *ipaddr)
{
    apr_table_set(r->subprocess_env, "MMDB_ADDR", ipaddr);
    set_user_env(r, mmsrvcfg, ipaddr);
}

static const char *set_maxminddb_enable(cmd_parms *cmd, void *config, int arg)
{
    maxminddb_config *conf = (maxminddb_config *)config;

    if (!conf) {
        return "mod_maxminddb: server structure not allocated";
    }

    conf->enabled = arg;
    INFO(cmd->server, "set_maxminddb_enable: (server) %d", arg);

    return NULL;
}

static const char *set_maxminddb_filename(cmd_parms *cmd, void *config,
                                          const char *nickname,
                                          const char *filename)
{

    maxminddb_config *cfg = (maxminddb_config *)config;

    INFO(cmd->server, "add-database (server) %s", filename);
    add_database(cmd, cfg, nickname, filename);

    INFO(cmd->server, "set_maxminddb_filename (server) %s", filename);

    return NULL;
}

static void add_database(cmd_parms *cmd, maxminddb_config *conf,
                         const char *nickname, const char *filename)
{
    for (maxminddb_database_list *cur = conf->nextdb;
         cur; cur = cur->nextdb) {
        if (!strcmp(cur->nick_name, nickname)) {
            // we know the nickname already
            INFO(cmd->server, "We know already db (%s) skipping %s", nickname,
                 filename);
            return;
        }
    }
    // insert
    maxminddb_database_list *sl =
        apr_palloc(cmd->pool, sizeof(maxminddb_database_list));
    sl->nextdb = NULL;
    sl->next = NULL;
    sl->mmdb = apr_palloc(cmd->pool, sizeof(MMDB_s));
    int mmdb_error = MMDB_open(filename, MMDB_MODE_MMAP, sl->mmdb);
    if (mmdb_error != MMDB_SUCCESS) {
        ERROR(cmd->server, "Opening %s failed: %s", filename,
              MMDB_strerror(mmdb_error));
        return;
    }

    apr_pool_pre_cleanup_register(cmd->pool, sl->mmdb, cleanup_database);

    sl->disk_name = (char *)apr_pstrdup(cmd->pool, filename);
    sl->nick_name = (char *)apr_pstrdup(cmd->pool, nickname);
    sl->nextdb = conf->nextdb;
    conf->nextdb = sl;
    INFO(cmd->server, "Insert db (%s)%s", nickname, filename);
}

static apr_status_t cleanup_database(void *mmdb)
{
    MMDB_close((MMDB_s *)mmdb);

    return APR_SUCCESS;
}


static void insert_kvlist(struct server_rec *srv,
                          maxminddb_config *mmsrvcfg,
                          key_value_list_s *list)
{
    const int max_names = 80;
    char *names[max_names + 1];

    apr_pool_t *pool = srv->process->pconf;

    names[0] = apr_pstrdup(pool, list->path);

    int i;
    char * strtok_last = NULL;

    char *token = apr_strtok(names[0], "/", &strtok_last);
    for (i = 1; i <= max_names && token; i++) {
        token = apr_strtok(NULL, "/", &strtok_last);
        names[i] = token;
    }

    if (!i) {
        return;
    }

    for (maxminddb_database_list *sl = mmsrvcfg->nextdb; sl; sl =
             sl->nextdb) {
        if (!strcmp(names[0], sl->nick_name)) {
            // found
            list->next = sl->next;
            sl->next = list;
            list->names = (const char **)apr_pmemdup(pool, names,
                                                     (1 + i) *
                                                     sizeof(char *));
            break;
        }
    }
}

static const char *set_maxminddb_env(cmd_parms *cmd, void *config,
                                     const char *env, const char *dbpath)
{
    key_value_list_s *list = apr_palloc(cmd->pool, sizeof(key_value_list_s));
    list->path = dbpath;
    list->env_key = env;
    list->next = NULL;
    list->names = NULL;

    maxminddb_config *conf = (maxminddb_config *)
                             config;

    INFO(cmd->server, "set_maxminddb_env (server) %s %s", env, dbpath);

    insert_kvlist(cmd->server, conf, list);

    return NULL;
}

static const command_rec maxminddb_directives[] = {
    AP_INIT_FLAG("MaxMindDBEnable",
                 set_maxminddb_enable,
                 NULL,
                 OR_FILEINFO,
                 "Turn on mod_maxminddb"),
    AP_INIT_TAKE2("MaxMindDBFile",
                  set_maxminddb_filename,
                  NULL,
                  OR_ALL,
                  "Path to the Database File"),
    AP_INIT_ITERATE2("MaxMindDBEnv",
                     set_maxminddb_env,
                     NULL,
                     OR_ALL,
                     "Set desired env var"),
    { NULL }
};

static void maxminddb_register_hooks(apr_pool_t *UNUSED(p))
{
    /* make sure we run before mod_rewrite's handler */
    static const char *const asz_succ[] =
    { "mod_setenvif.c", "mod_rewrite.c", NULL };

    ap_hook_header_parser(maxminddb_per_dir, NULL, asz_succ, APR_HOOK_MIDDLE);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA maxminddb_module = {
    STANDARD20_MODULE_STUFF,
    create_dir_config,       /* create per-dir    config structures */
    merge_dir_config,        /* merge  per-dir    config structures */
    NULL,                    /* create per-server config structures */
    NULL,                    /* merge  per-server config structures */
    maxminddb_directives,    /* table of config file commands       */
    maxminddb_register_hooks /* register hooks                      */
};

static void set_user_env(request_rec *r, maxminddb_config *mmsrvcfg,
                         const char *ipaddr)
{
    if (ipaddr == NULL) {
        return;
    }

    for (maxminddb_database_list *sl = mmsrvcfg->nextdb; sl; sl =
             sl->nextdb) {

        if (sl->next == NULL) {
            continue;
        }

        int gai_error, mmdb_error;
        MMDB_lookup_result_s lookup_result =
            MMDB_lookup_string(sl->mmdb, ipaddr, &gai_error, &mmdb_error);

        if (0 != gai_error || MMDB_SUCCESS != mmdb_error) {
            apr_table_set(r->subprocess_env, "MMDB_INFO", "lookup failed");

            const char *msg = 0 != gai_error ? "failed to resolve IP address" :
                              MMDB_strerror(mmdb_error);
            ERROR(r->server, "Error looking up '%s': %s", ipaddr,
                  msg);
            continue;
        }

        apr_table_set(r->subprocess_env, "MMDB_INFO", "lookup success");

        INFO(r->server, "MMDB_lookup_string %s works", ipaddr);

        if (lookup_result.found_entry) {

            for (key_value_list_s *kv = sl->next; kv; kv = kv->next) {
                apr_table_set(r->subprocess_env, "MMDB_INFO", "result found");

                MMDB_entry_data_s result;
                mmdb_error = MMDB_aget_value(
                    &lookup_result.entry, &result,
                    &kv->names[1]);
                if (mmdb_error == MMDB_LOOKUP_PATH_DOES_NOT_MATCH_DATA_ERROR) {
                    // INFO(r->server, MMDB_strerror(mmdb_error));
                    continue;
                } else if (mmdb_error != MMDB_SUCCESS) {
                    ERROR(r->server, "Error getting data for '%s': %s", ipaddr,
                          MMDB_strerror(mmdb_error));
                    continue;
                }
                if (result.offset > 0) {
                    char *value = NULL;

                    switch (result.type) {
                    case MMDB_DATA_TYPE_BOOLEAN:
                        value = apr_psprintf(r->pool, "%d", result.boolean);
                        break;
                    case MMDB_DATA_TYPE_UTF8_STRING:
                        value = apr_pstrmemdup(r->pool, result.utf8_string,
                                               result.data_size);
                        break;
                    case MMDB_DATA_TYPE_BYTES:
                        /* XXX - treating bytes as strings is broken.
                           They may contain null characters */
                        value = apr_pstrmemdup(r->pool,
                                               (const char *)result.bytes,
                                               result.data_size);
                        break;
                    case MMDB_DATA_TYPE_FLOAT:
                        value = apr_psprintf(r->pool, "%.5f",
                                             result.float_value);
                        break;
                    case MMDB_DATA_TYPE_DOUBLE:
                        value = apr_psprintf(r->pool, "%.5f",
                                             result.double_value);
                        break;
                    case MMDB_DATA_TYPE_UINT16:
                        value = apr_psprintf(r->pool, "%d", result.uint16);
                        break;
                    case MMDB_DATA_TYPE_UINT32:
                        value = apr_psprintf(r->pool, "%u", result.uint32);
                        break;
                    case MMDB_DATA_TYPE_INT32:
                        value = apr_psprintf(r->pool, "%d", result.int32);
                        break;
                    case MMDB_DATA_TYPE_UINT64:
                        value = apr_psprintf(r->pool, "%" PRIu64, result.uint64);
                        break;
                    case MMDB_DATA_TYPE_UINT128:
                        value = from_uint128(r->pool, &result);
                        break;
                    default:
                        ERROR(r->server, "Database error: unknown data type");
                        continue;
                    }

                    if (NULL != value) {
                        apr_table_set(r->subprocess_env, kv->env_key, value);
                    }
                }
            }
        }
    }
}

static char * from_uint128(apr_pool_t *pool,
                           const MMDB_entry_data_s *result)
{

#if MMDB_UINT128_IS_BYTE_ARRAY
    uint8_t *p = (uint8_t *)result->uint128;
    return apr_psprintf(pool, "0x"
                        "%02x%02x%02x%02x"
                        "%02x%02x%02x%02x"
                        "%02x%02x%02x%02x"
                        "%02x%02x%02x%02x",
                        p[0], p[1], p[2], p[3],
                        p[4], p[5], p[6], p[7],
                        p[8], p[9], p[10], p[11],
                        p[12], p[13], p[14], p[15]);
#else

    mmdb_uint128_t v = result->uint128;
    return apr_psprintf(pool,
                        "0x%016" PRIx64 "%016" PRIx64,
                        (uint64_t)(v >> 64),
                        (uint64_t)v);
#endif
}
