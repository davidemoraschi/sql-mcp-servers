/*
 * rdbms_tools.h - Portable RDBMS MCP Tool Headers
 *
 * Line budget constraint: < 150 lines
 */

#ifndef RDBMS_TOOLS_H
#define RDBMS_TOOLS_H

#include "json_rpc.h"

/* Backend-specific wording and capabilities. Defaults to the SQLite profile. */
typedef struct {
    const char *server_name;   /* serverInfo.name                     */
    const char *db_label;      /* e.g. "SQLite", "Snowflake"          */
    const char *health_desc;   /* check_database_health description   */
    bool has_backup;           /* advertise/accept backup_database    */
} McpProfile;

void tools_set_profile(const McpProfile *profile);

void tools_handle_initialize(const char *id);
void tools_handle_discover(const char *id);
void tools_handle_list(const char *id);
void tools_handle_call(const char *id, const JsonValue *params);

#endif /* RDBMS_TOOLS_H */
