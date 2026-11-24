/*
 * db_snowflake_transfer.c - Snowflake bulk data export and import tools
 *
 * Export: issues a single SELECT, writes the inline rowset (partition 0), then
 *         fetches additional partitions via sf_session_fetch_partition.
 *
 * Import: reads a local delimited file and executes one INSERT per row.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db_snowflake.h"
#include "db_snowflake_internal.h"

static size_t strip_newline(char *line) {
    size_t ll = strlen(line);
    while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r')) line[--ll] = '\0';
    return ll;
}

static void parse_delimited_line(char *tmp, char delim, char **vals, int col_count) {
    char *p = tmp; int vi = 0;
    while (p && vi < col_count) {
        char *end = strchr(p, delim); if (end) *end = '\0';
        size_t fl = strlen(p);
        if (fl >= 2 && p[0] == '"' && p[fl-1] == '"') { p++; p[fl-2] = '\0'; }
        vals[vi++] = p;
        p = end ? end + 1 : NULL;
    }
    while (vi < col_count) vals[vi++] = NULL;
}

/* Write one JSON array of row-arrays to file. Returns number of rows written. */
static long write_rowset(FILE *f, const JsonValue *rowset, size_t cols,
                         char **col_names, bool is_json, char dc,
                         char *num, long rows_so_far) {
    if (!rowset || rowset->type != JSON_ARRAY) return 0;
    size_t rows = rowset->u.array.count;
    bool is_null;
    for (size_t r = 0; r < rows; r++) {
        const JsonValue *row = rowset->u.array.items[r];
        if (!row || row->type != JSON_ARRAY) continue;
        if (is_json) {
            if (rows_so_far + (long)r > 0) fputs(",", f);
            fputs("{", f);
            for (size_t c = 0; c < cols; c++) {
                const char *t = sf_cell_text(c < row->u.array.count ? row->u.array.items[c] : NULL,
                                             num, 40, &is_null);
                if (c > 0) fputs(",", f);
                fprintf(f, "\"%s\":", col_names ? col_names[c] : "col");
                if (is_null) fputs("null", f); else fprintf(f, "\"%s\"", t);
            }
            fputs("}", f);
        } else {
            for (size_t c = 0; c < cols; c++) {
                if (c > 0) fputc(dc, f);
                const char *t = sf_cell_text(c < row->u.array.count ? row->u.array.items[c] : NULL,
                                             num, 40, &is_null);
                if (!is_null) fputs(t, f);
            }
            fputc('\n', f);
        }
    }
    return (long)rows;
}

DbQueryResult db_export_data(const char *target, const char *destination_path, const char *format,
                              const char *compression, bool header, const char *delimiter) {
    (void)compression;
    const char *fmt = format ? format : "csv";
    char dc = (delimiter && delimiter[0]) ? delimiter[0] : (strcmp(fmt, "tsv") == 0 ? '\t' : ',');

    if (!target || !*target)                     return sf_fail("target is required");
    if (!destination_path || !*destination_path) return sf_fail("destination_path is required");
    if (strstr(destination_path, ".."))          return sf_fail("Path traversal not allowed");

    bool is_sel = (strncmp(target, "SELECT", 6) == 0 || strncmp(target, "select", 6) == 0
                   || strncmp(target, "WITH", 4) == 0 || strncmp(target, "with", 4) == 0);
    if (!is_sel && (!db_validate_identifier(target) || strlen(target) > 256))
        return sf_fail("Invalid table identifier");

    SfBuf qb = {NULL,0,0,false};
    if (is_sel) sf_buf_str(&qb, target);
    else { sf_buf_str(&qb, "SELECT * FROM "); sf_buf_str(&qb, target); }
    char *sql = sf_buf_take(&qb);
    if (!sql) return sf_fail("Out of memory");

    SfQueryResult q = sf_session_query(sql); free(sql);
    if (!q.success) {
        DbQueryResult r = sf_fail(q.error_message ? q.error_message : "Query failed");
        sf_query_free(&q); return r;
    }

    FILE *f = fopen(destination_path, "w");
    if (!f) { sf_query_free(&q); return sf_fail("Cannot open destination file for writing"); }

    size_t cols = sf_col_count(&q);
    char **col_names = (char **)calloc(cols, sizeof(char *));
    if (col_names) {
        for (size_t c = 0; c < cols; c++)
            col_names[c] = sf_safe_strdup(sf_column_name(&q, c));
    }

    bool is_json = strcmp(fmt, "json") == 0;
    char num[40];
    long rw = 0;

    if (is_json) fputs("[", f);
    if (!is_json && header && col_names) {
        for (size_t c = 0; c < cols; c++) { if (c > 0) fputc(dc, f); fputs(col_names[c], f); }
        fputc('\n', f);
    }

    /* Partition 0: inline rowset */
    rw += write_rowset(f, q.rowset, cols, col_names, is_json, dc, num, rw);

    /* Additional pages/partitions */
    if (q.more_chunks) {
        if (q.next_page_url[0]) {
            /* v1 Link pagination */
            char cur_url[512];
            snprintf(cur_url, sizeof(cur_url), "%s", q.next_page_url);
            while (cur_url[0]) {
                SfQueryResult pq = sf_session_fetch_page(cur_url);
                if (!pq.success) { sf_query_free(&pq); break; }
                long written = write_rowset(f, pq.rowset, cols, col_names, is_json, dc, num, rw);
                rw += written;
                snprintf(cur_url, sizeof(cur_url), "%s", pq.next_page_url);
                bool last = pq.is_last_page;
                sf_query_free(&pq);
                if (written == 0 || last) break;
            }
        } else if (q.statement_handle[0]) {
            /* v2 partition pagination */
            int total = q.partition_count;
            for (int p = 1; total == 0 || p < total; p++) {
                SfQueryResult pq = sf_session_fetch_partition(q.statement_handle, p);
                if (!pq.success) { sf_query_free(&pq); break; }
                long written = write_rowset(f, pq.rowset, cols, col_names, is_json, dc, num, rw);
                rw += written;
                sf_query_free(&pq);
                if (written == 0) break;
            }
        }
    }

    if (is_json) fputs("]\n", f);
    long bw = ftell(f); fclose(f);
    sf_query_free(&q);
    if (col_names) { for (size_t c = 0; c < cols; c++) free(col_names[c]); free(col_names); }

    SfBuf md = {NULL,0,0,false}, js = {NULL,0,0,false};
    sf_buf_printf(&md, "| status | target | format | rows | bytes | file |\n|---|---|---|---|---|---|\n| success | %s | %s | %ld | %ld | %s |", target, fmt, rw, bw, destination_path);
    sf_buf_printf(&js, "{\"status\":\"success\",\"engine\":\"snowflake\",\"operation\":\"export\",\"target\":\"%s\",\"rows_processed\":%ld,\"bytes_processed\":%ld,\"duration_seconds\":0,\"files_created_or_read\":[\"%s\"],\"warnings\":[]}", target, rw, bw, destination_path);
    DbQueryResult res; memset(&res, 0, sizeof(res));
    res.markdown_table = sf_buf_take(&md); res.json_structured = sf_buf_take(&js);
    res.success = res.markdown_table && res.json_structured;
    if (!res.success) res.error_message = sf_safe_strdup("Out of memory");
    res.rows_affected = (int)rw; return res;
}

DbQueryResult db_import_data(const char *source_path, const char *destination_table, const char *format,
                              const char *compression, bool header, const char *delimiter,
                              const char *on_error, bool auto_create_table, bool optimize_speed) {
    (void)compression; (void)optimize_speed;
    const char *fmt    = format   ? format   : "csv";
    const char *on_err = on_error ? on_error : "abort";
    char dc = (delimiter && delimiter[0]) ? delimiter[0] : (strcmp(fmt, "tsv") == 0 ? '\t' : ',');

    if (!source_path || !*source_path)             return sf_fail("source_path is required");
    if (!destination_table || !*destination_table) return sf_fail("destination_table is required");
    if (strstr(source_path, ".."))                 return sf_fail("Path traversal not allowed");
    if (!db_validate_identifier(destination_table) || strlen(destination_table) > 256)
        return sf_fail("Invalid destination_table identifier");
    if (g_read_only) return sf_fail("Server is in READONLY mode; import is disabled");

    FILE *f = fopen(source_path, "r");
    if (!f) return sf_fail("Cannot open source file for reading");

    char line[65536];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return sf_fail("Empty source file"); }
    strip_newline(line);

    int col_count = 0;
    { char *tmp = sf_safe_strdup(line); char *p = tmp;
      while (p) { col_count++; p = strchr(p, dc); if (p) p++; }
      free(tmp); }

    char **col_names = (char **)malloc((size_t)col_count * sizeof(char *));
    { char *tmp = sf_safe_strdup(line); char *p = tmp; int ci = 0;
      while (p && ci < col_count) {
          char *end = strchr(p, dc); if (end) *end = '\0';
          size_t fl = strlen(p);
          if (fl >= 2 && p[0] == '"' && p[fl-1] == '"') { p++; p[fl-2] = '\0'; }
          col_names[ci++] = sf_safe_strdup(p);
          p = end ? end + 1 : NULL;
      }
      free(tmp); }

    if (auto_create_table) {
        SfBuf b = {NULL,0,0,false};
        sf_buf_printf(&b, "CREATE TABLE IF NOT EXISTS %s (", destination_table);
        for (int i = 0; i < col_count; i++)
            sf_buf_printf(&b, "%s\"%s\" TEXT", i > 0 ? "," : "", col_names[i] ? col_names[i] : "col");
        sf_buf_str(&b, ")");
        char *create_sql = sf_buf_take(&b);
        if (!create_sql) { for (int i=0;i<col_count;i++) free(col_names[i]); free(col_names); fclose(f); return sf_fail("Out of memory"); }
        SfQueryResult qr = sf_session_query(create_sql); free(create_sql);
        if (!qr.success) {
            DbQueryResult r = sf_fail(qr.error_message ? qr.error_message : "CREATE TABLE failed");
            sf_query_free(&qr); for (int i=0;i<col_count;i++) free(col_names[i]); free(col_names); fclose(f); return r;
        }
        sf_query_free(&qr);
    }

    long rows_ok = 0, rows_err = 0;
    bool abort_on_error = (strcmp(on_err, "abort") == 0), aborted = false;
    if (!header) rewind(f);

    char *vals[1024];
    while (!aborted && fgets(line, sizeof(line), f)) {
        if (strip_newline(line) == 0) continue;
        char *tmp = sf_safe_strdup(line);
        parse_delimited_line(tmp, dc, vals, col_count);

        SfBuf row_sql = {NULL,0,0,false};
        sf_buf_printf(&row_sql, "INSERT INTO %s VALUES (", destination_table);
        for (int i = 0; i < col_count; i++) {
            if (i > 0) sf_buf_str(&row_sql, ",");
            if (vals[i] && *vals[i]) { sf_buf_str(&row_sql, "'"); sf_buf_json_escaped(&row_sql, vals[i]); sf_buf_str(&row_sql, "'"); }
            else sf_buf_str(&row_sql, "NULL");
        }
        sf_buf_str(&row_sql, ")");
        free(tmp);
        char *row_s = sf_buf_take(&row_sql);
        if (!row_s) { aborted = true; break; }
        SfQueryResult qr = sf_session_query(row_s); free(row_s);
        if (qr.success) rows_ok++; else { rows_err++; if (abort_on_error) aborted = true; }
        sf_query_free(&qr);
    }
    fclose(f);
    for (int i = 0; i < col_count; i++) free(col_names[i]);
    free(col_names);

    if (aborted) return sf_fail("Import aborted due to row error (on_error=abort)");

    SfBuf md = {NULL,0,0,false}, js = {NULL,0,0,false};
    sf_buf_printf(&md, "| status | destination | rows_processed | errors |\n|---|---|---|---|\n| success | %s | %ld | %ld |", destination_table, rows_ok, rows_err);
    sf_buf_printf(&js, "{\"status\":\"success\",\"engine\":\"snowflake\",\"operation\":\"import\",\"target\":\"%s\",\"rows_processed\":%ld,\"bytes_processed\":0,\"duration_seconds\":0,\"files_created_or_read\":[\"%s\"],\"warnings\":[]}", destination_table, rows_ok, source_path);
    DbQueryResult res; memset(&res, 0, sizeof(res));
    res.markdown_table = sf_buf_take(&md); res.json_structured = sf_buf_take(&js);
    res.success = res.markdown_table && res.json_structured;
    if (!res.success) res.error_message = sf_safe_strdup("Out of memory");
    res.rows_affected = (int)rows_ok; return res;
}
