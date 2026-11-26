/* BigQuery backend lifecycle; query functions implement db_sqlite.h. */
#ifndef DB_BIGQUERY_H
#define DB_BIGQUERY_H

#include <stdbool.h>
#include <stddef.h>
#include "db_sqlite.h"

typedef struct {
    const char *host;
    const char *project_id;
    const char *dataset;
    const char *location;
    const char *oauth_client_id;
    const char *oauth_client_secret;
    const char *oauth_refresh_token;
    bool read_only;
    int timeout_seconds;
} BqConfig;

bool bq_db_init(const BqConfig *cfg, char *err, size_t err_size);
void bq_db_close(void);

#endif
