/*
 * sf_loopback.c - One-shot 127.0.0.1 HTTP listener (Winsock / BSD sockets)
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
#include "sf_loopback.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
  #endif
  typedef SOCKET sf_sock;
  #define SF_BAD_SOCK INVALID_SOCKET
  #define sf_sock_close closesocket
  typedef int sf_socklen;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <time.h>
  typedef int sf_sock;
  #define SF_BAD_SOCK (-1)
  #define sf_sock_close close
  typedef socklen_t sf_socklen;
#endif

#define SF_REQ_MAX 65536
#define SF_READ_SLICE_MS 5000

struct SfLoopback {
    sf_sock listen_sock;
    unsigned short port;
};

static const char SF_PAGE[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>"
    "Authentication complete</title></head><body style=\"font-family:sans-serif\">"
    "<h3>Authentication complete</h3><p>You may close this tab and return to the "
    "application.</p></body></html>";

static long long sf_now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
#endif
}

static char *sf_lb_dup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* Wait until `s` is readable or `ms` elapses. 1 = readable, 0 = timeout, -1 = error. */
static int sf_wait_readable(sf_sock s, long long ms) {
    fd_set rs;
    struct timeval tv;
    int rc;
    if (ms < 0) ms = 0;
    FD_ZERO(&rs);
    FD_SET(s, &rs);
    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)((ms % 1000) * 1000);
    rc = select((int)s + 1, &rs, NULL, NULL, &tv);
    if (rc > 0) return 1;
    return rc == 0 ? 0 : -1;
}

SfLoopback *sf_loopback_start(unsigned short port) {
    SfLoopback *lb;
    struct sockaddr_in addr;
    sf_socklen alen = (sf_socklen)sizeof(addr);
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
#endif
    lb = (SfLoopback *)calloc(1, sizeof(*lb));
    if (!lb) {
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    lb->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lb->listen_sock == SF_BAD_SOCK) goto fail;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* loopback only, never exposed */
    if (bind(lb->listen_sock, (struct sockaddr *)&addr, (sf_socklen)sizeof(addr)) != 0) goto fail;
    if (listen(lb->listen_sock, 4) != 0) goto fail;

    lb->port = port;
    memset(&addr, 0, sizeof(addr));
    if (getsockname(lb->listen_sock, (struct sockaddr *)&addr, &alen) == 0) {
        lb->port = ntohs(addr.sin_port);
    }
    return lb;

fail:
    sf_loopback_stop(lb);
    return NULL;
}

unsigned short sf_loopback_port(const SfLoopback *lb) {
    return lb ? lb->port : 0;
}

void sf_loopback_stop(SfLoopback *lb) {
    if (!lb) return;
    if (lb->listen_sock != SF_BAD_SOCK) sf_sock_close(lb->listen_sock);
    free(lb);
#ifdef _WIN32
    WSACleanup();
#endif
}

void sf_loopback_free_callback(SfCallback *cb) {
    if (!cb) return;
    free(cb->query);
    free(cb->body);
    cb->query = NULL;
    cb->body = NULL;
}

/*
 * Read one HTTP request (headers + Content-Length body, capped) into a
 * malloc'd NUL-terminated buffer. Gives up when `deadline` passes.
 */
static char *sf_read_request(sf_sock c, long long deadline) {
    size_t len = 0;
    size_t need_total = 0;       /* known once headers are complete */
    char *buf = (char *)malloc(SF_REQ_MAX + 1);
    if (!buf) return NULL;
    for (;;) {
        long long left = deadline - sf_now_ms();
        int r, n;
        if (left <= 0 || len >= SF_REQ_MAX) break;
        if (left > SF_READ_SLICE_MS) left = SF_READ_SLICE_MS;
        r = sf_wait_readable(c, left);
        if (r <= 0) break;
        n = (int)recv(c, buf + len, (int)(SF_REQ_MAX - len), 0);
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = '\0';
        if (need_total == 0) {
            char *hdr_end = strstr(buf, "\r\n\r\n");
            if (hdr_end) {
                size_t head = (size_t)(hdr_end - buf) + 4;
                size_t clen = 0;
                char *cl = strstr(buf, "Content-Length:");
                if (!cl) cl = strstr(buf, "content-length:");
                if (cl && cl < hdr_end) clen = (size_t)strtoul(cl + 15, NULL, 10);
                if (clen > SF_REQ_MAX - head) clen = SF_REQ_MAX - head;
                need_total = head + clen;
            }
        }
        if (need_total != 0 && len >= need_total) break;
    }
    buf[len] = '\0';
    return buf;
}

static void sf_send_page(sf_sock c) {
    char head[160];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: %u\r\nConnection: close\r\n\r\n",
                      (unsigned)(sizeof(SF_PAGE) - 1));
    send(c, head, hl, 0);
    send(c, SF_PAGE, (int)(sizeof(SF_PAGE) - 1), 0);
}

/* Extract the query ("a=b&c=d") of the request target in "GET /p?query HTTP/1.1". */
static char *sf_request_query(const char *req) {
    const char *sp = strchr(req, ' ');
    const char *end, *q;
    if (!sp) return sf_lb_dup("", 0);
    sp++;
    end = strchr(sp, ' ');
    if (!end) return sf_lb_dup("", 0);
    q = (const char *)memchr(sp, '?', (size_t)(end - sp));
    if (!q) return sf_lb_dup("", 0);
    return sf_lb_dup(q + 1, (size_t)(end - q - 1));
}

SfCallback sf_loopback_wait(SfLoopback *lb, int timeout_ms) {
    SfCallback out;
    long long deadline = sf_now_ms() + timeout_ms;
    memset(&out, 0, sizeof(out));
    if (!lb || lb->listen_sock == SF_BAD_SOCK) return out;

    for (;;) {
        sf_sock c;
        char *req, *query, *body;
        const char *sep;
        int r = sf_wait_readable(lb->listen_sock, deadline - sf_now_ms());
        if (r == 0) { out.timed_out = true; return out; }
        if (r < 0) return out;
        c = accept(lb->listen_sock, NULL, NULL);
        if (c == SF_BAD_SOCK) return out;

        req = sf_read_request(c, deadline);
        sf_send_page(c);
        sf_sock_close(c);
        if (!req) return out;

        query = sf_request_query(req);
        sep = strstr(req, "\r\n\r\n");
        body = sf_lb_dup(sep ? sep + 4 : "", sep ? strlen(sep + 4) : 0);
        if (!query || !body) { free(req); free(query); free(body); return out; }
        free(req);

        /* Skip empty probes (favicon, preconnect) and keep listening. */
        if (query[0] == '\0' && body[0] == '\0') {
            free(query);
            free(body);
            continue;
        }
        out.received = true;
        out.query = query;
        out.body = body;
        return out;
    }
}
