/*
 * sf_http.c - Minimal blocking HTTPS JSON POST client
 *
 * Windows: WinHTTP (system library). Others: libcurl.
 * Line budget constraint: < 450 lines
 */
#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sf_http.h"

#define SF_HTTP_MAX_RESPONSE (4u * 1024u * 1024u)

static char *sf_http_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void sf_http_free_response(SfHttpResponse *resp) {
    if (!resp) return;
    free(resp->body);
    free(resp->error);
    resp->body = NULL;
    resp->error = NULL;
    resp->body_len = 0;
    resp->status = 0;
}

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#ifdef _MSC_VER
  #pragma comment(lib, "winhttp.lib")
#endif

static wchar_t *sf_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static void sf_win_error(SfHttpResponse *resp, const char *what) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s (WinHTTP error %lu)", what,
             (unsigned long)GetLastError());
    resp->error = sf_http_dup(buf);
}

static bool sf_win_read_body(HINTERNET req, SfHttpResponse *resp) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return false;
    for (;;) {
        DWORD avail = 0;
        DWORD got = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { free(buf); return false; }
        if (avail == 0) break;
        if (len + avail + 1 > SF_HTTP_MAX_RESPONSE) { free(buf); return false; }
        if (len + avail + 1 > cap) {
            char *nb;
            while (len + avail + 1 > cap) cap *= 2;
            nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return false; }
            buf = nb;
        }
        if (!WinHttpReadData(req, buf + len, avail, &got)) { free(buf); return false; }
        len += got;
    }
    buf[len] = '\0';
    resp->body = buf;
    resp->body_len = len;
    return true;
}

bool sf_http_request(const SfHttpRequest *rq, SfHttpResponse *resp) {
    HINTERNET ses = NULL, con = NULL, req = NULL;
    const char *json_body = rq->json_body;
    int timeout_ms = rq->timeout_ms;
    bool is_get = rq->method && strcmp(rq->method, "GET") == 0;
    wchar_t *whost = sf_to_wide(rq->host);
    wchar_t *wpath = sf_to_wide(rq->path_and_query);
    wchar_t *wheaders = NULL;
    char *hdr = NULL;
    size_t hdr_size = (rq->auth_header ? strlen(rq->auth_header) : 0) + 96;
    size_t body_len = (!is_get && json_body) ? strlen(json_body) : 0;
    DWORD status = 0, status_sz = sizeof(status);
    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    bool ok = false;

    memset(resp, 0, sizeof(*resp));
    hdr = (char *)malloc(hdr_size);
    if (hdr) snprintf(hdr, hdr_size, "Content-Type: application/json\r\nAccept: application/json\r\n%s%s",
             rq->auth_header ? rq->auth_header : "", rq->auth_header ? "\r\n" : "");
    wheaders = hdr ? sf_to_wide(hdr) : NULL;
    if (!whost || !wpath || !wheaders) { resp->error = sf_http_dup("Out of memory"); goto done; }

    ses = WinHttpOpen(L"sf-auth-c/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { sf_win_error(resp, "WinHttpOpen failed"); goto done; }
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protos |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
    WinHttpSetTimeouts(ses, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    con = WinHttpConnect(ses, whost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) { sf_win_error(resp, "WinHttpConnect failed"); goto done; }

    req = WinHttpOpenRequest(con, is_get ? L"GET" : L"POST", wpath, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { sf_win_error(resp, "WinHttpOpenRequest failed"); goto done; }

    {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    }

    if (!WinHttpSendRequest(req, wheaders, (DWORD)-1L,
                            body_len ? (LPVOID)json_body : NULL,
                            (DWORD)body_len, (DWORD)body_len, 0)) {
        sf_win_error(resp, "WinHttpSendRequest failed");
        goto done;
    }
    if (!WinHttpReceiveResponse(req, NULL)) {
        sf_win_error(resp, "WinHttpReceiveResponse failed");
        goto done;
    }
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
                             WINHTTP_NO_HEADER_INDEX)) {
        sf_win_error(resp, "Reading HTTP status failed");
        goto done;
    }
    resp->status = (int)status;
    if (!sf_win_read_body(req, resp)) {
        sf_win_error(resp, "Reading response body failed");
        goto done;
    }
    ok = true;

done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(whost);
    free(wpath);
    free(wheaders);
    if (hdr) { memset(hdr, 0, hdr_size); free(hdr); }
    return ok;
}

#else /* libcurl */

#include <curl/curl.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} SfCurlBuf;

static size_t sf_curl_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    SfCurlBuf *b = (SfCurlBuf *)ud;
    size_t n = size * nmemb;
    if (n > SF_HTTP_MAX_RESPONSE || b->len + n + 1 > SF_HTTP_MAX_RESPONSE) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        char *nd;
        while (b->len + n + 1 > nc) nc *= 2;
        nd = (char *)realloc(b->data, nc);
        if (!nd) return 0;
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

bool sf_http_request(const SfHttpRequest *rq, SfHttpResponse *resp) {
    static int curl_ready = 0;
    const char *host = rq->host;
    const char *path_and_query = rq->path_and_query;
    const char *json_body = rq->json_body;
    int timeout_ms = rq->timeout_ms;
    bool is_get = rq->method && strcmp(rq->method, "GET") == 0;
    CURL *curl;
    struct curl_slist *hdrs = NULL;
    SfCurlBuf buf = {NULL, 0, 0};
    char errbuf[CURL_ERROR_SIZE];
    char *url;
    size_t url_len;
    long code = 0;
    CURLcode rc;

    memset(resp, 0, sizeof(*resp));
    errbuf[0] = '\0';
    if (!curl_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            resp->error = sf_http_dup("curl_global_init failed");
            return false;
        }
        curl_ready = 1;
    }
    url_len = strlen(host) + strlen(path_and_query) + 16;
    url = (char *)malloc(url_len);
    curl = curl_easy_init();
    if (!url || !curl) {
        free(url);
        if (curl) curl_easy_cleanup(curl);
        resp->error = sf_http_dup("Out of memory");
        return false;
    }
    snprintf(url, url_len, "https://%s%s", host, path_and_query);

    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (rq->auth_header) hdrs = curl_slist_append(hdrs, rq->auth_header);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (is_get) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body ? json_body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(json_body ? strlen(json_body) : 0));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sf_curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp->status = (int)code;
        resp->body = buf.data ? buf.data : sf_http_dup("");
        resp->body_len = buf.len;
    } else {
        char msg[CURL_ERROR_SIZE + 64];
        snprintf(msg, sizeof(msg), "HTTPS request failed: %s",
                 errbuf[0] ? errbuf : curl_easy_strerror(rc));
        resp->error = sf_http_dup(msg);
        free(buf.data);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(url);
    return rc == CURLE_OK;
}

#endif

bool sf_http_post_json(const char *host, const char *path_and_query,
                       const char *json_body, int timeout_ms,
                       SfHttpResponse *resp) {
    SfHttpRequest rq;
    rq.method = "POST";
    rq.host = host;
    rq.path_and_query = path_and_query;
    rq.json_body = json_body;
    rq.auth_header = NULL;
    rq.timeout_ms = timeout_ms;
    return sf_http_request(&rq, resp);
}
