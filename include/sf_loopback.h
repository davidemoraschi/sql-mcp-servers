/*
 * sf_loopback.h - One-shot 127.0.0.1 HTTP listener and browser launcher
 *
 * Captures the redirect of the Snowflake external-browser (SSO) flow.
 * Line budget constraint: < 150 lines
 */

#ifndef SF_LOOPBACK_H
#define SF_LOOPBACK_H

#include <stdbool.h>

typedef struct SfLoopback SfLoopback;

typedef struct {
    bool received;     /* a request carrying a query or body arrived  */
    bool timed_out;    /* the wait expired                            */
    char *query;       /* raw (percent-encoded) query string, or ""   */
    char *body;        /* raw request body, or ""                     */
} SfCallback;

/* Bind and listen on 127.0.0.1. port 0 = OS-chosen. NULL on failure. */
SfLoopback *sf_loopback_start(unsigned short port);

/* Actual bound port. */
unsigned short sf_loopback_port(const SfLoopback *lb);

/*
 * Wait for the browser redirect. Empty probe requests (no query, no body)
 * are answered and skipped. Release with sf_loopback_free_callback().
 */
SfCallback sf_loopback_wait(SfLoopback *lb, int timeout_ms);

void sf_loopback_free_callback(SfCallback *cb);

/* Close the socket and free the server. Safe with NULL. */
void sf_loopback_stop(SfLoopback *lb);

/*
 * Open an https:// URL in the default browser. Anything else is refused.
 * Never writes to stdout (it carries the MCP JSON-RPC stream).
 */
bool sf_browser_open(const char *url);

#endif /* SF_LOOPBACK_H */
