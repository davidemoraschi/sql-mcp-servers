/*
 * db_postgres.h - PostgreSQL backend for the shared db_* tool interface
 *
 * db_postgres.c implements the query functions declared in db_sqlite.h
 * (db_execute_read, db_list_tables, ...) so rdbms_tools.c is reused as-is.
 *
 * Line budget constraint: < 150 lines
 */

#ifndef DB_POSTGRES_H
#define DB_POSTGRES_H

#include <stdbool.h>
#include <stddef.h>
#include "db_sqlite.h"

typedef struct {
    const char *host;       /* hostname or Unix-socket directory      */
    const char *port;       /* TCP port string, default "5432"        */
    const char *dbname;
    const char *user;
    const char *password;   /* cleared from memory after connect      */
    const char *sslmode;    /* "disable" | "prefer" (prefer = try TLS,
                               fall back to plain; disable = plain only) */
    int         connect_timeout_seconds;
    int         query_timeout_seconds;  /* unused placeholder for parity */
    bool        read_only;
} PgConfig;

bool pg_db_init(const PgConfig *cfg, char *err, size_t err_size);
void pg_db_close(void);

#endif /* DB_POSTGRES_H */
