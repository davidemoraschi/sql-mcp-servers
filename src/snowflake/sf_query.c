/*
 * sf_query.c - Snowflake session and SQL execution (query-request REST API)
 *
 * Line budget constraint: < 450 lines
 */
#ifndef _WIN32
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
  #endif
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif
#include "sf_query.h"
#include "sf_auth.h"
#include "sf_buf.h"
#include "sf_http.h"

#define SF_HOST_SUFFIX       ".snowflakecomputing.com"
#define SF_DEFAULT_QUERY_SEC 300
#define SF_POLL_INTERVAL_MS  1000
#define SF_REQ_TIMEOUT_MS    60000

typedef struct {
    bool configured;
    SfSessionConfig cfg;          /* numeric settings only; strings live in the copies below */
    char *account, *user, *role, *warehouse, *database, *schema;
    char host[300];
    char *token;                  /* session token, NULL when signed out        */
    bool context_applied;
    long long sequence_id;
} SfSession;

static SfSession g;

/* ---------- helpers ---------- */

static char *sq_dup(const char *s) {
    size_t n;
    char *p;
    if (!s || !*s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void sq_wipe_free(char **s) {
    if (*s) {
        volatile unsigned char *v = (volatile unsigned char *)*s;
        size_t n = strlen(*s);
        while (n--) *v++ = 0;
        free(*s);
        *s = NULL;
    }
}

static void sq_set_error(SfQueryResult *res, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    free(res->error_message);
    res->error_message = sq_dup(buf);
    res->success = false;
}

/* Role/warehouse/database/schema names: plain unquoted identifiers only. */
static bool sq_valid_name(const char *s) {
    if (!s) return true;
    if (!*s || strlen(s) > 255) return false;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '$') return false;
    }
    return true;
}

static void sq_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

void sf_query_free(SfQueryResult *res) {
    if (!res) return;
    json_free(res->root);
    free(res->error_message);
    memset(res, 0, sizeof(*res));
}

/* ---------- configuration ---------- */

void sf_session_close(void) {
    sq_wipe_free(&g.token);
    free(g.account); free(g.user); free(g.role);
    free(g.warehouse); free(g.database); free(g.schema);
    memset(&g, 0, sizeof(g));
}

bool sf_session_configure(const SfSessionConfig *cfg, char *err, size_t err_size) {
    sf_session_close();
    if (!cfg || !cfg->account || !cfg->account[0]) {
        snprintf(err, err_size, "SNOWFLAKE_ACCOUNT is required");
        return false;
    }
    if (!cfg->user || !cfg->user[0]) {
        snprintf(err, err_size, "SNOWFLAKE_USER is required");
        return false;
    }
    if (!sq_valid_name(cfg->role) || !sq_valid_name(cfg->warehouse) ||
        !sq_valid_name(cfg->database) || !sq_valid_name(cfg->schema)) {
        snprintf(err, err_size, "Role, warehouse, database and schema may only contain letters, digits, '_' and '$'");
        return false;
    }
    if (strlen(cfg->account) + sizeof(SF_HOST_SUFFIX) > sizeof(g.host)) {
        snprintf(err, err_size, "Account identifier too long");
        return false;
    }
    g.account = sq_dup(cfg->account);
    g.user = sq_dup(cfg->user);
    g.role = sq_dup(cfg->role);
    g.warehouse = sq_dup(cfg->warehouse);
    g.database = sq_dup(cfg->database);
    g.schema = sq_dup(cfg->schema);
    g.cfg = *cfg;
    g.cfg.account = g.cfg.user = g.cfg.role = NULL;   /* caller-owned: never use */
    g.cfg.warehouse = g.cfg.database = g.cfg.schema = NULL;
    g.sequence_id = 0;
    snprintf(g.host, sizeof(g.host), "%s%s", cfg->account, SF_HOST_SUFFIX);
    g.configured = g.account && g.user;
    if (!g.configured) {
        snprintf(err, err_size, "Out of memory");
        sf_session_close();
        return false;
    }
    return true;
}

/* ---------- one request/response round trip ---------- */

typedef enum { SQ_OK, SQ_FAILED, SQ_REAUTH } SqOutcome;

static bool sq_code_is(const JsonValue *root, const char *code) {
    const JsonValue *c = json_object_get(root, "code");
    char num[32];
    if (!c) return false;
    if (c->type == JSON_STRING) return strcmp(c->u.string, code) == 0;
    if (c->type == JSON_NUMBER) {
        snprintf(num, sizeof(num), "%.0f", c->u.number);
        return strcmp(num, code) == 0;
    }
    return false;
}

static bool sq_session_expired(const JsonValue *root) {
    return sq_code_is(root, "390111") || sq_code_is(root, "390112") ||
           sq_code_is(root, "390114");
}

static bool sq_in_progress(const JsonValue *root) {
    return sq_code_is(root, "333333") || sq_code_is(root, "333334");
}

static bool sq_do_request(const char *method, const char *path, const char *body,
                          SfHttpResponse *http, SfQueryResult *res) {
    char *auth;
    size_t auth_size;
    SfHttpRequest rq;
    const char *t;
    bool ok;
    for (t = g.token; *t; t++) {
        if (*t == '"' || *t == '\r' || *t == '\n') {
            sq_set_error(res, "Session token has an unexpected format");
            return false;
        }
    }
    auth_size = strlen(g.token) + 64;
    auth = (char *)malloc(auth_size);
    if (!auth) { sq_set_error(res, "Out of memory"); return false; }
    snprintf(auth, auth_size, "Authorization: Snowflake Token=\"%s\"", g.token);
    rq.method = method;
    rq.host = g.host;
    rq.path_and_query = path;
    rq.json_body = body;
    rq.auth_header = auth;
    rq.timeout_ms = SF_REQ_TIMEOUT_MS;
    ok = sf_http_request(&rq, http);
    memset(auth, 0, auth_size);
    free(auth);
    if (!ok) {
        sq_set_error(res, "Request to Snowflake failed: %s", http->error ? http->error : "transport error");
        return false;
    }
    if (http->status < 200 || http->status >= 300) {
        sq_set_error(res, "Snowflake returned HTTP status %d", http->status);
        return false;
    }
    return true;
}

static char *sq_build_body(const char *sql) {
    SfBuf b = {NULL, 0, 0, false};
    sf_buf_str(&b, "{\"sqlText\":\"");
    sf_buf_json_escaped(&b, sql);
    sf_buf_printf(&b, "\",\"asyncExec\":false,\"sequenceId\":%lld,\"querySubmissionTime\":%lld}",
                  ++g.sequence_id, (long long)time(NULL) * 1000LL);
    return sf_buf_take(&b);
}

/* Parse rel="next" and rel="last" from a Link header value.
   Fills next_url (up to next_size bytes) and sets *is_last. */
static void sq_parse_link(const char *link, char *next_url, size_t next_size, bool *is_last) {
    *is_last = false;
    next_url[0] = '\0';
    if (!link) return;
    /* Format: <url>; rel="next", <url>; rel="last" */
    const char *p = link;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '<') break;
        p++;
        const char *url_start = p;
        while (*p && *p != '>') p++;
        size_t url_len = (size_t)(p - url_start);
        if (*p == '>') p++;
        /* skip to rel= */
        while (*p == ' ' || *p == ';') p++;
        bool is_next = false, is_last_rel = false;
        if (strncmp(p, "rel=\"next\"", 10) == 0)  { is_next = true;     p += 10; }
        else if (strncmp(p, "rel=\"last\"", 11) == 0) { is_last_rel = true; p += 11; }
        if (is_next && url_len > 0 && url_len < next_size) {
            memcpy(next_url, url_start, url_len);
            next_url[url_len] = '\0';
        }
        if (is_last_rel) *is_last = true;
    }
}


static SqOutcome sq_execute(const char *sql, SfQueryResult *res) {
    SfHttpResponse http;
    char req_id[37], path[96];
    char *body = sq_build_body(sql);
    const char *method = "POST";
    char poll_path[256];
    const char *cur_path = path;
    const char *cur_body = body;
    time_t deadline = time(NULL) + (g.cfg.query_timeout_seconds > 0 ? g.cfg.query_timeout_seconds
                                                                    : SF_DEFAULT_QUERY_SEC);
    SqOutcome out = SQ_FAILED;
    const JsonValue *data;
    const JsonValue *fmt = NULL;

    memset(&http, 0, sizeof(http));
    if (!body) { sq_set_error(res, "Out of memory"); return SQ_FAILED; }
    sf_auth_request_id(req_id);
    snprintf(path, sizeof(path), "/queries/v1/query-request?requestId=%s", req_id);

    for (;;) {
        const JsonValue *url;
        if (!sq_do_request(method, cur_path, cur_body, &http, res)) goto done;
        json_free(res->root);
        res->root = json_parse(http.body);
        /* Capture Link header before freeing http */
        sq_parse_link(http.link_header,
                      res->next_page_url, sizeof(res->next_page_url),
                      &res->is_last_page);
        if (res->next_page_url[0]) res->more_chunks = true;
        sf_http_free_response(&http);
        if (!res->root || res->root->type != JSON_OBJECT) {
            sq_set_error(res, "Snowflake returned an unreadable response");
            goto done;
        }
        if (sq_session_expired(res->root)) { out = SQ_REAUTH; goto done; }
        if (sq_in_progress(res->root)) {            /* long-running: poll the result URL */
            data = json_object_get(res->root, "data");
            url = json_object_get(data, "getResultUrl");
            if (!url || url->type != JSON_STRING || url->u.string[0] != '/' ||
                strlen(url->u.string) >= sizeof(poll_path)) {
                sq_set_error(res, "Query is still running and no result URL was provided");
                goto done;
            }
            if (time(NULL) >= deadline) {
                sq_set_error(res, "Timed out waiting for the query to finish (SNOWFLAKE_QUERY_TIMEOUT_SECONDS)");
                goto done;
            }
            snprintf(poll_path, sizeof(poll_path), "%s", url->u.string);
            method = "GET";
            cur_path = poll_path;
            cur_body = NULL;
            sq_sleep_ms(SF_POLL_INTERVAL_MS);
            continue;
        }
        break;
    }

    {
        const JsonValue *ok = json_object_get(res->root, "success");
        if (!ok || ok->type != JSON_BOOL || !ok->u.boolean) {
            const char *m = json_string_value(json_object_get(res->root, "message"));
            const JsonValue *c = json_object_get(res->root, "code");
            sq_set_error(res, "%s%s%s%s", m ? m : "Query failed",
                         c && c->type == JSON_STRING ? " (code " : "",
                         c && c->type == JSON_STRING ? c->u.string : "",
                         c && c->type == JSON_STRING ? ")" : "");
            goto done;
        }
    }
    data = json_object_get(res->root, "data");
    fmt = json_object_get(data, "queryResultFormat");
    {
        /* jsonv2 (newer Snowflake): format under resultSetMetaData, rows in top-level data array,
           columns in resultSetMetaData.rowType, partitionInfo in resultSetMetaData, handle at root.
           v1/json (legacy): format under data.queryResultFormat, rows in data.rowset,
           columns in data.rowtype, partitionInfo in data, handle in data. */
        const JsonValue *rmd = json_object_get(res->root, "resultSetMetaData");
        fmt = json_object_get(data, "queryResultFormat");
        if (!fmt) fmt = json_object_get(rmd, "format");
        if (fmt && fmt->type == JSON_STRING &&
            strcmp(fmt->u.string, "json") != 0 &&
            strcmp(fmt->u.string, "jsonv2") != 0) {
            sq_set_error(res, "Snowflake returned the result in '%s' format; only JSON is supported", fmt->u.string);
            goto done;
        }
        res->rowtype = json_object_get(data, "rowtype");
        if (!res->rowtype) res->rowtype = json_object_get(rmd, "rowType");
        res->rowset = json_object_get(data, "rowset");
        if (!res->rowset && data && data->type == JSON_ARRAY) res->rowset = data;
    }
    {
        const JsonValue *rmd = json_object_get(res->root, "resultSetMetaData");
        const JsonValue *pi = json_object_get(data, "partitionInfo");
        if (!pi) pi = json_object_get(rmd, "partitionInfo");
        const JsonValue *sh = json_object_get(data, "statementHandle");
        if (!sh) sh = json_object_get(res->root, "statementHandle");
        if (pi && pi->type == JSON_ARRAY && pi->u.array.count > 0) {
            res->more_chunks = true;
            res->partition_count = (int)pi->u.array.count + 1; /* +1 for partition 0 */
        } else {
            const JsonValue *chunks = json_object_get(data, "chunks");
            res->more_chunks = chunks && chunks->type == JSON_ARRAY && chunks->u.array.count > 0;
            res->partition_count = res->more_chunks ? 0 : 1;
        }
        if (sh && sh->type == JSON_STRING && sh->u.string) {
            size_t slen = strlen(sh->u.string);
            if (slen < sizeof(res->statement_handle))
                memcpy(res->statement_handle, sh->u.string, slen + 1);
        }
    }
    res->success = true;
    out = SQ_OK;

done:
    sf_http_free_response(&http);
    free(body);
    return out;
}

/* ---------- sign-in and context ---------- */

static bool sq_sign_in(SfQueryResult *res) {
    SfAuthConfig ac;
    SfAuthResult ar;
    memset(&ac, 0, sizeof(ac));
    ac.account = g.account;
    ac.user = g.user;
    ac.callback_port = g.cfg.callback_port;
    ac.timeout_seconds = g.cfg.sso_timeout_seconds;
    ar = sf_auth_external_browser(&ac);
    if (!ar.success) {
        sq_set_error(res, "Snowflake sign-in failed: %s", ar.error_message ? ar.error_message : "unknown error");
        sf_auth_free_result(&ar);
        return false;
    }
    sq_wipe_free(&g.token);
    g.token = ar.session_token;      /* take ownership; scrubbed in sf_session_close */
    ar.session_token = NULL;
    g.context_applied = false;
    sf_auth_free_result(&ar);
    return true;
}

static bool sq_use(const char *what, const char *name, SfQueryResult *res) {
    char sql[320];
    SfQueryResult r;
    SqOutcome o;
    if (!name) return true;
    snprintf(sql, sizeof(sql), "USE %s %s", what, name);
    memset(&r, 0, sizeof(r));
    o = sq_execute(sql, &r);
    if (o == SQ_REAUTH) sq_wipe_free(&g.token);
    if (o != SQ_OK) {
        sq_set_error(res, "Could not %s: %s", sql, r.error_message ? r.error_message : "session expired");
    }
    sf_query_free(&r);
    return o == SQ_OK;
}

static bool sq_ensure_ready(SfQueryResult *res) {
    if (!g.token && !sq_sign_in(res)) return false;
    if (g.context_applied) return true;
    if (!sq_use("ROLE", g.role, res) || !sq_use("WAREHOUSE", g.warehouse, res) ||
        !sq_use("DATABASE", g.database, res) || !sq_use("SCHEMA", g.schema, res)) {
        return false;
    }
    g.context_applied = true;
    return true;
}

SfQueryResult sf_session_query(const char *sql) {
    SfQueryResult res;
    SqOutcome o;
    int attempt;
    memset(&res, 0, sizeof(res));
    if (!g.configured) { sq_set_error(&res, "Snowflake session is not configured"); return res; }
    if (!sql || !*sql) { sq_set_error(&res, "SQL query cannot be null or empty"); return res; }

    for (attempt = 0; attempt < 2; attempt++) {
        if (!sq_ensure_ready(&res)) return res;
        json_free(res.root);
        res.root = NULL;
        o = sq_execute(sql, &res);
        if (o != SQ_REAUTH) return res;
        sq_wipe_free(&g.token);        /* session expired: sign in again, retry once */
    }
    sq_set_error(&res, "Snowflake session expired and could not be renewed");
    return res;
}

SfQueryResult sf_session_fetch_partition(const char *statement_handle, int partition) {
    SfQueryResult res;
    SfHttpResponse http;
    char path[256];
    const char *p;
    memset(&res, 0, sizeof(res));
    memset(&http, 0, sizeof(http));
    if (!g.token) { sq_set_error(&res, "No active session"); return res; }
    /* Validate handle: alphanumeric, hyphens, underscores only */
    for (p = statement_handle; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            sq_set_error(&res, "Invalid statement handle"); return res;
        }
    }
    snprintf(path, sizeof(path), "/api/v2/statements/%s?partition=%d", statement_handle, partition);
    if (!sq_do_request("GET", path, NULL, &http, &res)) return res;
    res.root = json_parse(http.body);
    sf_http_free_response(&http);
    if (!res.root || res.root->type != JSON_OBJECT) {
        sq_set_error(&res, "Unreadable partition response"); return res;
    }
    /* Partition responses: {"data": [[row], [row], ...]} */
    res.rowset = json_object_get(res.root, "data");
    if (!res.rowset) {
        sq_set_error(&res, "No data in partition response"); return res;
    }
    res.success = true;
    return res;
}

SfQueryResult sf_session_fetch_chunk(const char *url, const JsonValue *chunk_headers) {
    SfQueryResult res;
    SfHttpResponse http;
    SfBuf headers = {NULL, 0, 0, false}, endpoint = {NULL, 0, 0, false}, body = {NULL, 0, 0, false};
    const char *end;
    memset(&res, 0, sizeof(res));
    memset(&http, 0, sizeof(http));
    if (!url || !*url) {
        sq_set_error(&res, "Invalid Snowflake result chunk URL"); return res;
    }
    if (url[0] == '/') {
        sf_buf_printf(&endpoint, "https://%s%s", g.host, url);
        url = endpoint.p;
    }
    if (!url || (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)) {
        sf_buf_free(&endpoint);
        sq_set_error(&res, "Invalid Snowflake result chunk URL"); return res;
    }
    if (chunk_headers && chunk_headers->type == JSON_OBJECT) {
        for (size_t i = 0; i < chunk_headers->u.object.count; i++) {
            const char *name = chunk_headers->u.object.keys[i];
            const char *value = json_string_value(chunk_headers->u.object.values[i]);
            if (name && value) sf_buf_printf(&headers, "%s: %s\r\n", name, value);
        }
    }
    if (headers.oom || !sf_http_get_url(url, headers.p, true, SF_REQ_TIMEOUT_MS, &http)) {
        sq_set_error(&res, "Snowflake result chunk request failed: %s", http.error ? http.error : "transport error");
        sf_http_free_response(&http); sf_buf_free(&headers); sf_buf_free(&endpoint); return res;
    }
    sf_buf_free(&headers); sf_buf_free(&endpoint);
    if (http.status < 200 || http.status >= 300) {
        sq_set_error(&res, "Snowflake result chunk returned HTTP status %d", http.status);
        sf_http_free_response(&http); return res;
    }
    /* Session chunks are comma-separated row arrays, not a complete JSON array. */
    end = http.body ? http.body + strlen(http.body) : NULL;
    while (end && end > http.body && (isspace((unsigned char)end[-1]) || end[-1] == ',')) end--;
    sf_buf_str(&body, "[");
    if (end) sf_buf_add(&body, http.body, (size_t)(end - http.body));
    sf_buf_str(&body, "]");
    sf_http_free_response(&http);
    res.root = json_parse(body.p ? body.p : "[]");
    sf_buf_free(&body);
    if (!res.root || res.root->type != JSON_ARRAY) {
        sq_set_error(&res, "Unreadable Snowflake result chunk"); return res;
    }
    res.rowset = res.root;
    res.success = true;
    return res;
}

SfQueryResult sf_session_fetch_page(const char *path_and_query) {
    SfQueryResult res;
    SfHttpResponse http;
    memset(&res, 0, sizeof(res));
    memset(&http, 0, sizeof(http));
    if (!g.token) { sq_set_error(&res, "No active session"); return res; }
    if (!path_and_query || path_and_query[0] != '/') {
        sq_set_error(&res, "Invalid page URL"); return res;
    }
    if (!sq_do_request("GET", path_and_query, NULL, &http, &res)) return res;
    sq_parse_link(http.link_header,
                  res.next_page_url, sizeof(res.next_page_url),
                  &res.is_last_page);
    res.root = json_parse(http.body);
    sf_http_free_response(&http);
    if (!res.root || res.root->type != JSON_OBJECT) {
        sq_set_error(&res, "Unreadable page response"); return res;
    }
    /* v1 page response: same envelope as initial query */
    const JsonValue *data = json_object_get(res.root, "data");
    res.rowset = json_object_get(data, "rowset");
    if (!res.rowset && data && data->type == JSON_ARRAY) res.rowset = data;
    if (!res.rowset) { sq_set_error(&res, "No rowset in page response"); return res; }
    res.more_chunks = (res.next_page_url[0] != '\0');
    res.success = true;
    return res;
}
