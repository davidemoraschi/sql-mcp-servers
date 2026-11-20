/*
 * db_snowflake.h - Snowflake backend for the shared db_* tool interface
 *
 * db_snowflake.c implements the query functions declared in db_sqlite.h
 * (db_execute_read, db_list_tables, ...), so rdbms_tools.c is reused as is.
 * Only the lifecycle calls below are Snowflake specific.
 *
 * Line budget constraint: < 150 lines
 */

#ifndef DB_SNOWFLAKE_H
#define DB_SNOWFLAKE_H

#include <stdbool.h>
#include <stddef.h>
#include "db_sqlite.h"
#include "sf_query.h"

/*
 * Configure the backend. No network access happens here: the browser SSO
 * sign-in runs on the first query. read_only rejects write_query.
 */
bool sf_db_init(const SfSessionConfig *cfg, bool read_only, char *err, size_t err_size);

void sf_db_close(void);

#endif /* DB_SNOWFLAKE_H */
