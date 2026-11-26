/* BigQuery REST backend for the shared db_* tools. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "db_bigquery.h"
#include "json_rpc.h"
#include "sf_http.h"
#include "sf_buf.h"
#include "bq_oauth.h"

#define BQ_MAX_ROWS 1000
#define BQ_DEFAULT_HOST "bigquery.googleapis.com"
#define BQ_TOKEN_HOST "oauth2.googleapis.com"

typedef struct {
    char *host, *project, *dataset, *location;
    char *client_id, *client_secret, *refresh_token, *access_token;
    time_t token_expiry;
    bool ready, read_only;
    int timeout_seconds;
} BqSession;

static BqSession g_bq;

static char *bq_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void bq_wipe_free(char **s) {
    if (*s) {
        volatile unsigned char *p = (volatile unsigned char *)*s;
        size_t n = strlen(*s);
        while (n--) *p++ = 0;
        free(*s); *s = NULL;
    }
}

static DbQueryResult bq_fail(const char *msg) {
    DbQueryResult r; memset(&r, 0, sizeof(r));
    r.error_message = bq_dup(msg ? msg : "BigQuery operation failed");
    return r;
}

DbQueryResult bq_fail_external(const char *message) { return bq_fail(message); }

void db_free_result(DbQueryResult *r) {
    if (!r) return;
    free(r->markdown_table); free(r->json_structured); free(r->error_message);
    memset(r, 0, sizeof(*r));
}

static bool bq_valid_host(const char *s) {
    if (!s || !*s || strlen(s) > 250 || strstr(s, "://") || strchr(s, '/') || strchr(s, '\\')) return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-') return false;
    return true;
}

static bool bq_valid_project(const char *s) {
    if (!s || !*s || strlen(s) > 128) return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '-') return false;
    return true;
}

bool db_validate_identifier(const char *s) {
    int parts = 0;
    if (!s || !*s) return false;
    while (*s) {
        if (!isalnum((unsigned char)*s) && *s != '_') return false;
        while (isalnum((unsigned char)*s) || *s == '_' || *s == '-') s++;
        parts++;
        if (!*s) break;
        if (*s++ != '.' || parts == 3 || !*s) return false;
    }
    return parts >= 1 && parts <= 3;
}

static char *bq_form_encode(const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s), cap;
    char *out;
    if (n > ((size_t)-1 - 1) / 3) return NULL;
    cap = n * 3 + 1; out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[j++] = (char)c;
        else if (c == ' ') out[j++] = '+';
        else { out[j++] = '%'; out[j++] = hex[c >> 4]; out[j++] = hex[c & 15]; }
    }
    out[j] = '\0'; return out;
}

static bool bq_refresh(void) {
    SfHttpRequest rq; SfHttpResponse http; JsonValue *root;
    if (!g_bq.refresh_token || !g_bq.refresh_token[0]) {
        char err[512] = {0}; char *refresh = NULL, *access = NULL; int ttl = 3600;
        if (!bq_oauth_user_login(g_bq.client_id, g_bq.client_secret,
                                 g_bq.timeout_seconds, &refresh, &access,
                                 &ttl, err, sizeof(err))) {
            fprintf(stderr, "[ERROR] BigQuery user authorization failed: %s\n", err);
            free(refresh); free(access); return false;
        }
        g_bq.refresh_token = refresh;
        bq_oauth_save_cached_refresh_token(refresh);
        g_bq.access_token = access;
        g_bq.token_expiry = time(NULL) + (ttl > 60 ? ttl - 60 : ttl);
        return true;
    }
    char *cid = bq_form_encode(g_bq.client_id);
    char *secret = bq_form_encode(g_bq.client_secret ? g_bq.client_secret : "");
    char *refresh = bq_form_encode(g_bq.refresh_token);
    if (!cid || !secret || !refresh) { free(cid); free(secret); free(refresh); return false; }
    size_t n = strlen(cid) + strlen(secret) + strlen(refresh) + 80;
    char *body = (char *)malloc(n);
    if (!body) { free(cid); free(secret); free(refresh); return false; }
    if (g_bq.client_secret && g_bq.client_secret[0])
        snprintf(body, n, "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token", cid, secret, refresh);
    else
        snprintf(body, n, "client_id=%s&refresh_token=%s&grant_type=refresh_token", cid, refresh);
    free(cid); free(secret); free(refresh);
    memset(&rq, 0, sizeof(rq));
    rq.method = "POST"; rq.host = BQ_TOKEN_HOST; rq.path_and_query = "/token";
    rq.json_body = body; rq.content_type = "application/x-www-form-urlencoded";
    rq.timeout_ms = g_bq.timeout_seconds * 1000;
    bool sent = sf_http_request(&rq, &http);
    memset(body, 0, n); free(body);
    if (!sent || http.status < 200 || http.status >= 300 || !http.body) {
        sf_http_free_response(&http); return false;
    }
    root = json_parse(http.body); sf_http_free_response(&http);
    if (!root) return false;
    const char *token = json_string_value(json_object_get(root, "access_token"));
    const JsonValue *exp = json_object_get(root, "expires_in");
    int ttl = exp && exp->type == JSON_NUMBER ? (int)exp->u.number : 3600;
    char *copy = token ? bq_dup(token) : NULL;
    json_free(root);
    if (!copy) return false;
    bq_wipe_free(&g_bq.access_token);
    g_bq.access_token = copy; g_bq.token_expiry = time(NULL) + (ttl > 60 ? ttl - 60 : ttl);
    return true;
}

static bool bq_http(const char *method, const char *host, const char *path,
                    const char *body, const char *content_type, SfHttpResponse *http,
                    char *err, size_t err_size) {
    SfHttpRequest rq; char *auth = NULL;
    if (host == NULL) host = g_bq.host;
    memset(&rq, 0, sizeof(rq));
    if (host != BQ_TOKEN_HOST) {
        if (!g_bq.access_token || time(NULL) >= g_bq.token_expiry) {
            if (!bq_refresh()) { snprintf(err, err_size, "Google user OAuth failed; check the Desktop OAuth client configuration and user consent"); return false; }
        }
        size_t n = strlen(g_bq.access_token) + 32;
        auth = (char *)malloc(n);
        if (!auth) { snprintf(err, err_size, "Out of memory"); return false; }
        snprintf(auth, n, "Authorization: Bearer %s", g_bq.access_token);
    }
    rq.method = method; rq.host = host; rq.path_and_query = path; rq.json_body = body;
    rq.content_type = content_type ? content_type : "application/json";
    rq.auth_header = auth; rq.timeout_ms = g_bq.timeout_seconds * 1000;
    bool ok = sf_http_request(&rq, http);
    if (auth) { memset(auth, 0, strlen(auth)); free(auth); }
    if (!ok) { snprintf(err, err_size, "BigQuery HTTPS request failed: %s", http->error ? http->error : "transport error"); sf_http_free_response(http); return false; }
    if (http->status < 200 || http->status >= 300) {
        const char *message = NULL; JsonValue *e = http->body ? json_parse(http->body) : NULL;
        const JsonValue *obj = e ? json_object_get(e, "error") : NULL;
        message = json_string_value(json_object_get(obj, "message"));
        snprintf(err, err_size, "Google API HTTP %d: %s", http->status, message ? message : "request failed");
        json_free(e); sf_http_free_response(http); return false;
    }
    return true;
}

JsonValue *bq_api_json(const char *method, const char *path, const char *body,
                       char *err, size_t err_size) {
    SfHttpResponse http; JsonValue *root;
    if (!bq_http(method, NULL, path, body, "application/json", &http, err, err_size)) return NULL;
    root = http.body ? json_parse(http.body) : NULL;
    if (!root) snprintf(err, err_size, "BigQuery returned invalid JSON");
    sf_http_free_response(&http);
    if (root) {
        const char *message = json_string_value(json_object_get(json_object_get(root, "error"), "message"));
        if (message) { snprintf(err, err_size, "%s", message); json_free(root); return NULL; }
    }
    return root;
}

bool bq_db_init(const BqConfig *cfg, char *err, size_t err_size) {
    memset(&g_bq, 0, sizeof(g_bq));
    if (!cfg || !bq_valid_project(cfg->project_id)) { snprintf(err, err_size, "BIGQUERY_PROJECT_ID is required and must be a valid project ID"); return false; }
    if (!cfg->oauth_client_id || !cfg->oauth_client_id[0]) {
        snprintf(err, err_size, "BIGQUERY_OAUTH_CLIENT_ID is required for user OAuth"); return false;
    }
    if (!bq_valid_host(cfg->host ? cfg->host : BQ_DEFAULT_HOST)) { snprintf(err, err_size, "BIGQUERY_HOST must be a hostname without a scheme or path"); return false; }
    g_bq.host = bq_dup(cfg->host ? cfg->host : BQ_DEFAULT_HOST);
    g_bq.project = bq_dup(cfg->project_id); g_bq.dataset = bq_dup(cfg->dataset);
    g_bq.location = bq_dup(cfg->location ? cfg->location : "US");
    g_bq.client_id = bq_dup(cfg->oauth_client_id); g_bq.client_secret = bq_dup(cfg->oauth_client_secret);
    g_bq.refresh_token = bq_dup((cfg->oauth_refresh_token && cfg->oauth_refresh_token[0]) ? cfg->oauth_refresh_token : NULL);
    if (!g_bq.refresh_token) g_bq.refresh_token = bq_oauth_load_cached_refresh_token();
    g_bq.read_only = cfg->read_only; g_bq.timeout_seconds = cfg->timeout_seconds > 0 ? cfg->timeout_seconds : 60;
    if (!g_bq.host || !g_bq.project || !g_bq.location || !g_bq.client_id) {
        snprintf(err, err_size, "Out of memory"); bq_db_close(); return false;
    }
    g_bq.ready = true;
    return true;
}

void bq_db_close(void) {
    free(g_bq.host); free(g_bq.project); free(g_bq.dataset); free(g_bq.location);
    free(g_bq.client_id); bq_wipe_free(&g_bq.client_secret); bq_wipe_free(&g_bq.refresh_token);
    bq_wipe_free(&g_bq.access_token); memset(&g_bq, 0, sizeof(g_bq));
}

bool bq_is_read_only(void) { return g_bq.read_only; }
const char *bq_project_id(void) { return g_bq.project; }
const char *bq_location(void) { return g_bq.location; }

static const char *bq_skip_sql_noise(const char *p) {
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (p[0]=='-'&&p[1]=='-') { while(*p&&*p!='\n')p++; }
        else if (p[0]=='/'&&p[1]=='*') { p+=2; while(*p&&!(p[0]=='*'&&p[1]=='/'))p++; if(*p)p+=2; }
        else return p;
    }
}

static bool bq_read_sql(const char *sql) {
    const char *p=bq_skip_sql_noise(sql); char word[24]; size_t n=0;
    while(isalpha((unsigned char)*p)&&n+1<sizeof(word))word[n++]=(char)toupper((unsigned char)*p++);
    word[n]='\0';
    if(strcmp(word,"SELECT")&&strcmp(word,"WITH")&&strcmp(word,"EXPLAIN")&&strcmp(word,"SHOW")&&strcmp(word,"DESCRIBE"))return false;
    char quote=0;
    for(;*p;p++){
        if(quote){if(*p=='\\'&&p[1])p++;else if(*p==quote){if(p[1]==quote)p++;else quote=0;}continue;}
        if(*p=='\''||*p=='"'||*p=='`'){quote=*p;continue;}
        if(p[0]=='-'&&p[1]=='-'){while(*p&&*p!='\n')p++;continue;}
        if(p[0]=='/'&&p[1]=='*'){p+=2;while(*p&&!(p[0]=='*'&&p[1]=='/'))p++;if(*p)p++;continue;}
        if(*p==';')return *bq_skip_sql_noise(p+1)=='\0';
    }
    return true;
}

static void bq_sleep(void) {
#ifdef _WIN32
    Sleep(500);
#else
    struct timespec t = {0, 500000000L}; nanosleep(&t, NULL);
#endif
}

static char *bq_url_encode(const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s); char *out = (char *)malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[j++] = (char)c;
        else { out[j++] = '%'; out[j++] = hex[c >> 4]; out[j++] = hex[c & 15]; }
    }
    out[j] = '\0'; return out;
}

static bool bq_query_error(const JsonValue *root, char *err, size_t err_size) {
    const JsonValue *problem = NULL;
    const JsonValue *errors = json_object_get(root, "errors");
    if (errors && errors->type == JSON_ARRAY && errors->u.array.count)
        problem = errors->u.array.items[0];
    if (!problem) {
        const JsonValue *status = json_object_get(root, "status");
        problem = json_object_get(status, "errorResult");
    }
    if (!problem || problem->type != JSON_OBJECT) return false;
    const char *message = json_string_value(json_object_get(problem, "message"));
    const char *reason = json_string_value(json_object_get(problem, "reason"));
    const char *location = json_string_value(json_object_get(problem, "location"));
    if (reason && location)
        snprintf(err, err_size, "BigQuery query failed (%s at %s): %s", reason, location, message ? message : "unknown error");
    else if (reason)
        snprintf(err, err_size, "BigQuery query failed (%s): %s", reason, message ? message : "unknown error");
    else
        snprintf(err, err_size, "BigQuery query failed: %s", message ? message : "unknown error");
    return true;
}

static JsonValue *bq_query_start(const char *sql, char *err, size_t err_size) {
    SfBuf body = {NULL,0,0,false}; char path[512]; char *project = bq_url_encode(g_bq.project);
    if (!project) { snprintf(err, err_size, "Out of memory"); return NULL; }
    snprintf(path, sizeof(path), "/bigquery/v2/projects/%s/queries", project); free(project);
    sf_buf_str(&body, "{\"query\":\""); sf_buf_json_escaped(&body, sql);
    sf_buf_str(&body, "\",\"useLegacySql\":false,\"timeoutMs\":20000,\"maxResults\":1000");
    if (g_bq.location) { sf_buf_str(&body, ",\"location\":\""); sf_buf_json_escaped(&body, g_bq.location); sf_buf_str(&body, "\""); }
    if (g_bq.dataset) {
        sf_buf_str(&body, ",\"defaultDataset\":{\"projectId\":\""); sf_buf_json_escaped(&body, g_bq.project);
        sf_buf_str(&body, "\",\"datasetId\":\""); sf_buf_json_escaped(&body, g_bq.dataset); sf_buf_str(&body, "\"}");
    }
    sf_buf_str(&body, "}");
    char *json = sf_buf_take(&body); if (!json) { snprintf(err, err_size, "Out of memory"); return NULL; }
    JsonValue *root = bq_api_json("POST", path, json, err, err_size); free(json);
    if (!root) return NULL;
    if (bq_query_error(root, err, err_size)) { json_free(root); return NULL; }
    const JsonValue *done = json_object_get(root, "jobComplete");
    if (done && done->type == JSON_BOOL && done->u.boolean) return root;

    const JsonValue *jr = json_object_get(root, "jobReference");
    const char *job_id = json_string_value(json_object_get(jr, "jobId"));
    if (!job_id) { json_free(root); snprintf(err, err_size, "BigQuery query returned no completed job or job ID"); return NULL; }
    char *job = bq_url_encode(job_id); project = bq_url_encode(g_bq.project);
    if (!job || !project) { free(job); free(project); json_free(root); snprintf(err, err_size, "Out of memory"); return NULL; }
    char poll[1024];
    snprintf(poll, sizeof(poll), "/bigquery/v2/projects/%s/queries/%s?location=%s&maxResults=1000", project, job, g_bq.location);
    free(project); free(job); json_free(root); root = NULL;
    for (int i = 0; i < g_bq.timeout_seconds * 2; i++) {
        bq_sleep(); root = bq_api_json("GET", poll, NULL, err, err_size);
        if (!root) return NULL;
        if (bq_query_error(root, err, err_size)) { json_free(root); return NULL; }
        done = json_object_get(root, "jobComplete");
        if (done && done->type == JSON_BOOL && done->u.boolean) return root;
        json_free(root); root = NULL;
    }
    snprintf(err, err_size, "BigQuery query timed out waiting for job completion"); return NULL;
}

static void bq_render_json(SfBuf *b, const JsonValue *v) {
    if (!v || v->type == JSON_NULL) { sf_buf_str(b,"null"); return; }
    if (v->type == JSON_STRING) { sf_buf_str(b,"\""); sf_buf_json_escaped(b,v->u.string); sf_buf_str(b,"\""); return; }
    if (v->type == JSON_NUMBER) { sf_buf_printf(b,"%.15g",v->u.number); return; }
    if (v->type == JSON_BOOL) { sf_buf_str(b,v->u.boolean?"true":"false"); return; }
    if (v->type == JSON_ARRAY) {
        sf_buf_str(b,"[");
        for (size_t i=0;i<v->u.array.count;i++) { if(i)sf_buf_str(b,","); bq_render_json(b,v->u.array.items[i]); }
        sf_buf_str(b,"]"); return;
    }
    if (v->type == JSON_OBJECT) {
        sf_buf_str(b,"{");
        for (size_t i=0;i<v->u.object.count;i++) { if(i)sf_buf_str(b,","); sf_buf_str(b,"\""); sf_buf_json_escaped(b,v->u.object.keys[i]); sf_buf_str(b,"\":"); bq_render_json(b,v->u.object.values[i]); }
        sf_buf_str(b,"}");
    }
}

static char *bq_value_text(const JsonValue *v, bool *is_null) {
    if (!v || v->type == JSON_NULL) { *is_null = true; return bq_dup(""); }
    *is_null = false;
    if (v->type == JSON_STRING) return bq_dup(v->u.string ? v->u.string : "");
    char buf[64];
    if (v->type == JSON_NUMBER) snprintf(buf, sizeof(buf), "%.15g", v->u.number);
    else if (v->type == JSON_BOOL) snprintf(buf, sizeof(buf), "%s", v->u.boolean ? "true" : "false");
    else {
        SfBuf b={NULL,0,0,false}; bq_render_json(&b,v); return sf_buf_take(&b);
    }
    return bq_dup(buf);
}

static void bq_md_cell(SfBuf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '|') sf_buf_str(b, "\\|");
        else if (c < 0x20) sf_buf_str(b, " ");
        else sf_buf_add(b, (const char *)&c, 1);
    }
}

static bool bq_append_rows(const JsonValue *root, const JsonValue *schema,
                           SfBuf *md, SfBuf *js, size_t *shown, bool *have_header) {
    const JsonValue *rows = json_object_get(root, "rows");
    const JsonValue *fields = json_object_get(schema, "fields");
    size_t cols = fields && fields->type == JSON_ARRAY ? fields->u.array.count : 0;
    if (!*have_header) {
        sf_buf_str(md, "|");
        for (size_t c = 0; c < cols; c++) {
            const char *name = json_string_value(json_object_get(fields->u.array.items[c], "name"));
            sf_buf_str(md, " "); bq_md_cell(md, name ? name : "col"); sf_buf_str(md, " |");
        }
        sf_buf_str(md, "\n|"); for (size_t c = 0; c < cols; c++) sf_buf_str(md, "---|");
        sf_buf_str(js, "["); *have_header = true;
    }
    if (!rows || rows->type != JSON_ARRAY) return true;
    for (size_t r = 0; r < rows->u.array.count && *shown < BQ_MAX_ROWS; r++) {
        const JsonValue *f = json_object_get(rows->u.array.items[r], "f");
        sf_buf_str(md, "\n|"); sf_buf_str(js, *shown ? ",{" : "{");
        for (size_t c = 0; c < cols; c++) {
            const char *name = json_string_value(json_object_get(fields->u.array.items[c], "name"));
            const JsonValue *cell = (f && f->type == JSON_ARRAY && c < f->u.array.count)
                ? json_object_get(f->u.array.items[c], "v") : NULL;
            bool is_null = false; char *text = bq_value_text(cell, &is_null);
            if (!text) return false;
            sf_buf_str(md, " "); bq_md_cell(md, text); sf_buf_str(md, " |");
            sf_buf_str(js, c ? ",\"" : "\""); sf_buf_json_escaped(js, name ? name : "col");
            if (is_null) sf_buf_str(js, "\":null");
            else { sf_buf_str(js, "\":\""); sf_buf_json_escaped(js, text); sf_buf_str(js, "\""); }
            free(text);
        }
        sf_buf_str(js, "}"); (*shown)++;
    }
    return !md->oom && !js->oom;
}

DbQueryResult bq_run(const char *sql) {
    char err[1024] = ""; JsonValue *root = bq_query_start(sql, err, sizeof(err));
    if (!root) return bq_fail(err);
    JsonValue *initial = root;
    const JsonValue *schema = json_object_get(initial, "schema");
    const JsonValue *jr = json_object_get(root, "jobReference");
    const char *job_id = json_string_value(json_object_get(jr, "jobId"));
    char *job = job_id ? bq_url_encode(job_id) : NULL;
    char *project = bq_url_encode(g_bq.project);
    SfBuf md = {NULL,0,0,false}, js = {NULL,0,0,false};
    size_t shown = 0; bool header = false, ok = true;
    ok = bq_append_rows(root, schema, &md, &js, &shown, &header);
    const char *page_token = json_string_value(json_object_get(root, "pageToken"));
    char *token = page_token ? bq_dup(page_token) : NULL;
    for (int page = 0; ok && token && *token && shown < BQ_MAX_ROWS && job && project; page++) {
        char *encoded = bq_url_encode(token); free(token); token = NULL;
        if (!encoded) { ok = false; break; }
        char path[2048];
        snprintf(path, sizeof(path), "/bigquery/v2/projects/%s/queries/%s?location=%s&pageToken=%s&maxResults=1000", project, job, g_bq.location, encoded);
        free(encoded);
        root = bq_api_json("GET", path, NULL, err, sizeof(err));
        if (!root) { ok = false; break; }
        ok = bq_append_rows(root, schema, &md, &js, &shown, &header);
        page_token = json_string_value(json_object_get(root, "pageToken"));
        token = page_token ? bq_dup(page_token) : NULL;
        json_free(root);
        if (page > 10000) { ok = false; snprintf(err, sizeof(err), "BigQuery pagination limit exceeded"); }
    }
    json_free(initial);
    free(token); free(job); free(project);
    if (!header) { sf_buf_str(&md, "| status |\n|---|\n| ok |"); sf_buf_str(&js, "["); }
    sf_buf_str(&js, "]");
    if (!ok || md.oom || js.oom) { sf_buf_free(&md); sf_buf_free(&js); return bq_fail(err[0] ? err : "Out of memory formatting BigQuery results"); }
    if (shown >= BQ_MAX_ROWS) sf_buf_printf(&md, "\n\n(Result limited to %d rows.)", BQ_MAX_ROWS);
    DbQueryResult res; memset(&res, 0, sizeof(res));
    res.markdown_table = sf_buf_take(&md); res.json_structured = sf_buf_take(&js);
    if (!res.markdown_table || !res.json_structured) { db_free_result(&res); return bq_fail("Out of memory"); }
    res.success = true; res.rows_affected = (int)shown; return res;
}

DbQueryResult db_execute_read(const char *sql, const char *params_json) {
    (void)params_json;
    if (!sql || !*sql) return bq_fail("SQL query cannot be empty");
    if (!bq_read_sql(sql)) return bq_fail("read_query accepts SELECT, WITH, EXPLAIN, SHOW, or DESCRIBE; use write_query for modifications");
    return bq_run(sql);
}

DbQueryResult db_execute_write(const char *sql, const char *params_json) {
    (void)params_json;
    if (!sql || !*sql) return bq_fail("SQL query cannot be empty");
    if (g_bq.read_only) return bq_fail("Server is READONLY (BIGQUERY_ACCESS_MODE)");
    return bq_run(sql);
}

DbQueryResult db_backup(const char *target_path) { (void)target_path; return bq_fail("backup_database is not supported for BigQuery"); }

DbQueryResult db_explain(const char *sql) {
    if (!sql || !*sql) return bq_fail("SQL query cannot be empty");
    SfBuf b = {NULL,0,0,false}; sf_buf_str(&b, "EXPLAIN "); sf_buf_str(&b, sql);
    char *q = sf_buf_take(&b); if (!q) return bq_fail("Out of memory");
    DbQueryResult r = bq_run(q); free(q); return r;
}

bool bq_table_parts(const char *name, char *project, size_t ps,
                    char *dataset, size_t ds, char *table, size_t ts) {
    char tmp[512]; char *parts[3]; int n = 0;
    if (!name || strlen(name) >= sizeof(tmp)) return false;
    snprintf(tmp, sizeof(tmp), "%s", name);
    char *p = tmp;
    while (p && n < 3) { parts[n++] = p; char *dot = strchr(p, '.'); if (dot) { *dot = '\0'; p = dot + 1; } else p = NULL; }
    if (p || !db_validate_identifier(name)) return false;
    if (n == 3) { snprintf(project, ps, "%s", parts[0]); snprintf(dataset, ds, "%s", parts[1]); snprintf(table, ts, "%s", parts[2]); }
    else if (n == 2) { snprintf(project, ps, "%s", g_bq.project); snprintf(dataset, ds, "%s", parts[0]); snprintf(table, ts, "%s", parts[1]); }
    else if (g_bq.dataset) { snprintf(project, ps, "%s", g_bq.project); snprintf(dataset, ds, "%s", g_bq.dataset); snprintf(table, ts, "%s", parts[0]); }
    else return false;
    return bq_valid_project(project) && dataset[0] && table[0];
}

