/*
 * sf_query.h - Snowflake session and SQL execution (query-request REST API)
 *
 * The session is created lazily: the first query triggers the browser SSO
 * sign-in (sf_auth), then applies the optional role/warehouse/database/schema.
 * An expired session is renewed transparently with one more sign-in.
 *
 * Line budget constraint: < 150 lines
 */

#ifndef SF_QUERY_H
#define SF_QUERY_H

#include <stdbool.h>
#include "json_rpc.h"

typedef struct {
    const char *account;      /* required: "ORG-ACCOUNT" or "locator[.region]" */
    const char *user;         /* required: Snowflake login name                  */
    const char *role;         /* optional                                        */
    const char *warehouse;    /* optional                                        */
    const char *database;     /* optional                                        */
    const char *schema;       /* optional                                        */
    unsigned short callback_port;   /* SSO loopback port, 0 = OS-chosen          */
    int sso_timeout_seconds;        /* <= 0: 120                                 */
    int query_timeout_seconds;      /* <= 0: 300                                 */
} SfSessionConfig;

typedef struct {
    bool success;
    char *error_message;          /* set when success is false                  */
    JsonValue *root;              /* owns everything below                      */
    const JsonValue *rowtype;     /* array of column descriptors ({"name":..})  */
    const JsonValue *rowset;      /* array of rows; each row is an array        */
    bool more_chunks;             /* result continues in chunks not downloaded  */
} SfQueryResult;

/* Validate and copy the configuration. No network access. */
bool sf_session_configure(const SfSessionConfig *cfg, char *err, size_t err_size);

/* Run one SQL statement. Release with sf_query_free(). */
SfQueryResult sf_session_query(const char *sql);

void sf_query_free(SfQueryResult *res);

/* Forget the session and scrub the token. */
void sf_session_close(void);

#endif /* SF_QUERY_H */
