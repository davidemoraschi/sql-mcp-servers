#ifndef DB_ATHENA_H
#define DB_ATHENA_H
#include <stdbool.h>
#include <stddef.h>
#include "db_sqlite.h"

typedef struct {
    const char *region;
    const char *database;
    const char *catalog;
    const char *workgroup;
    const char *output_location;
    const char *profile;
    bool read_only;
    int timeout_seconds;
} AthenaConfig;

bool athena_db_init(const AthenaConfig *cfg, char *err, size_t err_size);
void athena_db_close(void);
#endif
