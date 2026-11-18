/*
 * rdbms_tools.c - Portable RDBMS MCP Tool Definitions & Handlers
 *
 * Line budget constraint: < 450 lines
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdbms_tools.h"
#include "db_sqlite.h"

void tools_handle_discover(const char *id) {
    const char *discover_json = 
        "{\"protocolVersion\":\"2026-07-28\",\"serverInfo\":{\"name\":\"sqlite-mcp-server\",\"version\":\"1.0.0\"},\"capabilities\":{\"tools\":{\"listChanged\":false}}}";
    json_rpc_send_result(id, discover_json);
}

void tools_handle_list(const char *id) {
    const char *list_json = 
        "{\"ttlMs\":300000,\"cacheScope\":\"public\",\"tools\":["
        "{\"name\":\"read_query\",\"description\":\"Execute read-only SQL SELECT queries\",\"readOnlyHint\":true},"
        "{\"name\":\"write_query\",\"description\":\"Execute SQL modification queries (INSERT/UPDATE/DELETE)\",\"idempotentHint\":false},"
        "{\"name\":\"backup_database\",\"description\":\"Create online database backup\",\"readOnlyHint\":true},"
        "{\"name\":\"explain_query\",\"description\":\"Inspect query execution plan\",\"readOnlyHint\":true},"
        "{\"name\":\"describe_table\",\"description\":\"Describe table schema, columns, types, and keys\",\"readOnlyHint\":true},"
        "{\"name\":\"infer_relationships\",\"description\":\"Infer primary/foreign key relationships across tables\",\"readOnlyHint\":true},"
        "{\"name\":\"generate_erd\",\"description\":\"Generate PlantUML ERD schema diagram\",\"readOnlyHint\":true},"
        "{\"name\":\"list_tables\",\"description\":\"List all database tables and views\",\"readOnlyHint\":true},"
        "{\"name\":\"export_data\",\"description\":\"Export database table contents (CSV/JSON)\",\"readOnlyHint\":true},"
        "{\"name\":\"check_database_health\",\"description\":\"Check database integrity and key constraints\",\"readOnlyHint\":true}"
        "]}";
    json_rpc_send_result(id, list_json);
}

static void send_tool_response(const char *id, DbQueryResult *res) {
    if (!res->success) {
        char err_json[512];
        snprintf(err_json, sizeof(err_json), "{\"error\":\"%s\"}", res->error_message ? res->error_message : "Execution failed");
        json_rpc_send_error(id, JSONRPC_ERR_INTERNAL, "Tool execution failed", err_json);
        db_free_result(res);
        return;
    }

    size_t len = strlen(res->markdown_table) + strlen(res->json_structured) + 512;
    char *out_buf = (char *)malloc(len);
    if (!out_buf) {
        json_rpc_send_error(id, JSONRPC_ERR_INTERNAL, "Out of memory", NULL);
        db_free_result(res);
        return;
    }

    snprintf(out_buf, len,
        "{"
          "\"resultType\":\"complete\","
          "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
          "\"structuredContent\":%s"
        "}",
        res->markdown_table, res->json_structured);

    json_rpc_send_result(id, out_buf);
    free(out_buf);
    db_free_result(res);
}

void tools_handle_call(const char *id, const JsonValue *params) {
    if (!params) {
        json_rpc_send_error(id, JSONRPC_ERR_INVALID_PARAMS, "Missing params", NULL);
        return;
    }

    JsonValue *v_name = json_object_get(params, "name");
    JsonValue *v_args = json_object_get(params, "arguments");

    const char *tool_name = json_string_value(v_name);
    if (!tool_name) {
        json_rpc_send_error(id, JSONRPC_ERR_INVALID_PARAMS, "Missing tool name", NULL);
        return;
    }

    DbQueryResult res;
    memset(&res, 0, sizeof(DbQueryResult));

    if (strcmp(tool_name, "read_query") == 0) {
        JsonValue *v_sql = v_args ? json_object_get(v_args, "sql") : NULL;
        const char *sql = json_string_value(v_sql);
        res = db_execute_read(sql ? sql : "SELECT 1;", NULL);
    } else if (strcmp(tool_name, "write_query") == 0) {
        JsonValue *v_sql = v_args ? json_object_get(v_args, "sql") : NULL;
        const char *sql = json_string_value(v_sql);
        res = db_execute_write(sql ? sql : "UPDATE dummy SET id=1;", NULL);
    } else if (strcmp(tool_name, "backup_database") == 0) {
        JsonValue *v_path = v_args ? json_object_get(v_args, "target_path") : NULL;
        res = db_backup(json_string_value(v_path));
    } else if (strcmp(tool_name, "explain_query") == 0) {
        JsonValue *v_sql = v_args ? json_object_get(v_args, "sql") : NULL;
        res = db_explain(json_string_value(v_sql));
    } else if (strcmp(tool_name, "describe_table") == 0) {
        JsonValue *v_tbl = v_args ? json_object_get(v_args, "table_name") : NULL;
        res = db_describe_table(json_string_value(v_tbl) ? json_string_value(v_tbl) : "users");
    } else if (strcmp(tool_name, "infer_relationships") == 0) {
        res = db_infer_relationships();
    } else if (strcmp(tool_name, "generate_erd") == 0) {
        res = db_generate_erd_plantuml();
    } else if (strcmp(tool_name, "list_tables") == 0) {
        res = db_list_tables();
    } else if (strcmp(tool_name, "export_data") == 0) {
        JsonValue *v_tbl = v_args ? json_object_get(v_args, "table_name") : NULL;
        JsonValue *v_fmt = v_args ? json_object_get(v_args, "format") : NULL;
        res = db_export_data(json_string_value(v_tbl), json_string_value(v_fmt));
    } else if (strcmp(tool_name, "check_database_health") == 0) {
        res = db_check_health();
    } else {
        json_rpc_send_error(id, JSONRPC_ERR_METHOD_NOT_FOUND, "Unknown tool name", NULL);
        return;
    }

    send_tool_response(id, &res);
}
