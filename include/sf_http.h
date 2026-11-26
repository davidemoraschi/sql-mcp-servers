/*
 * sf_http.h - Minimal blocking HTTPS JSON POST client (Snowflake auth)
 *
 * Windows: WinHTTP.  Other platforms: libcurl.
 * Line budget constraint: < 150 lines
 */

#ifndef SF_HTTP_H
#define SF_HTTP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int status;        /* HTTP status code, 0 if no response was received */
    char *body;        /* NUL-terminated response body (may be NULL)       */
    size_t body_len;
    char *error;       /* transport error message, NULL on success         */
    char *link_header; /* value of the Link response header, or NULL       */
} SfHttpResponse;

typedef struct {
    const char *method;       /* "GET" or "POST"                              */
    const char *host;
    const char *path_and_query;
    const char *json_body;    /* POST only, may be NULL                       */
    const char *content_type; /* request body type; defaults to application/json */
    const char *auth_header;  /* full header line without CRLF, or NULL       */
    int timeout_ms;
} SfHttpRequest;

/*
 * Send a request to https://<host><path_and_query> (port 443).
 * TLS certificate validation is always on; redirects are not followed.
 * Returns true if an HTTP response was received (check resp->status);
 * false on transport failure (resp->error is set).
 * Release with sf_http_free_response().
 */
bool sf_http_request(const SfHttpRequest *req, SfHttpResponse *resp);

/* Convenience wrapper: unauthenticated JSON POST. */
bool sf_http_post_json(const char *host, const char *path_and_query,
                       const char *json_body, int timeout_ms,
                       SfHttpResponse *resp);

/* Fetch an absolute HTTPS URL. extra_headers is a CRLF-delimited header block. */
bool sf_http_get_url(const char *url, const char *extra_headers, bool decompress,
                     int timeout_ms, SfHttpResponse *resp);

void sf_http_free_response(SfHttpResponse *resp);

#endif /* SF_HTTP_H */
