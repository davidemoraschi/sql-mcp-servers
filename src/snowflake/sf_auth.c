/*
 * sf_auth.c - Snowflake external-browser (SSO) authentication
 *
 * Port of kiro-course ExternalBrowserCredential + SnowflakeProtocol.
 * Line budget constraint: < 450 lines
 */
#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#ifdef _WIN32
  #define _CRT_RAND_S
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include "json_rpc.h"
#include "sf_auth.h"
#include "sf_buf.h"
#include "sf_http.h"
#include "sf_loopback.h"

/* Same identifiers as the C++ library so Snowflake sees identical clients. */
#define SF_CLIENT_APP_ID      "SnowflakeAuthLib"
#define SF_CLIENT_APP_VERSION "1.0.0"
#define SF_HOST_SUFFIX        ".snowflakecomputing.com"
#define SF_HTTP_TIMEOUT_MS    30000
#define SF_DEFAULT_WAIT_SEC   120
#define SF_MAX_FIELD          255

/* ---------- small utilities ---------- */

static char *sf_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Overwrite secret memory in a way the optimiser may not remove. */
static void sf_wipe(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static void sf_wipe_str(char *s) {
    if (s) sf_wipe(s, strlen(s));
}

static void sf_fail(SfAuthResult *res, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    res->success = false;
    free(res->error_message);
    res->error_message = sf_strdup(buf);
}

static void sfb_field(SfBuf *b, const char *key, const char *val, bool last) {
    sf_buf_str(b, "\"");
    sf_buf_str(b, key);
    sf_buf_str(b, "\":\"");
    sf_buf_json_escaped(b, val);
    sf_buf_str(b, last ? "\"" : "\",");
}

/* ---------- identifier handling ---------- */

/* Account identifiers become part of a hostname: allow only [A-Za-z0-9._-]. */
static bool sf_valid_account(const char *a) {
    size_t i, n;
    if (!a || !a[0]) return false;
    n = strlen(a);
    if (n > SF_MAX_FIELD) return false;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)a[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) return false;
    }
    return a[0] != '.' && a[0] != '-';
}

bool sf_auth_account_name(const char *account, char *out, size_t out_size) {
    const char *dot, *dash, *start;
    size_t n, i;
    if (!account || !out || out_size == 0) return false;
    dot = strchr(account, '.');
    n = dot ? (size_t)(dot - account) : strlen(account);
    start = account;
    dash = (const char *)memchr(account, '-', n);
    if (dash) {
        start = dash + 1;
        n -= (size_t)(start - account);
    }
    if (n == 0 || n + 1 > out_size) return false;
    for (i = 0; i < n; i++) out[i] = (char)toupper((unsigned char)start[i]);
    out[n] = '\0';
    return true;
}

/* ---------- query-string parsing ---------- */

static int sf_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *sf_url_decode(const char *in, size_t n) {
    char *out = (char *)malloc(n + 1);
    size_t i, o = 0;
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        if (in[i] == '%' && i + 2 < n &&
            sf_hexval((unsigned char)in[i + 1]) >= 0 &&
            sf_hexval((unsigned char)in[i + 2]) >= 0) {
            out[o++] = (char)(sf_hexval((unsigned char)in[i + 1]) * 16 +
                              sf_hexval((unsigned char)in[i + 2]));
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
    return out;
}

char *sf_auth_query_value(const char *query, const char *name) {
    size_t name_len;
    const char *p = query;
    if (!query || !name) return NULL;
    name_len = strlen(name);
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        if (pair_len > name_len && p[name_len] == '=' && strncmp(p, name, name_len) == 0) {
            return sf_url_decode(p + name_len + 1, pair_len - name_len - 1);
        }
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

/* ---------- request ids ---------- */

static void sf_random_bytes(unsigned char *out, size_t n) {
    size_t i;
#ifdef _WIN32
    for (i = 0; i < n; i += 4) {
        unsigned int v = 0;
        size_t k;
        rand_s(&v);
        for (k = 0; k < 4 && i + k < n; k++) out[i + k] = (unsigned char)(v >> (8 * k));
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(out, 1, n, f) : 0;
    if (f) fclose(f);
    if (got != n) {                    /* weak fallback: ids only need uniqueness */
        srand((unsigned)time(NULL));
        for (i = got; i < n; i++) out[i] = (unsigned char)(rand() & 0xFF);
    }
#endif
}

void sf_auth_request_id(char out[37]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char b[16];
    size_t i, o = 0;
    sf_random_bytes(b, sizeof(b));
    for (i = 0; i < sizeof(b); i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex[b[i] >> 4];
        out[o++] = hex[b[i] & 0x0F];
    }
    out[o] = '\0';
}

/* ---------- protocol bodies and responses ---------- */

static char *sf_build_body(const char *acct_name, const char *user,
                           const char *port_str, const char *token,
                           const char *proof_key) {
    SfBuf b = {NULL, 0, 0, false};
    sf_buf_str(&b, "{\"data\":{");
    sfb_field(&b, "CLIENT_APP_ID", SF_CLIENT_APP_ID, false);
    sfb_field(&b, "CLIENT_APP_VERSION", SF_CLIENT_APP_VERSION, false);
    sfb_field(&b, "ACCOUNT_NAME", acct_name, false);
    sfb_field(&b, "LOGIN_NAME", user, false);
    if (port_str) {             /* authenticator-request */
        sfb_field(&b, "AUTHENTICATOR", "EXTERNALBROWSER", false);
        sfb_field(&b, "BROWSER_MODE_REDIRECT_PORT", port_str, true);
    } else {                    /* login-request */
        sfb_field(&b, "AUTHENTICATOR", "EXTERNALBROWSER", false);
        sfb_field(&b, "TOKEN", token, false);
        sfb_field(&b, "PROOF_KEY", proof_key, true);
    }
    sf_buf_str(&b, "}}");
    return sf_buf_take(&b);
}

/* Parsed Snowflake envelope: {"success":bool,"message":..,"data":{..}}. */
typedef struct {
    JsonValue *root;
    bool success;
} SfEnvelope;

static bool sf_parse_envelope(const char *json, SfEnvelope *env) {
    JsonValue *ok;
    env->root = json ? json_parse(json) : NULL;
    env->success = false;
    if (!env->root || env->root->type != JSON_OBJECT) return false;
    ok = json_object_get(env->root, "success");
    env->success = ok && ok->type == JSON_BOOL && ok->u.boolean;
    return true;
}

static const char *sf_env_data(const SfEnvelope *env, const char *key) {
    JsonValue *data = json_object_get(env->root, "data");
    return json_string_value(json_object_get(data, key));
}

static const char *sf_env_message(const SfEnvelope *env) {
    return json_string_value(json_object_get(env->root, "message"));
}

static void sf_env_free(SfEnvelope *env) {
    json_free(env->root);
    env->root = NULL;
}

/* ---------- public API ---------- */

void sf_auth_free_result(SfAuthResult *res) {
    if (!res) return;
    sf_wipe_str(res->session_token);
    sf_wipe_str(res->master_token);
    free(res->session_token);
    free(res->master_token);
    free(res->error_message);
    memset(res, 0, sizeof(*res));
}

/* POST and require a 2xx status. On failure fills res and returns false. */
static bool sf_post_checked(SfAuthResult *res, const char *host, const char *path,
                            const char *body, const char *what, SfHttpResponse *resp) {
    if (!sf_http_post_json(host, path, body, SF_HTTP_TIMEOUT_MS, resp)) {
        sf_fail(res, "%s failed: %s", what, resp->error ? resp->error : "transport error");
        return false;
    }
    if (resp->status < 200 || resp->status >= 300) {
        sf_fail(res, "%s failed with HTTP status %d", what, resp->status);
        return false;
    }
    return true;
}

SfAuthResult sf_auth_external_browser(const SfAuthConfig *cfg) {
    SfAuthResult res;
    SfHttpResponse resp;
    SfEnvelope env = {NULL, false};
    SfLoopback *lb = NULL;
    SfCallback cb;
    char acct_name[SF_MAX_FIELD + 1];
    char host[SF_MAX_FIELD + sizeof(SF_HOST_SUFFIX) + 1];
    char port_str[8], path[96], req_id[37];
    char *body = NULL, *sso_url = NULL, *proof_key = NULL, *token = NULL;
    const char *s;
    int wait_ms;

    memset(&res, 0, sizeof(res));
    memset(&resp, 0, sizeof(resp));
    memset(&cb, 0, sizeof(cb));

    if (!cfg || !sf_valid_account(cfg->account)) {
        sf_fail(&res, "Account identifier is required and may contain only letters, digits, '.', '-' and '_'");
        return res;
    }
    if (!cfg->user || !cfg->user[0] || strlen(cfg->user) > SF_MAX_FIELD) {
        sf_fail(&res, "Username (login name) is required");
        return res;
    }
    if (!sf_auth_account_name(cfg->account, acct_name, sizeof(acct_name))) {
        sf_fail(&res, "Invalid account identifier");
        return res;
    }
    snprintf(host, sizeof(host), "%s%s", cfg->account, SF_HOST_SUFFIX);
    wait_ms = (cfg->timeout_seconds > 0 ? cfg->timeout_seconds : SF_DEFAULT_WAIT_SEC) * 1000;

    /* Step 1: loopback listener, so the redirect port is known. */
    lb = sf_loopback_start(cfg->callback_port);
    if (!lb) {
        sf_fail(&res, "Could not start the local SSO callback server on 127.0.0.1");
        return res;
    }
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)sf_loopback_port(lb));

    /* Step 2: ask Snowflake for the IdP SSO URL and proof key. */
    body = sf_build_body(acct_name, cfg->user, port_str, NULL, NULL);
    if (!body) { sf_fail(&res, "Out of memory"); goto done; }
    if (!sf_post_checked(&res, host, "/session/authenticator-request", body,
                         "Authenticator-request", &resp)) goto done;
    free(body);
    body = NULL;

    if (!sf_parse_envelope(resp.body, &env) || !env.success ||
        !(s = sf_env_data(&env, "ssoUrl")) || !s[0]) {
        const char *msg = env.root ? sf_env_message(&env) : NULL;
        sf_fail(&res, "Authenticator-request did not return an SSO URL: %s",
                msg ? msg : "no ssoUrl in response");
        goto done;
    }
    sso_url = sf_strdup(s);
    s = sf_env_data(&env, "proofKey");
    proof_key = sf_strdup(s ? s : "");
    sf_env_free(&env);
    sf_http_free_response(&resp);
    if (!sso_url || !proof_key) { sf_fail(&res, "Out of memory"); goto done; }

    /* Step 3: send the user to the IdP. stderr only: stdout is JSON-RPC. */
    fprintf(stderr,
            "\n=== Snowflake external-browser sign-in ===\n"
            "Open this URL in your browser to sign in:\n\n  %s\n\n"
            "Waiting for the sign-in to complete (listening on http://127.0.0.1:%s/) ...\n",
            sso_url, port_str);
    fflush(stderr);
    if (!cfg->skip_browser_launch) sf_browser_open(sso_url);

    /* Step 4: wait for the browser to redirect back with the token. */
    cb = sf_loopback_wait(lb, wait_ms);
    sf_loopback_stop(lb);
    lb = NULL;
    if (cb.timed_out) { sf_fail(&res, "Timed out waiting for the browser sign-in to complete"); goto done; }
    if (!cb.received) { sf_fail(&res, "No callback received from the browser"); goto done; }

    /* Snowflake sends the token as ?token=... (GET) or in a form body (POST). */
    token = sf_auth_query_value(cb.query, "token");
    if (!token || !token[0]) {
        free(token);
        token = sf_auth_query_value(cb.body, "token");
    }
    if (!token || !token[0]) { sf_fail(&res, "Browser callback did not contain an SSO token"); goto done; }

    /* Step 5: exchange browser token + proof key for a session token. */
    sf_auth_request_id(req_id);
    snprintf(path, sizeof(path), "/session/v1/login-request?requestId=%s", req_id);
    body = sf_build_body(acct_name, cfg->user, NULL, token, proof_key);
    if (!body) { sf_fail(&res, "Out of memory"); goto done; }
    if (!sf_post_checked(&res, host, path, body, "External-browser login", &resp)) goto done;

    if (!sf_parse_envelope(resp.body, &env) || !env.success ||
        !(s = sf_env_data(&env, "token")) || !s[0]) {
        const char *msg = env.root ? sf_env_message(&env) : NULL;
        sf_fail(&res, "External-browser login failed: %s", msg ? msg : "no session token in response");
        goto done;
    }
    res.session_token = sf_strdup(s);
    s = sf_env_data(&env, "masterToken");
    if (s && s[0]) res.master_token = sf_strdup(s);
    if (!res.session_token) { sf_fail(&res, "Out of memory"); goto done; }
    res.success = true;

done:
    if (body) { sf_wipe_str(body); free(body); }
    sf_wipe_str(token);
    sf_wipe_str(proof_key);
    free(token);
    free(proof_key);
    free(sso_url);
    sf_env_free(&env);
    sf_http_free_response(&resp);
    sf_loopback_free_callback(&cb);
    sf_loopback_stop(lb);
    if (!res.success) {
        sf_wipe_str(res.session_token);
        free(res.session_token);
        res.session_token = NULL;
        free(res.master_token);
        res.master_token = NULL;
    }
    return res;
}
