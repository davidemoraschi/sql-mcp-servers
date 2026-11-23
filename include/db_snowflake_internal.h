/*
 * db_snowflake_internal.h - Private shared state between db_snowflake_core.c and db_snowflake_transfer.c
 *
 * Not part of the public API. Include only from Snowflake translation units.
 */

#ifndef DB_SNOWFLAKE_INTERNAL_H
#define DB_SNOWFLAKE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include "sf_query.h"
#include "sf_buf.h"
#include "json_rpc.h"
#include "db_sqlite.h"   /* DbQueryResult */

/* Defined in db_snowflake_core.c */
extern bool g_ready;
extern bool g_read_only;

char       *sf_safe_strdup(const char *s);
DbQueryResult sf_fail(const char *msg);
DbQueryResult sf_run(const char *sql);
DbQueryResult sf_format_result(const SfQueryResult *q);

/* Result-set accessors */
void          sf_md_cell(SfBuf *b, const char *s);
const char   *sf_cell_text(const JsonValue *v, char *num, size_t num_size, bool *is_null);
const char   *sf_column_name(const SfQueryResult *q, size_t i);
int           sf_column_index(const SfQueryResult *q, const char *name);
const JsonValue *sf_rowset_cell(const SfQueryResult *q, size_t r, size_t c);
size_t        sf_row_count(const SfQueryResult *q);
size_t        sf_col_count(const SfQueryResult *q);

#endif /* DB_SNOWFLAKE_INTERNAL_H */
