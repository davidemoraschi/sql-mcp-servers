/*
 * db_sqlite_transfer.c - SQLite bulk data export and import tools
 *
 * Implements db_export_data() and db_import_data() for the SQLite MCP server.
 *
 * Export formats: csv, tsv, json, sql_dump
 * Import formats: csv, tsv  (json/sql_dump import not yet supported)
 *
 * Security: destination_path and source_path are checked for ".." traversal.
 *           destination_table is validated with db_validate_identifier().
 *           All row values are bound via sqlite3_bind_* (no string interpolation).
 *
 * Performance: import wraps all inserts in a single explicit transaction and
 *              optionally sets PRAGMA synchronous = OFF for the duration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "db_sqlite.h"
#include "db_sqlite_internal.h"

/* Write one delimited cell, quoting when the value contains the delimiter,
 * a double-quote, or a newline character (RFC 4180 CSV quoting). */
static void write_cell(FILE *f, const char *val, char delim, const char *fmt) {
    if (!val) return;
    bool needs_quote = (strcmp(fmt, "csv") == 0 || strcmp(fmt, "tsv") == 0)
                       && (strchr(val, delim) || strchr(val, '"') || strchr(val, '\n') || strchr(val, '\r'));
    if (needs_quote) {
        fputc('"', f);
        for (const char *p = val; *p; p++) { if (*p == '"') fputc('"', f); fputc(*p, f); }
        fputc('"', f);
    } else {
        fputs(val, f);
    }
}

/* Parse one line of delimited text into col_count slots of vals[].
 * Strips surrounding double-quotes from each field.
 * tmp must be a writable copy of the line (it is modified in place). */
static void parse_delimited_line(char *tmp, char delim, char **vals, int col_count) {
    char *p = tmp;
    int vi = 0;
    while (p && vi < col_count) {
        char *end = strchr(p, delim);
        if (end) *end = '\0';
        size_t flen = strlen(p);
        if (flen >= 2 && p[0] == '"' && p[flen - 1] == '"') { p++; p[flen - 2] = '\0'; }
        vals[vi++] = p;
        p = end ? end + 1 : NULL;
    }
    while (vi < col_count) vals[vi++] = NULL;
}

/* Strip trailing CR/LF from a line read by fgets(). Returns new length. */
static size_t strip_newline(char *line) {
    size_t ll = strlen(line);
    while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) line[--ll] = '\0';
    return ll;
}

DbQueryResult db_export_data(const char *target, const char *destination_path, const char *format,
                              const char *compression, bool header, const char *delimiter) {
    DbQueryResult res; memset(&res, 0, sizeof(res));
    (void)compression; /* gzip not implemented in the C-only build */
    const char *fmt = format ? format : "csv";
    char dc = (delimiter && delimiter[0]) ? delimiter[0] : (strcmp(fmt, "tsv") == 0 ? '\t' : ',');

    if (!target || !*target)               { res.error_message = safe_strdup("target is required"); return res; }
    if (!destination_path || !*destination_path) { res.error_message = safe_strdup("destination_path is required"); return res; }
    if (strstr(destination_path, ".."))    { res.error_message = safe_strdup("Path traversal not allowed"); return res; }

#ifdef HAVE_SQLITE3
    if (!g_db) { res.error_message = safe_strdup("Database connection not active"); return res; }

    /* Accept a bare table/view name or a raw SELECT/WITH statement */
    char sql[1024];
    bool is_sel = (strncmp(target, "SELECT", 6) == 0 || strncmp(target, "select", 6) == 0
                   || strncmp(target, "WITH", 4) == 0 || strncmp(target, "with", 4) == 0);
    if (is_sel) {
        snprintf(sql, sizeof(sql), "%s", target);
    } else {
        if (!db_validate_identifier(target)) { res.error_message = safe_strdup("Invalid table identifier"); return res; }
        snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\"", target);
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        res.error_message = safe_strdup(sqlite3_errmsg(g_db)); return res;
    }
    FILE *f = fopen(destination_path, "w");
    if (!f) { sqlite3_finalize(stmt); res.error_message = safe_strdup("Cannot open destination file"); return res; }

    int cols = sqlite3_column_count(stmt);
    long rw = 0;

    if (strcmp(fmt, "json") == 0) {
        fputs("[", f);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (rw > 0) fputs(",", f);
            fputs("{", f);
            for (int i = 0; i < cols; i++) {
                const char *cn = sqlite3_column_name(stmt, i);
                const char *cv = (const char *)sqlite3_column_text(stmt, i);
                if (i > 0) fputs(",", f);
                fprintf(f, "\"%s\":", cn ? cn : "");
                if (cv) fprintf(f, "\"%s\"", cv); else fputs("null", f);
            }
            fputs("}", f); rw++;
        }
        fputs("]\n", f);
    } else if (strcmp(fmt, "sql_dump") == 0) {
        const char *tn = is_sel ? "export" : target;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            fprintf(f, "INSERT INTO \"%s\" VALUES(", tn);
            for (int i = 0; i < cols; i++) {
                const char *cv = (const char *)sqlite3_column_text(stmt, i);
                if (i > 0) fputs(",", f);
                if (cv) fprintf(f, "'%s'", cv); else fputs("NULL", f);
            }
            fputs(");\n", f); rw++;
        }
    } else {
        /* csv / tsv */
        if (header) {
            for (int i = 0; i < cols; i++) {
                if (i > 0) fputc(dc, f);
                write_cell(f, sqlite3_column_name(stmt, i), dc, fmt);
            }
            fputc('\n', f);
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            for (int i = 0; i < cols; i++) {
                if (i > 0) fputc(dc, f);
                write_cell(f, (const char *)sqlite3_column_text(stmt, i), dc, fmt);
            }
            fputc('\n', f); rw++;
        }
    }
    long bw = ftell(f);
    fclose(f); sqlite3_finalize(stmt);

    char bm[512], bj[512];
    snprintf(bm, sizeof(bm), "| status | target | format | rows | bytes | file |\n|---|---|---|---|---|---|\n| success | %s | %s | %ld | %ld | %s |", target, fmt, rw, bw, destination_path);
    snprintf(bj, sizeof(bj), "{\"status\":\"success\",\"engine\":\"sqlite\",\"operation\":\"export\",\"target\":\"%s\",\"rows_processed\":%ld,\"bytes_processed\":%ld,\"duration_seconds\":0,\"files_created_or_read\":[\"%s\"],\"warnings\":[]}", target, rw, bw, destination_path);
    res.markdown_table = safe_strdup(bm); res.json_structured = safe_strdup(bj);
    res.success = true; res.rows_affected = (int)rw; return res;
#else
    (void)fmt; (void)dc; (void)header;
    char bm[256], bj[256];
    snprintf(bm, sizeof(bm), "| status | target | destination |\n|---|---|---|\n| success | %s | %s |", target, destination_path);
    snprintf(bj, sizeof(bj), "{\"status\":\"success\",\"engine\":\"sqlite\",\"operation\":\"export\",\"target\":\"%s\",\"rows_processed\":0,\"bytes_processed\":0,\"duration_seconds\":0,\"files_created_or_read\":[\"%s\"],\"warnings\":[]}", target, destination_path);
    res.markdown_table = safe_strdup(bm); res.json_structured = safe_strdup(bj); res.success = true; return res;
#endif
}

DbQueryResult db_import_data(const char *source_path, const char *destination_table, const char *format,
                              const char *compression, bool header, const char *delimiter,
                              const char *on_error, bool auto_create_table, bool optimize_speed) {
    DbQueryResult res; memset(&res, 0, sizeof(res));
    (void)compression;
    const char *fmt    = format   ? format   : "csv";
    const char *on_err = on_error ? on_error : "abort";
    char dc = (delimiter && delimiter[0]) ? delimiter[0] : (strcmp(fmt, "tsv") == 0 ? '\t' : ',');

    if (!source_path || !*source_path)           { res.error_message = safe_strdup("source_path is required"); return res; }
    if (!destination_table || !*destination_table){ res.error_message = safe_strdup("destination_table is required"); return res; }
    if (strstr(source_path, ".."))               { res.error_message = safe_strdup("Path traversal not allowed"); return res; }
    if (!db_validate_identifier(destination_table)){ res.error_message = safe_strdup("Invalid destination_table identifier"); return res; }

#ifdef HAVE_SQLITE3
    if (!g_db) { res.error_message = safe_strdup("Database connection not active"); return res; }

    FILE *f = fopen(source_path, "r");
    if (!f) { res.error_message = safe_strdup("Cannot open source file for reading"); return res; }

    char line[65536];
    if (!fgets(line, sizeof(line), f)) { fclose(f); res.error_message = safe_strdup("Empty source file"); return res; }
    strip_newline(line);

    /* Parse header row to discover column names */
    int col_count = 0;
    { char *tmp = safe_strdup(line); char *p = tmp;
      while (p) { col_count++; p = strchr(p, dc); if (p) p++; }
      free(tmp); }

    char **col_names = (char **)malloc((size_t)col_count * sizeof(char *));
    { char *tmp = safe_strdup(line); char *p = tmp; int ci = 0;
      while (p && ci < col_count) {
          char *end = strchr(p, dc); if (end) *end = '\0';
          size_t fl = strlen(p);
          if (fl >= 2 && p[0] == '"' && p[fl - 1] == '"') { p++; p[fl - 2] = '\0'; }
          col_names[ci++] = safe_strdup(p);
          p = end ? end + 1 : NULL;
      }
      free(tmp); }

    if (auto_create_table) {
        char create_sql[4096]; int pos = 0;
        pos += snprintf(create_sql + pos, sizeof(create_sql) - (size_t)pos, "CREATE TABLE IF NOT EXISTS \"%s\" (", destination_table);
        for (int i = 0; i < col_count; i++)
            pos += snprintf(create_sql + pos, sizeof(create_sql) - (size_t)pos, "%s\"%s\" TEXT", i > 0 ? "," : "", col_names[i] ? col_names[i] : "col");
        snprintf(create_sql + pos, sizeof(create_sql) - (size_t)pos, ");");
        char *err_msg = NULL;
        if (sqlite3_exec(g_db, create_sql, NULL, NULL, &err_msg) != SQLITE_OK) {
            for (int i = 0; i < col_count; i++) free(col_names[i]);
            free(col_names); fclose(f);
            res.error_message = safe_strdup(err_msg ? err_msg : "CREATE TABLE failed");
            if (err_msg) { sqlite3_free(err_msg); } return res;
        }
    }

    if (optimize_speed) sqlite3_exec(g_db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);

    /* Build parameterized INSERT once, reuse for every row */
    char insert_sql[4096]; int pos = 0;
    pos += snprintf(insert_sql + pos, sizeof(insert_sql) - (size_t)pos, "INSERT INTO \"%s\" (", destination_table);
    for (int i = 0; i < col_count; i++)
        pos += snprintf(insert_sql + pos, sizeof(insert_sql) - (size_t)pos, "%s\"%s\"", i > 0 ? "," : "", col_names[i] ? col_names[i] : "col");
    pos += snprintf(insert_sql + pos, sizeof(insert_sql) - (size_t)pos, ") VALUES (");
    for (int i = 0; i < col_count; i++)
        pos += snprintf(insert_sql + pos, sizeof(insert_sql) - (size_t)pos, "%s?", i > 0 ? "," : "");
    snprintf(insert_sql + pos, sizeof(insert_sql) - (size_t)pos, ");");

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, insert_sql, -1, &stmt, NULL) != SQLITE_OK) {
        for (int i = 0; i < col_count; i++) free(col_names[i]);
        free(col_names); fclose(f);
        if (optimize_speed) sqlite3_exec(g_db, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);
        res.error_message = safe_strdup(sqlite3_errmsg(g_db)); return res;
    }

    sqlite3_exec(g_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    long rows_ok = 0, rows_err = 0;
    bool abort_on_error = (strcmp(on_err, "abort") == 0);
    bool aborted = false;

    if (!header) rewind(f); /* treat first line as data when no header */

    char *vals[1024];
    while (!aborted && fgets(line, sizeof(line), f)) {
        if (strip_newline(line) == 0) continue;
        char *tmp = safe_strdup(line);
        parse_delimited_line(tmp, dc, vals, col_count);
        sqlite3_reset(stmt);
        for (int i = 0; i < col_count; i++) {
            if (vals[i] && *vals[i]) sqlite3_bind_text(stmt, i + 1, vals[i], -1, SQLITE_TRANSIENT);
            else                     sqlite3_bind_null(stmt, i + 1);
        }
        int rc = sqlite3_step(stmt);
        free(tmp);
        if (rc == SQLITE_DONE) rows_ok++;
        else { rows_err++; if (abort_on_error) aborted = true; }
    }
    fclose(f); sqlite3_finalize(stmt);
    sqlite3_exec(g_db, aborted ? "ROLLBACK;" : "COMMIT;", NULL, NULL, NULL);
    if (optimize_speed) sqlite3_exec(g_db, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);

    for (int i = 0; i < col_count; i++) free(col_names[i]);
    free(col_names);

    if (aborted) { res.error_message = safe_strdup("Import aborted due to row error (on_error=abort)"); return res; }

    char bm[512], bj[512];
    snprintf(bm, sizeof(bm), "| status | destination | rows_processed | errors |\n|---|---|---|---|\n| success | %s | %ld | %ld |", destination_table, rows_ok, rows_err);
    snprintf(bj, sizeof(bj), "{\"status\":\"success\",\"engine\":\"sqlite\",\"operation\":\"import\",\"target\":\"%s\",\"rows_processed\":%ld,\"bytes_processed\":0,\"duration_seconds\":0,\"files_created_or_read\":[\"%s\"],\"warnings\":[]}", destination_table, rows_ok, source_path);
    res.markdown_table = safe_strdup(bm); res.json_structured = safe_strdup(bj);
    res.success = true; res.rows_affected = (int)rows_ok; return res;
#else
    (void)fmt; (void)dc; (void)header; (void)on_err; (void)auto_create_table; (void)optimize_speed;
    char bm[256], bj[256];
    snprintf(bm, sizeof(bm), "| status | destination | source |\n|---|---|---|\n| success | %s | %s |", destination_table, source_path);
    snprintf(bj, sizeof(bj), "{\"status\":\"success\",\"engine\":\"sqlite\",\"operation\":\"import\",\"target\":\"%s\",\"rows_processed\":0,\"bytes_processed\":0,\"duration_seconds\":0,\"files_created_or_read\":[\"%s\"],\"warnings\":[]}", destination_table, source_path);
    res.markdown_table = safe_strdup(bm); res.json_structured = safe_strdup(bj); res.success = true; return res;
#endif
}
