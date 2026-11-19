/*
 * db_sqlite.h - SQLite Core Engine Wrapper Header
 *
 * Line budget constraint: < 150 lines
 */

#ifndef DB_SQLITE_H
#define DB_SQLITE_H

#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_DEPRECATE
  #define _CRT_NONSTDC_NO_DEPRECATE
#endif

#include <stdbool.h>
#include <stddef.h>



typedef struct {
    char *db_path;
    char *open_mode;
    int busy_timeout_ms;
    char *encryption_key;
} DbConfig;

typedef struct {
    bool success;
    int rows_affected;
    char *markdown_table;
    char *json_structured;
    char *error_message;
} DbQueryResult;

bool db_init(const DbConfig *config);
void db_close(void);
void db_free_result(DbQueryResult *res);
bool db_validate_identifier(const char *identifier);

DbQueryResult db_execute_read(const char *sql_query, const char *params_json);
DbQueryResult db_execute_write(const char *sql_query, const char *params_json);
DbQueryResult db_backup(const char *target_path);
DbQueryResult db_explain(const char *sql_query);
DbQueryResult db_describe_table(const char *table_name);
DbQueryResult db_infer_relationships(void);
DbQueryResult db_generate_erd_plantuml(void);
DbQueryResult db_list_tables(void);
DbQueryResult db_export_data(const char *table_name, const char *format);
DbQueryResult db_check_health(void);

#endif /* DB_SQLITE_H */
