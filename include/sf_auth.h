/*
 * sf_auth.h - Snowflake external-browser (SSO) authentication
 *
 * C11 port of the kiro-course snowflake-auth ExternalBrowserCredential.
 * Flow: authenticator-request -> browser sign-in -> loopback redirect ->
 * login-request with TOKEN + PROOF_KEY -> session token.
 *
 * Line budget constraint: < 150 lines
 */

#ifndef SF_AUTH_H
#define SF_AUTH_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *account;          /* "ORG-ACCOUNT" or "locator[.region]" (required) */
    const char *user;             /* Snowflake login name (required)                */
    unsigned short callback_port; /* loopback port, 0 = OS-chosen                   */
    int timeout_seconds;          /* browser sign-in wait, <= 0 means 120           */
    bool skip_browser_launch;     /* true: only print the URL, do not open it       */
} SfAuthConfig;

typedef struct {
    bool success;
    char *session_token;   /* data.token                                     */
    char *master_token;    /* data.masterToken (may be NULL)                 */
    char *error_message;   /* set when success is false                      */
} SfAuthResult;

/*
 * Run the external-browser flow. Blocks until sign-in completes, fails or
 * times out. The SSO URL is printed to stderr (stdout belongs to JSON-RPC).
 * Release with sf_auth_free_result(), which scrubs the tokens.
 */
SfAuthResult sf_auth_external_browser(const SfAuthConfig *cfg);

void sf_auth_free_result(SfAuthResult *res);

/* --- Helpers exposed for unit testing --- */

/* ACCOUNT_NAME for the request body: drop ".region", drop "ORG-", uppercase. */
bool sf_auth_account_name(const char *account, char *out, size_t out_size);

/* Random RFC 4122-style request id (36 chars + NUL) for ?requestId=. */
void sf_auth_request_id(char out[37]);

/* Value of `name` in a raw query string, percent-decoded. malloc'd or NULL. */
char *sf_auth_query_value(const char *query, const char *name);

#endif /* SF_AUTH_H */
