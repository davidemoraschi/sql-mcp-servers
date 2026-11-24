/*
 * db_snowflake_core.c - Snowflake lifecycle, result formatting, and schema tools
 *
 * Covers: sf_db_init, sf_db_close, db_free_result, db_validate_identifier,
 *         db_execute_read, db_execute_write, db_backup, db_explain,
 *         db_describe_table, db_infer_relationships, db_list_tables,
 *         db_check_health, db_generate_erd_plantuml
 *
 * Also exposes the internal g_ready / g_read_only flags and formatting helpers
 * used by db_snowflake_transfer.c via db_snowflake_internal.h.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "db_snowflake.h"
#include "db_snowflake_internal.h"
#include "sf_buf.h"

#define SF_MAX_RESULT_ROWS 1000

/* Shared state accessed by db_snowflake_transfer.c */
bool g_ready     = false;
bool g_read_only = false;

char *sf_safe_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = (char *)malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

DbQueryResult sf_fail(const char *msg) {
    DbQueryResult res; memset(&res, 0, sizeof(res));
    res.error_message = sf_safe_strdup(msg);
    return res;
}

void db_free_result(DbQueryResult *res) {
    if (!res) return;
    free(res->markdown_table); free(res->json_structured); free(res->error_message);
    memset(res, 0, sizeof(DbQueryResult));
}

/* ---------- lifecycle ---------- */

bool sf_db_init(const SfSessionConfig *cfg, bool read_only, char *err, size_t err_size) {
    if (!sf_session_configure(cfg, err, err_size)) return false;
    g_read_only = read_only; g_ready = true; return true;
}

void sf_db_close(void) { sf_session_close(); g_ready = false; }

/* ---------- identifier and statement validation ---------- */

/* Accepts one to three dot-separated unquoted identifiers: [A-Za-z_][A-Za-z0-9_$]* */
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

static const char *skip_noise(const char *p) {
    for (;;) {
        while (isspace((unsigned char)*p) || *p == '(') p++;
        if (p[0] == '-' && p[1] == '-') { while (*p && *p != '\n') p++; }
        else if (p[0] == '/' && p[1] == '*') { p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) p++; if (*p) p += 2; }
        else return p;
    }
}

static bool is_read_only_sql(const char *sql) {
    static const char *const allowed[] = { "SELECT","WITH","SHOW","DESC","DESCRIBE","EXPLAIN","LIST","VALUES" };
    const char *p = skip_noise(sql);
    size_t n = 0; char quote = 0; bool ok = false;
    while (isalpha((unsigned char)p[n])) n++;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strlen(allowed[i]) == n) {
            size_t k = 0;
            while (k < n && toupper((unsigned char)p[k]) == allowed[i][k]) k++;
            if (k == n) ok = true;
        }
    }
    if (!ok) return false;
    for (; *p; p++) {
        if (quote) { if (*p == quote) quote = 0; continue; }
        if (*p == '\'' || *p == '"') { quote = *p; continue; }
        if (*p == ';') return *skip_noise(p + 1) == '\0';
    }
    return true;
}

/* ---------- result formatting (shared with transfer) ---------- */

void sf_md_cell(SfBuf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '|') sf_buf_str(b, "\\|");
        else if (c < 0x20) sf_buf_add(b, " ", 1);
        else sf_buf_add(b, (const char *)&c, 1);
    }
}

const char *sf_cell_text(const JsonValue *v, char *num, size_t num_size, bool *is_null) {
    *is_null = false;
    if (!v || v->type == JSON_NULL) { *is_null = true; return "NULL"; }
    if (v->type == JSON_STRING) return v->u.string ? v->u.string : "";
    if (v->type == JSON_BOOL)   return v->u.boolean ? "true" : "false";
    if (v->type == JSON_NUMBER) { snprintf(num, num_size, "%.15g", v->u.number); return num; }
    return "";
}

const char *sf_column_name(const SfQueryResult *q, size_t i) {
    const JsonValue *col = (q->rowtype && i < q->rowtype->u.array.count) ? q->rowtype->u.array.items[i] : NULL;
    const char *n = col ? json_string_value(json_object_get(col, "name")) : NULL;
    return n ? n : "col";
}

int sf_column_index(const SfQueryResult *q, const char *name) {
    size_t n = q->rowtype ? q->rowtype->u.array.count : 0;
    for (size_t i = 0; i < n; i++) {
        const char *c = sf_column_name(q, i); size_t k = 0;
        while (c[k] && name[k] && tolower((unsigned char)c[k]) == tolower((unsigned char)name[k])) k++;
        if (!c[k] && !name[k]) return (int)i;
    }
    return -1;
}

const JsonValue *sf_rowset_cell(const SfQueryResult *q, size_t r, size_t c) {
    const JsonValue *row = q->rowset->u.array.items[r];
    if (!row || row->type != JSON_ARRAY || c >= row->u.array.count) return NULL;
    return row->u.array.items[c];
}

size_t sf_row_count(const SfQueryResult *q) {
    return (q->rowset && q->rowset->type == JSON_ARRAY) ? q->rowset->u.array.count : 0;
}

size_t sf_col_count(const SfQueryResult *q) {
    return (q->rowtype && q->rowtype->type == JSON_ARRAY) ? q->rowtype->u.array.count : 0;
}

DbQueryResult sf_format_result(const SfQueryResult *q) {
    DbQueryResult res; memset(&res, 0, sizeof(res));
    SfBuf md = {NULL,0,0,false}, js = {NULL,0,0,false};
    size_t cols = sf_col_count(q), rows = sf_row_count(q), shown = rows < SF_MAX_RESULT_ROWS ? rows : SF_MAX_RESULT_ROWS;
    char num[40];

    sf_buf_str(&md, "|");
    for (size_t c = 0; c < cols; c++) { sf_buf_str(&md, " "); sf_md_cell(&md, sf_column_name(q, c)); sf_buf_str(&md, " |"); }
    sf_buf_str(&md, "\n|");
    for (size_t c = 0; c < cols; c++) sf_buf_str(&md, "---|");

    sf_buf_str(&js, "[");
    for (size_t r = 0; r < shown; r++) {
        bool is_null; const char *t;
        sf_buf_str(&md, "\n|");
        sf_buf_str(&js, r ? ",{" : "{");
        for (size_t c = 0; c < cols; c++) {
            t = sf_cell_text(sf_rowset_cell(q, r, c), num, sizeof(num), &is_null);
            sf_buf_str(&md, " "); sf_md_cell(&md, t); sf_buf_str(&md, " |");
            sf_buf_str(&js, c ? ",\"" : "\""); sf_buf_json_escaped(&js, sf_column_name(q, c));
            if (is_null) sf_buf_str(&js, "\":null");
            else { sf_buf_str(&js, "\":\""); sf_buf_json_escaped(&js, t); sf_buf_str(&js, "\""); }
        }
        sf_buf_str(&js, "}");
    }
    sf_buf_str(&js, "]");
    if (rows > shown || q->more_chunks)
        sf_buf_printf(&md, "\n\n(Result truncated: showing %lu of %lu%s rows.)",
                      (unsigned long)shown, (unsigned long)rows, q->more_chunks ? "+" : "");

    res.markdown_table = sf_buf_take(&md); res.json_structured = sf_buf_take(&js);
    if (!res.markdown_table || !res.json_structured) {
        free(res.markdown_table); free(res.json_structured);
        res.markdown_table = res.json_structured = NULL;
        res.error_message = sf_safe_strdup("Out of memory"); return res;
    }
    res.success = true; res.rows_affected = (int)shown; return res;
}

DbQueryResult sf_run(const char *sql) {
    if (!g_ready) return sf_fail("Snowflake connection not configured");
    SfQueryResult q = sf_session_query(sql);
    if (!q.success) {
        DbQueryResult r = sf_fail(q.error_message ? q.error_message : "Query failed");
        sf_query_free(&q); return r;
    }

    /* Build result: format partition 0, then append rows from further partitions */
    DbQueryResult res; memset(&res, 0, sizeof(res));
    SfBuf md = {NULL,0,0,false}, js = {NULL,0,0,false};
    size_t cols = sf_col_count(&q);
    char num[40];
    long shown = 0;

    /* Header row */
    sf_buf_str(&md, "|");
    for (size_t c = 0; c < cols; c++) { sf_buf_str(&md, " "); sf_md_cell(&md, sf_column_name(&q, c)); sf_buf_str(&md, " |"); }
    sf_buf_str(&md, "\n|");
    for (size_t c = 0; c < cols; c++) sf_buf_str(&md, "---|");
    sf_buf_str(&js, "[");

    /* Helper: append one rowset up to the cap */
    #define APPEND_ROWSET(rowset_ptr) do { \
        const JsonValue *_rs = (rowset_ptr); \
        if (_rs && _rs->type == JSON_ARRAY) { \
            size_t _rows = _rs->u.array.count; \
            for (size_t _r = 0; _r < _rows && shown < SF_MAX_RESULT_ROWS; _r++, shown++) { \
                bool _null; const char *_t; \
                const JsonValue *_row = _rs->u.array.items[_r]; \
                if (!_row || _row->type != JSON_ARRAY) continue; \
                sf_buf_str(&md, "\n|"); \
                sf_buf_str(&js, shown ? ",{" : "{"); \
                for (size_t _c = 0; _c < cols; _c++) { \
                    _t = sf_cell_text(_c < _row->u.array.count ? _row->u.array.items[_c] : NULL, num, sizeof(num), &_null); \
                    sf_buf_str(&md, " "); sf_md_cell(&md, _t); sf_buf_str(&md, " |"); \
                    sf_buf_str(&js, _c ? ",\"" : "\""); sf_buf_json_escaped(&js, sf_column_name(&q, _c)); \
                    if (_null) sf_buf_str(&js, "\":null"); \
                    else { sf_buf_str(&js, "\":\""); sf_buf_json_escaped(&js, _t); sf_buf_str(&js, "\""); } \
                } \
                sf_buf_str(&js, "}"); \
            } \
        } \
    } while(0)

    APPEND_ROWSET(q.rowset);

    if (q.more_chunks && shown < SF_MAX_RESULT_ROWS) {
        if (q.next_page_url[0]) {
            /* v1 Link pagination */
            char cur_url[512];
            snprintf(cur_url, sizeof(cur_url), "%s", q.next_page_url);
            while (cur_url[0] && shown < SF_MAX_RESULT_ROWS) {
                SfQueryResult pq = sf_session_fetch_page(cur_url);
                if (!pq.success) { sf_query_free(&pq); break; }
                long before = shown;
                APPEND_ROWSET(pq.rowset);
                snprintf(cur_url, sizeof(cur_url), "%s", pq.next_page_url);
                bool last = pq.is_last_page;
                sf_query_free(&pq);
                if (shown == before || last) break;
            }
        } else if (q.statement_handle[0]) {
            /* v2 partition pagination */
            for (int p = 1; (q.partition_count == 0 || p < q.partition_count) && shown < SF_MAX_RESULT_ROWS; p++) {
                SfQueryResult pq = sf_session_fetch_partition(q.statement_handle, p);
                if (!pq.success) { sf_query_free(&pq); break; }
                long before = shown;
                APPEND_ROWSET(pq.rowset);
                sf_query_free(&pq);
                if (shown == before) break;
            }
        }
    }
    #undef APPEND_ROWSET

    sf_buf_str(&js, "]");
    if (q.more_chunks)
        sf_buf_printf(&md, "\n\n(Result truncated: showing %ld of 1000+ rows.)", shown);

    sf_query_free(&q);
    res.markdown_table = sf_buf_take(&md); res.json_structured = sf_buf_take(&js);
    if (!res.markdown_table || !res.json_structured) {
        free(res.markdown_table); free(res.json_structured);
        res.error_message = sf_safe_strdup("Out of memory"); return res;
    }
    res.success = true; res.rows_affected = (int)shown; return res;
}

/* ---------- db_* interface ---------- */

DbQueryResult db_execute_read(const char *sql_query, const char *params_json) {
    (void)params_json;
    if (!sql_query || !*sql_query) return sf_fail("SQL query cannot be null or empty");
    if (!is_read_only_sql(sql_query))
        return sf_fail("read_query accepts SELECT, WITH, SHOW, DESCRIBE, EXPLAIN, LIST or VALUES; use write_query for other statements");
    return sf_run(sql_query);
}

DbQueryResult db_execute_write(const char *sql_query, const char *params_json) {
    (void)params_json;
    if (!sql_query || !*sql_query) return sf_fail("SQL query cannot be null or empty");
    if (g_read_only) return sf_fail("Server is in READONLY mode (SNOWFLAKE_ACCESS_MODE); write queries are disabled");
    return sf_run(sql_query);
}

DbQueryResult db_backup(const char *target_path) { (void)target_path; return sf_fail("backup_database is not supported for Snowflake"); }

DbQueryResult db_explain(const char *sql_query) {
    if (!sql_query || !*sql_query) return sf_fail("SQL query cannot be null or empty");
    SfBuf b = {NULL,0,0,false};
    sf_buf_str(&b, "EXPLAIN USING TEXT "); sf_buf_str(&b, sql_query);
    char *sql = sf_buf_take(&b);
    if (!sql) return sf_fail("Out of memory");
    DbQueryResult res = sf_run(sql); free(sql); return res;
}

DbQueryResult db_describe_table(const char *table_name) {
    char sql[320];
    if (!table_name || !db_validate_identifier(table_name) || strlen(table_name) > 256) return sf_fail("Invalid table identifier");
    snprintf(sql, sizeof(sql), "DESCRIBE TABLE %s", table_name);
    return sf_run(sql);
}

DbQueryResult db_infer_relationships(void) { return sf_run("SHOW IMPORTED KEYS IN SCHEMA"); }

DbQueryResult db_list_tables(void) {
    return sf_run("SELECT table_name, table_type AS type, row_count, bytes "
                  "FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY table_name");
}

DbQueryResult db_check_health(void) {
    return sf_run("SELECT CURRENT_ACCOUNT() AS account, CURRENT_REGION() AS region, "
                  "CURRENT_USER() AS user_name, CURRENT_ROLE() AS role_name, "
                  "CURRENT_WAREHOUSE() AS warehouse, CURRENT_DATABASE() AS database_name, "
                  "CURRENT_SCHEMA() AS schema_name, CURRENT_VERSION() AS version");
}

DbQueryResult db_generate_erd_plantuml(void) {
    if (!g_ready) return sf_fail("Snowflake connection not configured");
    SfQueryResult cols = sf_session_query(
        "SELECT table_name, column_name, data_type FROM information_schema.columns "
        "WHERE table_schema = CURRENT_SCHEMA() ORDER BY table_name, ordinal_position");
    if (!cols.success) {
        DbQueryResult r = sf_fail(cols.error_message ? cols.error_message : "Query failed");
        sf_query_free(&cols); return r;
    }
    SfQueryResult keys = sf_session_query("SHOW IMPORTED KEYS IN SCHEMA");

    int ct = sf_column_index(&cols, "table_name"), cc = sf_column_index(&cols, "column_name"), cd = sf_column_index(&cols, "data_type");
    SfBuf uml = {NULL,0,0,false}, js = {NULL,0,0,false};
    char prev[260] = "", num[40]; bool is_null;

    sf_buf_str(&uml, "@startuml\n");
    for (size_t r = 0; ct >= 0 && cc >= 0 && cd >= 0 && r < sf_row_count(&cols); r++) {
        const char *t = sf_cell_text(sf_rowset_cell(&cols, r, (size_t)ct), num, sizeof(num), &is_null);
        if (strcmp(t, prev) != 0) {
            if (prev[0]) sf_buf_str(&uml, "}\n");
            sf_buf_printf(&uml, "entity \"%s\" {\n", t);
            snprintf(prev, sizeof(prev), "%s", t);
        }
        sf_buf_printf(&uml, "  %s : %s\n",
            sf_cell_text(sf_rowset_cell(&cols, r, (size_t)cc), num, sizeof(num), &is_null),
            sf_cell_text(sf_rowset_cell(&cols, r, (size_t)cd), num, sizeof(num), &is_null));
    }
    if (prev[0]) sf_buf_str(&uml, "}\n");
    if (keys.success) {
        int kfk = sf_column_index(&keys, "fk_table_name"), kfc = sf_column_index(&keys, "fk_column_name"), kpk = sf_column_index(&keys, "pk_table_name");
        for (size_t r = 0; kfk >= 0 && kpk >= 0 && r < sf_row_count(&keys); r++)
            sf_buf_printf(&uml, "\"%s\" ||--o{ \"%s\" : %s\n",
                sf_cell_text(sf_rowset_cell(&keys, r, (size_t)kpk), num, sizeof(num), &is_null),
                sf_cell_text(sf_rowset_cell(&keys, r, (size_t)kfk), num, sizeof(num), &is_null),
                kfc >= 0 ? sf_cell_text(sf_rowset_cell(&keys, r, (size_t)kfc), num, sizeof(num), &is_null) : "fk");
    }
    sf_buf_str(&uml, "@enduml\n");

    sf_buf_str(&js, "{\"format\":\"plantuml\",\"schema_erd\":\"");
    if (uml.p) sf_buf_json_escaped(&js, uml.p);
    sf_buf_str(&js, "\"}");

    DbQueryResult res; memset(&res, 0, sizeof(res));
    SfBuf md = {NULL,0,0,false};
    sf_buf_str(&md, "```plantuml\n"); if (uml.p) sf_buf_str(&md, uml.p); sf_buf_str(&md, "```");
    res.markdown_table = sf_buf_take(&md); res.json_structured = sf_buf_take(&js);
    res.success = res.markdown_table && res.json_structured;
    if (!res.success) res.error_message = sf_safe_strdup("Out of memory");
    sf_buf_free(&uml); sf_query_free(&cols); sf_query_free(&keys);
    return res;
}
