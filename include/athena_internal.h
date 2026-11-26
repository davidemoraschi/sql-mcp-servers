#ifndef ATHENA_INTERNAL_H
#define ATHENA_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include "db_sqlite.h"
#include "json_rpc.h"
DbQueryResult athena_run(const char *sql);
DbQueryResult athena_fail(const char *message);
const char *athena_database(void);
const char *athena_catalog(void);
bool athena_is_read_only(void);
#endif
