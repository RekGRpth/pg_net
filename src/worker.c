#include <postgres.h>

#include <access/xact.h>
#include <catalog/pg_type.h>
#include <commands/extension.h>
#include <executor/spi.h>
#include <miscadmin.h>
#include <pgstat.h>
#include <postmaster/bgworker.h>
#include <storage/ipc.h>
#include <storage/proc.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <utils/jsonb.h>
#include <utils/memutils.h>
#include <utils/snapmgr.h>

#include <curl/curl.h>

#include <uv.h>

PG_MODULE_MAGIC;

void _PG_init(void);
void worker_main(Datum main_arg) pg_attribute_noreturn();

static char *ttl;

static volatile sig_atomic_t got_sighup = false;

static uv_loop_t *loop;
static uv_idle_t idle;
static uv_timer_t timer;
static CURLM *cm;

static void handle_sigterm(SIGNAL_ARGS) { uv_stop(loop); }

static void handle_sighup(SIGNAL_ARGS) {
    int save_errno = errno;
    got_sighup = true;
    if (MyProc)
        SetLatch(&MyProc->procLatch);
    errno = save_errno;
}

struct curl_data {
    bool done;
    int64 id;
    char *method;
    char *url;
    struct curl_slist *request_headers;
    char *request_body;
    JsonbParseState *response_headers;
    StringInfo response_body;
    MemoryContext mem_ctx;
};

struct curl_context {
    uv_poll_t poll;
    curl_socket_t socket_fd;
    struct curl_data *data;
};

static void close_cb(uv_handle_t *handle) {
    struct curl_context *ctx =
        (struct curl_context *)uv_handle_get_data(handle);

    elog(LOG, "curl_close_cb %ld", ctx->data->id);

    if (ctx->data->done) {
        MemoryContext mem_ctx = MemoryContextSwitchTo(ctx->data->mem_ctx);
        if (ctx->data->method) {
            free(ctx->data->method);
        }
        if (ctx->data->url) {
            free(ctx->data->url);
        }
        if (ctx->data->request_headers) {
            curl_slist_free_all(ctx->data->request_headers);
        }
        if (ctx->data->request_body) {
            free(ctx->data->request_body);
        }
        // response_body &response_headers should be freed when mem_ctx is
        // freed.
        MemoryContextSwitchTo(mem_ctx);
        MemoryContextDelete(ctx->data->mem_ctx);
        free(ctx->data);
    }

    free(ctx);
}

static bool is_extension_loaded(void) {
    Oid extension_oid;

    StartTransactionCommand();
    extension_oid = get_extension_oid("pg_net", true);
    CommitTransactionCommand();

    return OidIsValid(extension_oid);
}

static size_t body_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    struct curl_data *data = (struct curl_data *)userp;
    MemoryContext mem_ctx = MemoryContextSwitchTo(data->mem_ctx);
    size_t realsize = size * nmemb;
    StringInfo si = data->response_body;

    /* elog(LOG, "body_cb"); */

    appendBinaryStringInfo(si, (const char *)contents, (int)realsize);

    MemoryContextSwitchTo(mem_ctx);
    return realsize;
}

static size_t header_cb(void *contents, size_t size, size_t nmemb,
                        void *userp) {
    struct curl_data *data = (struct curl_data *)userp;
    MemoryContext mem_ctx = MemoryContextSwitchTo(data->mem_ctx);
    size_t realsize = size * nmemb;
    JsonbParseState *headers = data->response_headers;

    /* per curl docs, the status code is included in the header data
     * (it starts with: HTTP/1.1 200 OK or HTTP/2 200 OK)*/
    bool firstLine = strncmp(contents, "HTTP/", 5) == 0;
    /* the final(end of headers) last line is empty - just a CRLF */
    bool lastLine = strncmp(contents, "\r\n", 2) == 0;

    /* elog(LOG, "header_cb"); */

    /*Ignore non-header data in the first header line and last header
line*/
    if (!firstLine && !lastLine) {
        /*TODO: make the parsing more robust, test with invalid headers*/
        char *token;
        char *tmp = pstrdup(contents);
        JsonbValue key, val;

        /*The header comes as "Header-Key: val", split by whitespace and
ditch
         * the colon later*/
        token = strtok(tmp, " ");

        key.type = jbvString;
        key.val.string.val = token;
        /*strlen - 1 because we ditch the last char - the colon*/
        key.val.string.len = strlen(token) - 1;
        (void)pushJsonbValue(&headers, WJB_KEY, &key);

        /*Every header line ends with CRLF, split and remove it*/
        token = strtok(NULL, "\r\n");

        val.type = jbvString;
        val.val.string.val = token;
        val.val.string.len = strlen(token);
        (void)pushJsonbValue(&headers, WJB_VALUE, &val);
    }

    MemoryContextSwitchTo(mem_ctx);
    return realsize;
}

static struct curl_slist *pg_text_array_to_slist(ArrayType *array,
                                                 struct curl_slist *headers) {
    ArrayIterator iterator;
    Datum value;
    bool isnull;
    char *header;

    iterator = array_create_iterator(array, 0, NULL);

    while (array_iterate(iterator, &value, &isnull)) {
        if (isnull) {
            continue;
        }

        header = TextDatumGetCString(value);
        headers = curl_slist_append(headers, header);
        pfree(header);
    }
    array_free_iterator(iterator);

    return headers;
}

static void submit_request(int64 id, char *method, char *url,
                           struct curl_slist *headers, char *body) {
    struct curl_data *data = (struct curl_data *)malloc(sizeof(*data));
    MemoryContext mem_ctx =
        AllocSetContextCreate(TopMemoryContext, NULL, ALLOCSET_DEFAULT_SIZES);
    CURL *easy = curl_easy_init();

    /* elog(LOG, "submit_request"); */

    headers = curl_slist_append(headers, "User-Agent: pg_net/0.2");

    data->mem_ctx = mem_ctx;
    mem_ctx = MemoryContextSwitchTo(data->mem_ctx);
    data->done = false;
    data->id = id;
    data->method = method;
    data->url = url;
    data->request_headers = headers;
    data->request_body = body;
    data->response_headers = NULL;
    data->response_body = makeStringInfo();

    pushJsonbValue(&data->response_headers, WJB_BEGIN_OBJECT, NULL);

    if (strcasecmp(method, "GET") == 0) {
        if (body) {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "GET");
        }
    } else if (strcasecmp(method, "POST") == 0) {
        if (body) {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
        }
    } else {
        elog(ERROR, "error: Unsupported request method %s\n", method);
    }

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, data);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, data);
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, data->request_headers);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, (void *)data);
    if (log_min_messages <= DEBUG2) {
        curl_easy_setopt(easy, CURLOPT_VERBOSE, 1);
    }
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_multi_add_handle(cm, easy);

    MemoryContextSwitchTo(mem_ctx);
}

static void check_curl_multi_info(void) {
    struct curl_data *data;
    CURL *easy;
    CURLMsg *msg;
    int pending;
    int rc;

    /* elog(LOG, "check_curl_multi_info"); */

    while ((msg = curl_multi_info_read(cm, &pending))) {
        switch (msg->msg) {
        case CURLMSG_DONE:
            easy = msg->easy_handle;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &data);
            data->done = true;

            elog(LOG, "CURLMSG_DONE: %ld", data->id);

            StartTransactionCommand();
            PushActiveSnapshot(GetTransactionSnapshot());
            SPI_connect();
            rc = msg->data.result;
            if (rc != CURLE_OK) {
                char *sql = "UPDATE\n"
                            "  net._http_response\n"
                            "SET\n"
                            "  error_msg = $1\n"
                            "WHERE\n"
                            "  id = $2";

                int argCount = 2;
                Oid argTypes[2];
                Datum argValues[2];

                const char *error_msg = curl_easy_strerror(rc);
                argTypes[0] = CSTRINGOID;
                argValues[0] = CStringGetDatum(error_msg);

                argTypes[1] = INT8OID;
                argValues[1] = Int64GetDatum(data->id);

                if (SPI_execute_with_args(sql, argCount, argTypes, argValues,
                                          NULL, false, 0) != SPI_OK_UPDATE) {
                    elog(ERROR, "SPI_exec failed:\n%s", sql);
                }
            } else {
                char *sql = "UPDATE\n"
                            "  net._http_response\n"
                            "SET\n"
                            "  status_code = $1,\n"
                            "  content = $2,\n"
                            "  headers = $3,\n"
                            "  content_type = $4,\n"
                            "  timed_out = $5\n"
                            "WHERE\n"
                            "  id = $6";

                int argCount = 6;
                Oid argTypes[6];
                Datum argValues[6];
                char nulls[6];
                int http_status_code;
                char *contentType = NULL;
                bool timedOut = false;

                curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE,
                                  &http_status_code);
                curl_easy_getinfo(msg->easy_handle, CURLINFO_CONTENT_TYPE,
                                  &contentType);
                // NOTE: ctx mysteriously becomes NULL after the getinfo calls
                // above.
                curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &data);

                argTypes[0] = INT4OID;
                argValues[0] = Int32GetDatum(http_status_code);
                nulls[0] = ' ';

                argTypes[1] = CSTRINGOID;
                argValues[1] = CStringGetDatum(data->response_body->data);
                if (data->response_body->data[0] == '\0') {
                    nulls[1] = 'n';
                } else {
                    nulls[1] = ' ';
                }

                argTypes[2] = JSONBOID;
                argValues[2] = JsonbPGetDatum(JsonbValueToJsonb(pushJsonbValue(
                    &data->response_headers, WJB_END_OBJECT, NULL)));
                nulls[2] = ' ';

                argTypes[3] = CSTRINGOID;
                argValues[3] = CStringGetDatum(contentType);
                if (!contentType)
                    nulls[3] = 'n';
                else
                    nulls[3] = ' ';

                argTypes[4] = BOOLOID;
                argValues[4] = BoolGetDatum(timedOut);
                nulls[4] = ' ';

                argTypes[5] = INT8OID;
                argValues[5] = Int64GetDatum(data->id);
                nulls[5] = ' ';

                if (SPI_execute_with_args(sql, argCount, argTypes, argValues,
                                          nulls, false, 0) != SPI_OK_UPDATE) {
                    elog(ERROR, "SPI_exec failed:\n%s", sql);
                }
            }
            SPI_finish();
            PopActiveSnapshot();
            CommitTransactionCommand();

            curl_multi_remove_handle(cm, easy);
            curl_easy_cleanup(easy);
            break;
        default:
            elog(ERROR, "Unexpected CURLMSG: %d", msg->msg);
        }
    }
}

static void poll_cb(uv_poll_t *poll, int status, int events) {
    int flags = 0;
    struct curl_context *ctx;
    int running_handles;

    /* elog(LOG, "poll_cb: req %p, status %d, events %d", poll, status, events);
     */

    if (status < 0)
        flags = CURL_CSELECT_ERR;
    if (!status && events & UV_READABLE)
        flags |= CURL_CSELECT_IN;
    if (!status && events & UV_WRITABLE)
        flags |= CURL_CSELECT_OUT;

    ctx = (struct curl_context *)uv_handle_get_data((uv_handle_t *)poll);

    curl_multi_socket_action(cm, ctx->socket_fd, flags, &running_handles);

    check_curl_multi_info();
}

static int socket_cb(CURL *easy, curl_socket_t s, int what, void *userp,
                     void *socketp) {
    struct curl_context *ctx;
    struct curl_data *data;
    int events = 0;

    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &data);

    elog(LOG, "socket_cb: socket %d, action %d, socketp %p", s, what, socketp);

    switch (what) {
    case CURL_POLL_IN:
    case CURL_POLL_OUT:
    case CURL_POLL_INOUT:
        if (socketp) {
            ctx = (struct curl_context *)socketp;
        } else {
            /* fprintf(stderr, "(%d) 1st socket_cb\n", data->id); */

            ctx = (struct curl_context *)malloc(sizeof(*ctx));
            ctx->socket_fd = s;
            ctx->data = data;

            uv_poll_init_socket(loop, &ctx->poll, s);
            uv_handle_set_data((uv_handle_t *)&ctx->poll, (void *)ctx);

            curl_multi_assign(cm, s, (void *)ctx);
        }

        if (what != CURL_POLL_IN)
            events |= UV_WRITABLE;
        if (what != CURL_POLL_OUT)
            events |= UV_READABLE;

        uv_poll_start(&ctx->poll, events, poll_cb);
        break;
    case CURL_POLL_REMOVE:
        /* fprintf(stderr, "(%d) CURL_POLL_REMOVE\n", data->id); */

        if (socketp) {
            ctx = (struct curl_context *)socketp;

            uv_poll_stop(&ctx->poll);
            uv_close((uv_handle_t *)&ctx->poll, close_cb);

            curl_multi_assign(cm, s, NULL);
        } else {
            elog(ERROR, "Missing socketp in CURL_POLL_REMOVE");
        }
        break;
    default:
        elog(ERROR, "Unexpected CURL_POLL symbol: %d\n", what);
    }

    return 0;
}

static void on_timeout(uv_timer_t *req) {
    int running_handles;
    curl_multi_socket_action(cm, CURL_SOCKET_TIMEOUT, 0, &running_handles);
    check_curl_multi_info();
}

static int timer_cb(CURLM *cm, long timeout_ms, void *userp) {
    if (timeout_ms < 0) {
        uv_timer_stop(&timer);
    } else {
        // NOTE: 0 means directly call socket_action, but we'll do it in a bit
        if (timeout_ms == 0) {
            timeout_ms = 1;
        }
        uv_timer_start(&timer, on_timeout, timeout_ms, 0);
    }

    return 0;
}

static void idle_cb(uv_idle_t *idle) {
    WaitLatch(&MyProc->procLatch,
              WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 1000,
              PG_WAIT_EXTENSION);
    ResetLatch(&MyProc->procLatch);

    if (!is_extension_loaded()) {
        elog(DEBUG2, "idle_cb: extension not yet loaded");
        return;
    }

    if (got_sighup) {
        got_sighup = false;
        ProcessConfigFile(PGC_SIGHUP);
    }

    // NOTE: Needs verification: any (p)allocations we do within the transaction
    // seems to be freed automatically. Probably because it's within a memory
    // context that is local to the transaction.
    // https://github.com/postgres/postgres/tree/master/src/backend/utils/mmgr
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());
    SPI_connect();
    {
        char *sql = "DELETE FROM\n"
                    "  net.http_request_queue\n"
                    "WHERE\n"
                    "  clock_timestamp() - created > $1::interval";

        int argCount = 1;
        Oid argTypes[1];
        Datum argValues[1];
        int rc;
        Datum *ids;
        int64 num_requests;

        argTypes[0] = TEXTOID;
        argValues[0] = CStringGetTextDatum(ttl);

        rc = SPI_execute_with_args(sql, argCount, argTypes, argValues, NULL,
                                   false, 0);
        if (rc != SPI_OK_DELETE) {
            elog(ERROR, "SPI_exec failed with error code %d:\n%s", rc, sql);
        }

        sql =
            "SELECT\n"
            "  q.id,\n"
            "  q.method,\n"
            "  q.url,\n"
            "  array(\n"
            "    select key || ': ' || value from jsonb_each_text(q.headers)\n"
            "  ),\n"
            "  q.body\n"
            "FROM net.http_request_queue q\n"
            "LEFT JOIN net._http_response r ON q.id = r.id\n"
            "WHERE r.id IS NULL";
        rc = SPI_execute(sql, true, 0);
        if (rc != SPI_OK_SELECT) {
            elog(ERROR, "SPI_execute() failed with error code %d:\n%s", rc,
                 sql);
        }

        num_requests = SPI_processed;
        ids = (Datum *)palloc(sizeof(*ids) * num_requests);

        for (int64 i = 0; i < num_requests; i++) {
            bool isnull;
            Datum tmp;

            Oid id_type = SPI_gettypeid(SPI_tuptable->tupdesc, 1);
            Datum id_binary_value = SPI_getbinval(
                SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
            int64 id = DatumGetInt64(id_binary_value);

            char *method_tmp = TextDatumGetCString(SPI_getbinval(
                SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2, &isnull));
            char *method = (char *)malloc(strlen(method_tmp) + 1);

            char *url_tmp = TextDatumGetCString(SPI_getbinval(
                SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 3, &isnull));
            char *url = (char *)malloc(strlen(url_tmp) + 1);

            struct curl_slist *headers = NULL;

            char *body = NULL;

            ids[i] = id_binary_value;

            memcpy(method, method_tmp, strlen(method_tmp) + 1);
            memcpy(url, url_tmp, strlen(url_tmp) + 1);
            tmp = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 4,
                                &isnull);
            if (!isnull) {
                headers =
                    pg_text_array_to_slist(DatumGetArrayTypeP(tmp), headers);
            }
            tmp = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 5,
                                &isnull);
            if (!isnull) {
                char *body_tmp = TextDatumGetCString(tmp);
                body = (char *)malloc(strlen(body_tmp) + 1);
                memcpy(body, body_tmp, strlen(body_tmp) + 1);
            }

            submit_request(id, method, url, headers, body);
            elog(LOG, "submitted");
        }

        // TODO: We currently insert an id-only row to the response table to
        // differentiate requests not in progress, requests in progress, and
        // requests fulfilled (whether successful or failed).
        //
        // But this creates a possibility for a request recognized as in
        // progress despite not being processed by curl, e.g. because the
        // worker crashed while fulfilling the request.
        //
        // One solution to this is for the worker to always start in a known
        // good state, e.g. by TRUNCATEing the http_request_queue.
        for (int64 i = 0; i < num_requests; i++) {
            char *sql = "INSERT INTO net._http_response(id) VALUES ($1)";
            Oid argtypes[1] = {INT8OID};
            Datum Values[1] = {ids[i]};

            int rc =
                SPI_execute_with_args(sql, 1, argtypes, Values, NULL, false, 0);
            if (rc != SPI_OK_INSERT) {
                elog(ERROR, "SPI_execute() failed with error code %d:\n%s", rc,
                     sql);
            }
        }
    }
    SPI_finish();
    PopActiveSnapshot();
    CommitTransactionCommand();
}

void worker_main(Datum main_arg) {
    int rc;

    pqsignal(SIGTERM, handle_sigterm);
    pqsignal(SIGHUP, handle_sighup);
    BackgroundWorkerUnblockSignals();
    // TODO: Unhardcode dbname
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    loop = uv_default_loop();

    uv_idle_init(loop, &idle);
    uv_idle_start(&idle, idle_cb);

    rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc) {
        elog(ERROR, "curl_global_init() error: %s", curl_easy_strerror(rc));
    }

    uv_timer_init(loop, &timer);

    cm = curl_multi_init();
    curl_multi_setopt(cm, CURLMOPT_MAX_TOTAL_CONNECTIONS, 100);
    curl_multi_setopt(cm, CURLMOPT_SOCKETFUNCTION, socket_cb);
    curl_multi_setopt(cm, CURLMOPT_TIMERFUNCTION, timer_cb);

    uv_run(loop, UV_RUN_DEFAULT);

    proc_exit(0);
}

void _PG_init(void) {
    BackgroundWorker bgw;

    if (IsBinaryUpgrade) {
        return;
    }

    if (!process_shared_preload_libraries_in_progress) {
        ereport(ERROR, errmsg("pg_net is not in shared_preload_libraries"),
                errhint("Add pg_net to the shared_preload_libraries "
                        "configuration variable in postgresql.conf."));
    }

    DefineCustomStringVariable("pg_net.ttl",
                               "time to live for request/response rows",
                               "should be a valid interval type", &ttl,
                               "3 days", PGC_SIGHUP, 0, NULL, NULL, NULL);

    memset(&bgw, 0, sizeof(bgw));
    bgw.bgw_flags =
        BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
    snprintf(bgw.bgw_library_name, BGW_MAXLEN, "pg_net");
    snprintf(bgw.bgw_function_name, BGW_MAXLEN, "worker_main");
    snprintf(bgw.bgw_name, BGW_MAXLEN, "pg_net worker");
    bgw.bgw_restart_time = 10;
    RegisterBackgroundWorker(&bgw);
}
