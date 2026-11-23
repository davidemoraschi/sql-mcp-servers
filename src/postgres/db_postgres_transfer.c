/*
 * db_postgres_transfer.c - PostgreSQL bulk data export and import tools
 *
 * Implements db_export_data() and db_import_data() for the PostgreSQL MCP server.
 *
 * Export: runs a SELECT against PostgreSQL and streams the result set to a
 *         local file in csv, tsv, json, or sql_dump format.
 *
 * Import: reads a local delimited file row-by-row and executes one INSERT per
 *         row.  auto_create_table infers TEXT columns from the header row.
 *         on_error=abort stops on the first failure; =continue skips bad rows.
 *
 * Security: destination_path and source_path are checked for ".." traversal.
 *           destination_table is validated with db_validate_identifier().
 *           Row values are single-quoted with sf_buf_json_escaped() escaping.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db_postgres.h"
#include "db_postgres_internal.h"

DbQueryResult db_export_data(const char *target, const char *destination_path, const char *format,
                              const char *compression, bool header, const char *delimiter) {
    (void)compression;
    const char *fmt = format ? format : "csv";
    char dc = (delimiter && delimiter[0]) ? delimiter[0] : (strcmp(fmt, "tsv") == 0 ? '\t' : ',');

    if (!target || !*target)                     return pg_fail("target is required");
    if (!destination_path || !*destination_path) return pg_fail("destination_path is required");
    if (strstr(destination_path, ".."))          return pg_fail("Path traversal not allowed");

    bool is_sel = (strncmp(target,"SELECT",6)==0 || strncmp(target,"select",6)==0
                   || strncmp(target,"WITH",4)==0 || strncmp(target,"with",4)==0);
    if (!is_sel && !db_validate_identifier(target)) return pg_fail("Invalid table identifier");

    SfBuf qb = {NULL,0,0,false};
    if (is_sel) sf_buf_str(&qb, target);
    else { sf_buf_str(&qb, "SELECT * FROM "); sf_buf_str(&qb, target); }
    char *sql = sf_buf_take(&qb);
    if (!sql) return pg_fail("Out of memory");
    PgResult r = pg_exec(sql); free(sql);
    if (!r.success) { DbQueryResult res = pg_fail(r.error ? r.error : "Query failed"); pg_result_free(&r); return res; }

    FILE *f = fopen(destination_path, "w");
    if (!f) { pg_result_free(&r); return pg_fail("Cannot open destination file for writing"); }

    long rw = 0;
    if (strcmp(fmt, "json") == 0) {
        fputs("[", f);
        for (int row = 0; row < r.row_count; row++) {
            if (row > 0) fputs(",", f);
            fputs("{", f);
            for (int c = 0; c < r.col_count; c++) {
                const char *v = r.rows[row][c];
                if (c > 0) fputs(",", f);
                fprintf(f, "\"%s\":", r.col_names[c] ? r.col_names[c] : "");
                if (v) fprintf(f, "\"%s\"", v); else fputs("null", f);
            }
            fputs("}", f); rw++;
        }
        fputs("]\n", f);
    } else if (strcmp(fmt, "sql_dump") == 0) {
        const char *tn = is_sel ? "export" : target;
        for (int row = 0; row < r.row_count; row++) {
            fprintf(f, "INSERT INTO \"%s\" VALUES(", tn);
            for (int c = 0; c < r.col_count; c++) {
                const char *v = r.rows[row][c];
                if (c > 0) fputs(",", f);
                if (v) fprintf(f, "'%s'", v); else fputs("NULL", f);
            }
            fputs(");\n", f); rw++;
        }
    } else {
        if (header) {
            for (int c = 0; c < r.col_count; c++) {
                if (c > 0) fputc(dc, f);
                fputs(r.col_names[c] ? r.col_names[c] : "", f);
            }
            fputc('\n', f);
        }
        for (int row = 0; row < r.row_count; row++) {
            for (int c = 0; c < r.col_count; c++) {
                if (c > 0) fputc(dc, f);
                if (r.rows[row][c]) fputs(r.rows[row][c], f);
            }
            fputc('\n', f); rw++;
        }
    }
    long bw = ftell(f); fclose(f); pg_result_free(&r);

    SfBuf md = {NULL,0,0,false}, js = {NULL,0,0,false};
    sf_buf_printf(&md, "| status | target | format | rows | bytes | file |\n|---|---|---|---|---|---|\n| success | %s | %s | %ld | %ld | %s |", target, fmt, rw, bw, destination_path);
    sf_buf_printf(&js, "{\"status\":\"success\",\"engine\":\"postgresql\",\"operation\":\"export\",\"target\":\"%s\",\"rows_processed\":%ld,\"bytes_processed\":%ld,\"duration_seconds\":0,\"files_created_or_read\":[\"%s\"],\"warnings\":[]}", target, rw, bw, destination_path);
    DbQueryResult res; memset(&res, 0, sizeof(res));
    res.markdown_table = sf_buf_take(&md); res.json_structured = sf_buf_take(&js);
    res.success = res.markdown_table && res.json_structured;
    if (!res.success) res.error_message = pg_strdup("Out of memory");
    res.rows_affected = (int)rw; return res;
}

DbQueryResult db_import_data(const char *source_path, const char *destination_table, const char *format,
                              const char *compression, bool header, const char *delimiter,
                              const char *on_error, bool auto_create_table, bool optimize_speed) {
    (void)compression;
    const char *fmt    = format   ? format   : "csv";
    const char *on_err = on_error ? on_error : "abort";
    char dc = (delimiter && delimiter[0]) ? delimiter[0] : (strcmp(fmt, "tsv") == 0 ? '\t' : ',');

    if (!source_path || !*source_path)             return pg_fail("source_path is required");
    if (!destination_table || !*destination_table) return pg_fail("destination_table is required");
    if (strstr(source_path, ".."))                 return pg_fail("Path traversal not allowed");
    if (!db_validate_identifier(destination_table)) return pg_fail("Invalid destination_table identifier");
    if (g_read_only) return pg_fail("Server is in READONLY mode; import is disabled");

    FILE *f = fopen(source_path, "r");
    if (!f) return pg_fail("Cannot open source file for reading");

    char line[65536];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return pg_fail("Empty source file"); }
    size_t ll = strlen(line);
    while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r')) line[--ll] = '\0';

    int col_count = 0;
    { char *tmp = pg_strdup(line); char *p = tmp; while (p) { col_count++; p = strchr(p, dc); if (p) p++; } free(tmp); }

    char **col_names = (char **)malloc((size_t)col_count * sizeof(char *));
    { char *tmp = pg_strdup(line); char *p = tmp; int ci = 0;
      while (p && ci < col_count) {
          char *end = strchr(p, dc); if (end) *end = '\0';
          size_t fl = strlen(p);
          if (fl >= 2 && p[0] == '"' && p[fl-1] == '"') { p++; p[fl-2] = '\0'; }
          col_names[ci++] = pg_strdup(p); p = end ? end + 1 : NULL;
      }
      free(tmp); }

    if (auto_create_table) {
        SfBuf b = {NULL,0,0,false};
        sf_buf_printf(&b, "CREATE TABLE IF NOT EXISTS %s (", destination_table);
        for (int i = 0; i < col_count; i++)
            sf_buf_printf(&b, "%s\"%s\" TEXT", i > 0 ? "," : "", col_names[i] ? col_names[i] : "col");
        sf_buf_str(&b, ")");
        char *create_sql = sf_buf_take(&b);
        if (!create_sql) { for (int i=0;i<col_count;i++) free(col_names[i]); free(col_names); fclose(f); return pg_fail("Out of memory"); }
        PgResult cr = pg_exec(create_sql); free(create_sql);
        if (!cr.success) {
            DbQueryResult res = pg_fail(cr.error ? cr.error : "CREATE TABLE failed");
            pg_result_free(&cr); for (int i=0;i<col_count;i++) free(col_names[i]); free(col_names); fclose(f); return res;
        }
        pg_result_free(&cr);
    }

    if (optimize_speed) pg_run("SET synchronous_commit = OFF");

    long rows_ok = 0, rows_err = 0;
    bool abort_on_error = (strcmp(on_err, "abort") == 0), aborted = false;
    if (!header) rewind(f);

    while (!aborted && fgets(line, sizeof(line), f)) {
        ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r')) line[--ll] = '\0';
        if (ll == 0) continue;

        SfBuf ins = {NULL,0,0,false};
        sf_buf_printf(&ins, "INSERT INTO %s VALUES (", destination_table);
        char *tmp = pg_strdup(line); char *p = tmp; int vi = 0;
        while (p && vi < col_count) {
            char *end = strchr(p, dc); if (end) *end = '\0';
            size_t fl = strlen(p);
            if (fl >= 2 && p[0] == '"' && p[fl-1] == '"') { p++; p[fl-2] = '\0'; }
            if (vi > 0) sf_buf_str(&ins, ",");
            if (*p) { sf_buf_str(&ins, "'"); sf_buf_json_escaped(&ins, p); sf_buf_str(&ins, "'"); }
            else sf_buf_str(&ins, "NULL");
            p = end ? end + 1 : NULL; vi++;
        }
        free(tmp);
        sf_buf_str(&ins, ")");
        char *isql = sf_buf_take(&ins);
        if (!isql) { aborted = true; break; }
        PgResult ir = pg_exec(isql); free(isql);
        if (ir.success) rows_ok++; else { rows_err++; if (abort_on_error) aborted = true; }
        pg_result_free(&ir);
    }
    fclose(f);
    if (optimize_speed) pg_run("SET synchronous_commit = ON");
    for (int i = 0; i < col_count; i++) free(col_names[i]);
    free(col_names);

    if (aborted) return pg_fail("Import aborted due to row error (on_error=abort)");

    SfBuf md = {NULL,0,0,false}, js = {NULL,0,0,false};
    sf_buf_printf(&md, "| status | destination | rows_processed | errors |\n|---|---|---|---|\n| success | %s | %ld | %ld |", destination_table, rows_ok, rows_err);
    sf_buf_printf(&js, "{\"status\":\"success\",\"engine\":\"postgresql\",\"operation\":\"import\",\"target\":\"%s\",\"rows_processed\":%ld,\"bytes_processed\":0,\"duration_seconds\":0,\"files_created_or_read\":[\"%s\"],\"warnings\":[]}", destination_table, rows_ok, source_path);
    DbQueryResult res; memset(&res, 0, sizeof(res));
    res.markdown_table = sf_buf_take(&md); res.json_structured = sf_buf_take(&js);
    res.success = res.markdown_table && res.json_structured;
    if (!res.success) res.error_message = pg_strdup("Out of memory");
    res.rows_affected = (int)rows_ok; return res;
}
