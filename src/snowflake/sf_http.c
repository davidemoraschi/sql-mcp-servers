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

#define SF_HTTP_MAX_RESPONSE (64u * 1024u * 1024u)

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
    free(resp->link_header);
    resp->body = NULL;
    resp->error = NULL;
    resp->link_header = NULL;
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
    {
        /* Capture Link header for v1 pagination */
        wchar_t lbuf[2048]; DWORD lsz = sizeof(lbuf);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Link",
                                lbuf, &lsz, WINHTTP_NO_HEADER_INDEX)) {
            int n = WideCharToMultiByte(CP_UTF8, 0, lbuf, -1, NULL, 0, NULL, NULL);
            if (n > 0) {
                char *mb = (char *)malloc((size_t)n);
                if (mb && WideCharToMultiByte(CP_UTF8, 0, lbuf, -1, mb, n, NULL, NULL) > 0)
                    resp->link_header = mb;
                else free(mb);
            }
        }
    }
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

static size_t sf_curl_header(char *buf, size_t size, size_t nitems, void *ud) {
    SfCurlBuf *b = (SfCurlBuf *)ud;
    size_t n = size * nitems;
    /* Look for "Link:" header (case-insensitive prefix) */
    if (n > 5 && strncasecmp(buf, "link:", 5) == 0) {
        const char *val = buf + 5;
        while (*val == ' ') val++;
        size_t vlen = n - (size_t)(val - buf);
        while (vlen > 0 && (val[vlen-1] == '\r' || val[vlen-1] == '\n')) vlen--;
        free(b->data);
        b->data = (char *)malloc(vlen + 1);
        if (b->data) { memcpy(b->data, val, vlen); b->data[vlen] = '\0'; b->len = vlen; }
    }
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
    SfCurlBuf lnk = {NULL, 0, 0}; /* Link header capture */
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
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, sf_curl_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &lnk);

    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp->status = (int)code;
        resp->body = buf.data ? buf.data : sf_http_dup("");
        resp->body_len = buf.len;
        resp->link_header = lnk.data; lnk.data = NULL;
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
    free(lnk.data);
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

bool sf_http_get_url(const char *url, const char *extra_headers, bool decompress,
                     int timeout_ms, SfHttpResponse *resp) {
#ifdef _WIN32
    URL_COMPONENTS parts;
    wchar_t *wurl = NULL, *wheaders = NULL;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    wchar_t host[512], path[4096];
    DWORD status = 0, status_sz = sizeof(status);
    bool ok = false;

    memset(resp, 0, sizeof(*resp));
    memset(&parts, 0, sizeof(parts));
    memset(host, 0, sizeof(host)); memset(path, 0, sizeof(path));
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host; parts.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    wurl = sf_to_wide(url);
    wheaders = sf_to_wide(extra_headers ? extra_headers : "");
    if (!wurl || !wheaders || !WinHttpCrackUrl(wurl, 0, 0, &parts)) {
        resp->error = sf_http_dup("Invalid chunk URL"); goto done;
    }
    /* Keep only the host/port from WinHttpCrackUrl; retain the complete request
       target, including the signed query string, from the original URL. */
    {
        const wchar_t *slash = wcschr(wurl, L'/');
        slash = slash ? wcschr(slash + 2, L'/') : NULL;
        if (!slash) slash = L"/";
        wcsncpy(path, slash, (sizeof(path) / sizeof(path[0])) - 1);
    }
    ses = WinHttpOpen(L"snowflake-mcp-server/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { sf_win_error(resp, "WinHttpOpen failed"); goto done; }
    WinHttpSetTimeouts(ses, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    con = WinHttpConnect(ses, host, parts.nPort, 0);
    if (!con) { sf_win_error(resp, "WinHttpConnect failed"); goto done; }
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { sf_win_error(resp, "WinHttpOpenRequest failed"); goto done; }
    if (decompress) {
        DWORD flags = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &flags, sizeof(flags));
    }
    if (!WinHttpSendRequest(req, wheaders, (DWORD)-1L, NULL, 0, 0, 0)) {
        sf_win_error(resp, "WinHttpSendRequest failed"); goto done;
    }
    if (!WinHttpReceiveResponse(req, NULL)) { sf_win_error(resp, "WinHttpReceiveResponse failed"); goto done; }
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX)) {
        sf_win_error(resp, "Reading HTTP status failed"); goto done;
    }
    resp->status = (int)status;
    if (!sf_win_read_body(req, resp)) { sf_win_error(resp, "Reading response body failed"); goto done; }
    ok = true;
done:
    if (req) WinHttpCloseHandle(req); if (con) WinHttpCloseHandle(con); if (ses) WinHttpCloseHandle(ses);
    free(wurl); free(wheaders);
    return ok;
#else
    static int curl_ready = 0;
    CURL *curl = NULL; struct curl_slist *hdrs = NULL; SfCurlBuf body = {NULL, 0, 0};
    char errbuf[CURL_ERROR_SIZE]; CURLcode rc; long code = 0;
    const char *p;
    memset(resp, 0, sizeof(*resp)); errbuf[0] = '\0';
    if (!curl_ready && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        resp->error = sf_http_dup("curl_global_init failed"); return false;
    }
    curl_ready = 1; curl = curl_easy_init();
    if (!curl) { resp->error = sf_http_dup("Out of memory"); return false; }
    for (p = extra_headers ? extra_headers : ""; *p;) {
        const char *e = strstr(p, "\r\n"); size_t n = e ? (size_t)(e - p) : strlen(p);
        char *h = (char *)malloc(n + 1);
        if (!h) break;
        memcpy(h, p, n); h[n] = '\0'; if (n) hdrs = curl_slist_append(hdrs, h); free(h);
        p = e ? e + 2 : p + n;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url); curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs); curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms); curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sf_curl_write); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    if (decompress) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) { curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code); resp->status = (int)code; resp->body = body.data ? body.data : sf_http_dup(""); resp->body_len = body.len; }
    else { char msg[CURL_ERROR_SIZE + 64]; snprintf(msg, sizeof(msg), "HTTPS request failed: %s", errbuf[0] ? errbuf : curl_easy_strerror(rc)); resp->error = sf_http_dup(msg); free(body.data); }
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl); return rc == CURLE_OK;
#endif
}
