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

static McpProfile g_profile = {
    "sqlite-mcp-server",
    "SQLite",
    "Check database integrity, foreign key consistency, and size",
    true
};

void tools_set_profile(const McpProfile *profile) {
    if (profile) g_profile = *profile;
}

void tools_handle_initialize(const char *id) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"protocolVersion\":\"2025-11-25\",\"serverInfo\":{\"name\":\"%s\",\"version\":\"1.0.0\"},\"capabilities\":{\"tools\":{\"listChanged\":false}}}",
        g_profile.server_name);
    json_rpc_send_result(id, buf);
}

void tools_handle_discover(const char *id) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"protocolVersion\":\"2026-07-28\",\"serverInfo\":{\"name\":\"%s\",\"version\":\"1.0.0\"},\"capabilities\":{\"tools\":{\"listChanged\":false}}}",
        g_profile.server_name);
    json_rpc_send_result(id, buf);
}

void tools_handle_list(const char *id) {
    static const char backup_tool[] =
        "{\"name\":\"backup_database\",\"description\":\"Create an online point-in-time backup of the database\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"target_path\":{\"type\":\"string\",\"description\":\"Destination file path for backup\"}},\"required\":[\"target_path\"]},\"annotations\":{\"readOnlyHint\":true}},";
    char list_json[8192];
    snprintf(list_json, sizeof(list_json),
        "{\"ttlMs\":300000,\"cacheScope\":\"public\",\"tools\":["
        "{\"name\":\"read_query\",\"description\":\"Execute read-only SQL SELECT queries against the %s database\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sql\":{\"type\":\"string\",\"description\":\"SQL SELECT statement to execute\"}},\"required\":[\"sql\"]},\"annotations\":{\"readOnlyHint\":true}},"
        "{\"name\":\"write_query\",\"description\":\"Execute SQL modification queries (INSERT/UPDATE/DELETE/CREATE)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sql\":{\"type\":\"string\",\"description\":\"SQL modification query to execute\"}},\"required\":[\"sql\"]},\"annotations\":{\"idempotentHint\":false}},"
        "%s"
        "{\"name\":\"explain_query\",\"description\":\"Inspect the query execution plan for a SQL query\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sql\":{\"type\":\"string\",\"description\":\"SQL statement to evaluate\"}},\"required\":[\"sql\"]},\"annotations\":{\"readOnlyHint\":true}},"
        "{\"name\":\"describe_table\",\"description\":\"Describe table schema, columns, data types, nullability, and primary keys\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"table_name\":{\"type\":\"string\",\"description\":\"Target relational table name\"}},\"required\":[\"table_name\"]},\"annotations\":{\"readOnlyHint\":true}},"
        "{\"name\":\"infer_relationships\",\"description\":\"Infer primary and foreign key relationships across database tables\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false},\"annotations\":{\"readOnlyHint\":true}},"
        "{\"name\":\"generate_erd\",\"description\":\"Generate PlantUML ERD schema diagram of the database\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false},\"annotations\":{\"readOnlyHint\":true}},"
        "{\"name\":\"list_tables\",\"description\":\"List all database tables, views, and row counts\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false},\"annotations\":{\"readOnlyHint\":true}},"
        "{\"name\":\"export_data\",\"description\":\"Export database table contents to CSV or JSON format\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"table_name\":{\"type\":\"string\",\"description\":\"Table name to export\"},\"format\":{\"type\":\"string\",\"enum\":[\"csv\",\"json\"],\"description\":\"Export format\"}},\"required\":[\"table_name\",\"format\"]},\"annotations\":{\"readOnlyHint\":true}},"
        "{\"name\":\"check_database_health\",\"description\":\"%s\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false},\"annotations\":{\"readOnlyHint\":true}}"
        "]}",
        g_profile.db_label, g_profile.has_backup ? backup_tool : "", g_profile.health_desc);
    json_rpc_send_result(id, list_json);
}

static char *json_escape_string(const char *input) {
    if (!input) {
        char *empty = (char *)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    size_t len = strlen(input);
    size_t esc_len = 0;
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '"' || input[i] == '\\') esc_len += 2;
        else if (input[i] == '\n') esc_len += 2;
        else if (input[i] == '\r') esc_len += 2;
        else if (input[i] == '\t') esc_len += 2;
        else esc_len += 1;
    }
    char *out = (char *)malloc(esc_len + 1);
    if (!out) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '"') { out[pos++] = '\\'; out[pos++] = '"'; }
        else if (input[i] == '\\') { out[pos++] = '\\'; out[pos++] = '\\'; }
        else if (input[i] == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if (input[i] == '\r') { out[pos++] = '\\'; out[pos++] = 'r'; }
        else if (input[i] == '\t') { out[pos++] = '\\'; out[pos++] = 't'; }
        else { out[pos++] = input[i]; }
    }
    out[pos] = '\0';
    return out;
}

static void send_tool_response(const char *id, DbQueryResult *res) {
    if (!res->success) {
        char err_json[512];
        snprintf(err_json, sizeof(err_json), "{\"error\":\"%s\"}", res->error_message ? res->error_message : "Execution failed");
        json_rpc_send_error(id, JSONRPC_ERR_INTERNAL, "Tool execution failed", err_json);
        db_free_result(res);
        return;
    }

    char *esc_md = json_escape_string(res->markdown_table);
    const char *struct_raw = (res->json_structured && res->json_structured[0]) ? res->json_structured : "[]";

    /* Guarantee structuredContent is a JSON Object (Record) per MCP 2026-07-28 spec */
    char *rows_obj = NULL;
    if (struct_raw[0] == '[') {
        size_t rlen = strlen(struct_raw) + 32;
        rows_obj = (char *)malloc(rlen);
        if (rows_obj) {
            snprintf(rows_obj, rlen, "{\"rows\":%s}", struct_raw);
        }
    }

    const char *struct_json = rows_obj ? rows_obj : (struct_raw[0] == '{' ? struct_raw : "{}");

    size_t len = (esc_md ? strlen(esc_md) : 0) + strlen(struct_json) + 512;
    char *out_buf = (char *)malloc(len);
    if (!out_buf) {
        if (esc_md) free(esc_md);
        if (rows_obj) free(rows_obj);
        json_rpc_send_error(id, JSONRPC_ERR_INTERNAL, "Out of memory", NULL);
        db_free_result(res);
        return;
    }

    snprintf(out_buf, len,
        "{"
          "\"resultType\":\"complete\","
          "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
          "\"structuredContent\":{\"rows\":%s}"
        "}",
        esc_md ? esc_md : "", struct_json);

    json_rpc_send_result(id, out_buf);
    if (esc_md) free(esc_md);
    if (rows_obj) free(rows_obj);
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
    } else if (strcmp(tool_name, "backup_database") == 0 && g_profile.has_backup) {
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
