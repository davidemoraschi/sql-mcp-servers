/*
 * db_postgres_internal.h - Private shared state between db_postgres.c and db_postgres_transfer.c
 *
 * Not part of the public API. Include only from PostgreSQL translation units.
 */

#ifndef DB_POSTGRES_INTERNAL_H
#define DB_POSTGRES_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include "db_sqlite.h"
#include "sf_buf.h"

typedef struct {
    char  **col_names;
    int     col_count;
    char ***rows;
    int     row_count;
    int     row_cap;
    char   *error;
    bool    success;
} PgResult;

extern bool g_read_only;

char        *pg_strdup(const char *s);
DbQueryResult pg_fail(const char *msg);
PgResult      pg_exec(const char *sql);
void          pg_result_free(PgResult *r);
DbQueryResult pg_run(const char *sql);

#endif /* DB_POSTGRES_INTERNAL_H */
