#ifndef BIGQUERY_INTERNAL_H
#define BIGQUERY_INTERNAL_H

#include "db_sqlite.h"
#include "json_rpc.h"

DbQueryResult bq_run(const char *sql);
DbQueryResult bq_fail_external(const char *message);
bool bq_table_parts(const char *name, char *project, size_t project_size,
                    char *dataset, size_t dataset_size, char *table, size_t table_size);
JsonValue *bq_api_json(const char *method, const char *path, const char *body,
                       char *err, size_t err_size);
bool bq_is_read_only(void);
const char *bq_project_id(void);
const char *bq_location(void);

#endif
