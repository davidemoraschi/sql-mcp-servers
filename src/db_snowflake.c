/*
 * db_snowflake.c - Snowflake implementation of the shared db_* tool interface
 *
 * Line budget constraint: < 450 lines
 */
#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "db_snowflake.h"
#include "sf_buf.h"

#define SF_MAX_RESULT_ROWS 1000

static bool g_ready = false;
static bool g_read_only = false;

static char *safe_strdup(const char *s) {
    size_t len;
    char *dup;
    if (!s) return NULL;
    len = strlen(s) + 1;
    dup = (char *)malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

static DbQueryResult fail(const char *msg) {
    DbQueryResult res;
    memset(&res, 0, sizeof(res));
    res.error_message = safe_strdup(msg);
    return res;
}

void db_free_result(DbQueryResult *res) {
    if (!res) return;
    free(res->markdown_table);
    free(res->json_structured);
    free(res->error_message);
    memset(res, 0, sizeof(DbQueryResult));
}

/* ---------- lifecycle ---------- */

bool sf_db_init(const SfSessionConfig *cfg, bool read_only, char *err, size_t err_size) {
    if (!sf_session_configure(cfg, err, err_size)) return false;
    g_read_only = read_only;
    g_ready = true;
    return true;
}

void sf_db_close(void) {
    sf_session_close();
    g_ready = false;
}

/* ---------- identifier and statement checks ---------- */

/* One to three dot-separated unquoted identifiers: [A-Za-z_][A-Za-z0-9_$]* */
bool db_validate_identifier(const char *identifier) {
    int parts = 0;
    const char *p = identifier;
    if (!p || !*p) return false;
    for (;;) {
        if (!isalpha((unsigned char)*p) && *p != '_') return false;
        for (p++; isalnum((unsigned char)*p) || *p == '_' || *p == '$'; p++) {}
        parts++;
        if (*p == '\0') return parts <= 3;
        if (*p != '.' || parts == 3) return false;
        p++;
    }
}

/* Skip whitespace, '(' and SQL comments. */
static const char *skip_noise(const char *p) {
    for (;;) {
        while (isspace((unsigned char)*p) || *p == '(') p++;
        if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
        } else if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
        } else {
            return p;
        }
    }
}

/* True when `sql` is one statement starting with a read-only keyword. */
static bool is_read_only_sql(const char *sql) {
    static const char *const allowed[] = {
        "SELECT", "WITH", "SHOW", "DESC", "DESCRIBE", "EXPLAIN", "LIST", "VALUES"
    };
    const char *p = skip_noise(sql);
    size_t i, n = 0;
    char quote = 0;
    bool ok = false;
    while (isalpha((unsigned char)p[n])) n++;
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strlen(allowed[i]) == n) {
            size_t k;
            for (k = 0; k < n && toupper((unsigned char)p[k]) == allowed[i][k]; k++) {}
            if (k == n) ok = true;
        }
    }
    if (!ok) return false;
    for (; *p; p++) {                       /* reject a second statement */
        if (quote) { if (*p == quote) quote = 0; continue; }
        if (*p == '\'' || *p == '"') { quote = *p; continue; }
        if (*p == ';') return *skip_noise(p + 1) == '\0';
    }
    return true;
}

/* ---------- result formatting ---------- */

static void md_cell(SfBuf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '|') sf_buf_str(b, "\\|");
        else if (c < 0x20) sf_buf_add(b, " ", 1);
        else sf_buf_add(b, (const char *)&c, 1);
    }
}

/* Text of a rowset cell; *is_null is set for SQL NULL. Buffer holds numbers. */
static const char *cell_text(const JsonValue *v, char *num, size_t num_size, bool *is_null) {
    *is_null = false;
    if (!v || v->type == JSON_NULL) { *is_null = true; return "NULL"; }
    if (v->type == JSON_STRING) return v->u.string ? v->u.string : "";
    if (v->type == JSON_BOOL) return v->u.boolean ? "true" : "false";
    if (v->type == JSON_NUMBER) { snprintf(num, num_size, "%.15g", v->u.number); return num; }
    return "";
}

static const char *column_name(const SfQueryResult *q, size_t i) {
    const JsonValue *col = (q->rowtype && i < q->rowtype->u.array.count) ? q->rowtype->u.array.items[i] : NULL;
    const char *n = col ? json_string_value(json_object_get(col, "name")) : NULL;
    return n ? n : "col";
}

/* Index of a rowtype column by case-insensitive name, or -1. */
static int column_index(const SfQueryResult *q, const char *name) {
    size_t i, n = q->rowtype ? q->rowtype->u.array.count : 0;
    for (i = 0; i < n; i++) {
        const char *c = column_name(q, i);
        size_t k = 0;
        while (c[k] && name[k] && tolower((unsigned char)c[k]) == tolower((unsigned char)name[k])) k++;
        if (!c[k] && !name[k]) return (int)i;
    }
    return -1;
}

static const JsonValue *rowset_cell(const SfQueryResult *q, size_t r, size_t c) {
    const JsonValue *row = q->rowset->u.array.items[r];
    if (!row || row->type != JSON_ARRAY || c >= row->u.array.count) return NULL;
    return row->u.array.items[c];
}

static size_t row_count(const SfQueryResult *q) {
    return (q->rowset && q->rowset->type == JSON_ARRAY) ? q->rowset->u.array.count : 0;
}

static size_t col_count(const SfQueryResult *q) {
    return (q->rowtype && q->rowtype->type == JSON_ARRAY) ? q->rowtype->u.array.count : 0;
}

static DbQueryResult format_result(const SfQueryResult *q) {
    DbQueryResult res;
    SfBuf md = {NULL, 0, 0, false}, js = {NULL, 0, 0, false};
    size_t cols = col_count(q), rows = row_count(q), shown = rows, r, c;
    char num[40];
    memset(&res, 0, sizeof(res));
    if (shown > SF_MAX_RESULT_ROWS) shown = SF_MAX_RESULT_ROWS;

    sf_buf_str(&md, "|");
    for (c = 0; c < cols; c++) { sf_buf_str(&md, " "); md_cell(&md, column_name(q, c)); sf_buf_str(&md, " |"); }
    sf_buf_str(&md, "\n|");
    for (c = 0; c < cols; c++) sf_buf_str(&md, "---|");

    sf_buf_str(&js, "[");
    for (r = 0; r < shown; r++) {
        sf_buf_str(&md, "\n|");
        sf_buf_str(&js, r ? ",{" : "{");
        for (c = 0; c < cols; c++) {
            bool is_null;
            const char *t = cell_text(rowset_cell(q, r, c), num, sizeof(num), &is_null);
            sf_buf_str(&md, " ");
            md_cell(&md, t);
            sf_buf_str(&md, " |");
            sf_buf_str(&js, c ? ",\"" : "\"");
            sf_buf_json_escaped(&js, column_name(q, c));
            if (is_null) {
                sf_buf_str(&js, "\":null");
            } else {
                sf_buf_str(&js, "\":\"");
                sf_buf_json_escaped(&js, t);
                sf_buf_str(&js, "\"");
            }
        }
        sf_buf_str(&js, "}");
    }
    sf_buf_str(&js, "]");
    if (rows > shown || q->more_chunks) {
        sf_buf_printf(&md, "\n\n(Result truncated: showing %lu of %lu%s rows.)",
                      (unsigned long)shown, (unsigned long)rows, q->more_chunks ? "+" : "");
    }
    res.markdown_table = sf_buf_take(&md);
    res.json_structured = sf_buf_take(&js);
    if (!res.markdown_table || !res.json_structured) {
        free(res.markdown_table);
        free(res.json_structured);
        res.markdown_table = res.json_structured = NULL;
        res.error_message = safe_strdup("Out of memory");
        return res;
    }
    res.success = true;
    res.rows_affected = (int)shown;
    return res;
}

static DbQueryResult run(const char *sql) {
    DbQueryResult res;
    SfQueryResult q;
    if (!g_ready) return fail("Snowflake connection not configured");
    q = sf_session_query(sql);
    if (!q.success) {
        res = fail(q.error_message ? q.error_message : "Query failed");
    } else {
        res = format_result(&q);
    }
    sf_query_free(&q);
    return res;
}

/* ---------- db_* interface ---------- */

DbQueryResult db_execute_read(const char *sql_query, const char *params_json) {
    (void)params_json;
    if (!sql_query || !*sql_query) return fail("SQL query cannot be null or empty");
    if (!is_read_only_sql(sql_query)) {
        return fail("read_query accepts a single SELECT, WITH, SHOW, DESCRIBE, EXPLAIN, LIST or VALUES statement; use write_query for other statements");
    }
    return run(sql_query);
}

DbQueryResult db_execute_write(const char *sql_query, const char *params_json) {
    (void)params_json;
    if (!sql_query || !*sql_query) return fail("SQL query cannot be null or empty");
    if (g_read_only) return fail("Server is in READONLY mode (SNOWFLAKE_ACCESS_MODE); write queries are disabled");
    return run(sql_query);
}

DbQueryResult db_backup(const char *target_path) {
    (void)target_path;
    return fail("backup_database is not supported for Snowflake");
}

DbQueryResult db_explain(const char *sql_query) {
    SfBuf b = {NULL, 0, 0, false};
    DbQueryResult res;
    char *sql;
    if (!sql_query || !*sql_query) return fail("SQL query cannot be null or empty");
    sf_buf_str(&b, "EXPLAIN USING TEXT ");
    sf_buf_str(&b, sql_query);
    sql = sf_buf_take(&b);
    if (!sql) return fail("Out of memory");
    res = run(sql);
    free(sql);
    return res;
}

DbQueryResult db_describe_table(const char *table_name) {
    char sql[320];
    if (!table_name || !db_validate_identifier(table_name) || strlen(table_name) > 256) {
        return fail("Invalid table identifier");
    }
    snprintf(sql, sizeof(sql), "DESCRIBE TABLE %s", table_name);
    return run(sql);
}

DbQueryResult db_infer_relationships(void) {
    /* Snowflake keys are informational; only declared foreign keys appear here. */
    return run("SHOW IMPORTED KEYS IN SCHEMA");
}

DbQueryResult db_list_tables(void) {
    return run("SELECT table_name, table_type AS type, row_count, bytes "
               "FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() "
               "ORDER BY table_name");
}

DbQueryResult db_export_data(const char *table_name, const char *format) {
    char sql[320];
    (void)format;   /* same as the SQLite backend: rows are returned in the tool result */
    if (!table_name || !db_validate_identifier(table_name) || strlen(table_name) > 256) {
        return fail("Invalid table identifier for export");
    }
    snprintf(sql, sizeof(sql), "SELECT * FROM %s LIMIT %d", table_name, SF_MAX_RESULT_ROWS);
    return run(sql);
}

DbQueryResult db_check_health(void) {
    return run("SELECT CURRENT_ACCOUNT() AS account, CURRENT_REGION() AS region, "
               "CURRENT_USER() AS user_name, CURRENT_ROLE() AS role_name, "
               "CURRENT_WAREHOUSE() AS warehouse, CURRENT_DATABASE() AS database_name, "
               "CURRENT_SCHEMA() AS schema_name, CURRENT_VERSION() AS version");
}

/* ---------- PlantUML ERD ---------- */

DbQueryResult db_generate_erd_plantuml(void) {
    DbQueryResult res;
    SfQueryResult cols, keys;
    SfBuf uml = {NULL, 0, 0, false}, js = {NULL, 0, 0, false};
    char prev[260] = "";
    size_t r;
    int ct, cc, cd, kfk, kfc, kpk;
    bool is_null;
    char num[40];

    if (!g_ready) return fail("Snowflake connection not configured");
    cols = sf_session_query("SELECT table_name, column_name, data_type FROM information_schema.columns "
                            "WHERE table_schema = CURRENT_SCHEMA() ORDER BY table_name, ordinal_position");
    if (!cols.success) {
        res = fail(cols.error_message ? cols.error_message : "Query failed");
        sf_query_free(&cols);
        return res;
    }
    keys = sf_session_query("SHOW IMPORTED KEYS IN SCHEMA");   /* may fail: ERD without relations */

    ct = column_index(&cols, "table_name");
    cc = column_index(&cols, "column_name");
    cd = column_index(&cols, "data_type");
    sf_buf_str(&uml, "@startuml\n");
    for (r = 0; ct >= 0 && cc >= 0 && cd >= 0 && r < row_count(&cols); r++) {
        const char *t = cell_text(rowset_cell(&cols, r, (size_t)ct), num, sizeof(num), &is_null);
        if (strcmp(t, prev) != 0) {
            if (prev[0]) sf_buf_str(&uml, "}\n");
            sf_buf_printf(&uml, "entity \"%s\" {\n", t);
            snprintf(prev, sizeof(prev), "%s", t);
        }
        sf_buf_printf(&uml, "  %s : %s\n",
                      cell_text(rowset_cell(&cols, r, (size_t)cc), num, sizeof(num), &is_null),
                      cell_text(rowset_cell(&cols, r, (size_t)cd), num, sizeof(num), &is_null));
    }
    if (prev[0]) sf_buf_str(&uml, "}\n");

    if (keys.success) {
        kfk = column_index(&keys, "fk_table_name");
        kfc = column_index(&keys, "fk_column_name");
        kpk = column_index(&keys, "pk_table_name");
        for (r = 0; kfk >= 0 && kpk >= 0 && r < row_count(&keys); r++) {
            sf_buf_printf(&uml, "\"%s\" ||--o{ \"%s\" : %s\n",
                          cell_text(rowset_cell(&keys, r, (size_t)kpk), num, sizeof(num), &is_null),
                          cell_text(rowset_cell(&keys, r, (size_t)kfk), num, sizeof(num), &is_null),
                          kfc >= 0 ? cell_text(rowset_cell(&keys, r, (size_t)kfc), num, sizeof(num), &is_null) : "fk");
        }
    }
    sf_buf_str(&uml, "@enduml\n");

    sf_buf_str(&js, "{\"format\":\"plantuml\",\"schema_erd\":\"");
    if (uml.p) sf_buf_json_escaped(&js, uml.p);
    sf_buf_str(&js, "\"}");
    memset(&res, 0, sizeof(res));
    {
        SfBuf md = {NULL, 0, 0, false};
        sf_buf_str(&md, "```plantuml\n");
        if (uml.p) sf_buf_str(&md, uml.p);
        sf_buf_str(&md, "```");
        res.markdown_table = sf_buf_take(&md);
    }
    res.json_structured = sf_buf_take(&js);
    res.success = res.markdown_table && res.json_structured;
    if (!res.success) res.error_message = safe_strdup("Out of memory");
    sf_buf_free(&uml);
    sf_query_free(&cols);
    sf_query_free(&keys);
    return res;
}
