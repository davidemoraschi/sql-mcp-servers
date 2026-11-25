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
    bool more_chunks;             /* result continues in partitions/pages       */
    int  partition_count;         /* total partitions (0 = unknown); v2 only    */
    char statement_handle[64];    /* statementHandle for v2 partition fetching  */
    char next_page_url[512];      /* rel="next" URL for v1 Link pagination      */
    bool is_last_page;            /* true when rel="last" seen in Link header   */
} SfQueryResult;

/* Validate and copy the configuration. No network access. */
bool sf_session_configure(const SfSessionConfig *cfg, char *err, size_t err_size);

/* Run one SQL statement. Release with sf_query_free(). */
SfQueryResult sf_session_query(const char *sql);

/*
 * Fetch one partition (1-based index) of a previously executed statement.
 * The returned SfQueryResult has rowtype=NULL (use the original query's rowtype)
 * and rowset pointing into the new root. Release with sf_query_free().
 */
SfQueryResult sf_session_fetch_partition(const char *statement_handle, int partition);

/* Fetch one session-API chunk URL. The chunk headers belong to the initial result. */
SfQueryResult sf_session_fetch_chunk(const char *url, const JsonValue *chunk_headers);

/*
 * Fetch the next page using a v1 Link header URL (absolute path).
 * rowtype=NULL — reuse the original query's rowtype. Release with sf_query_free().
 */
SfQueryResult sf_session_fetch_page(const char *path_and_query);

void sf_query_free(SfQueryResult *res);

/* Forget the session and scrub the token. */
void sf_session_close(void);

#endif /* SF_QUERY_H */
