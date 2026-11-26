/* BigQuery CSV/TSV import and local export helpers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "db_bigquery.h"
#include "bigquery_internal.h"
#include "json_rpc.h"
#include "sf_buf.h"

static char *bqt_dup(const char *s) {
    size_t n = strlen(s) + 1; char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n); return p;
}

static void bqt_write_cell(FILE *f, const char *s, char delimiter) {
    bool quote = strchr(s, delimiter) || strchr(s, '"') || strchr(s, '\n') || strchr(s, '\r');
    if (quote) fputc('"', f);
    for (; *s; s++) { if (quote && *s == '"') fputc('"', f); fputc(*s, f); }
    if (quote) fputc('"', f);
}

static void bqt_parse_line(char *line, char delimiter, char **values, size_t n) {
    char *p = line;
    for (size_t i = 0; i < n; i++) {
        char *end = strchr(p, delimiter);
        if (end) *end = '\0';
        size_t len = strlen(p);
        if (len >= 2 && p[0] == '"' && p[len - 1] == '"') { p++; p[len - 2] = '\0'; }
        values[i] = p;
        p = end ? end + 1 : p + strlen(p);
        if (!end) { for (++i; i < n; i++) values[i] = (char *)""; break; }
    }
}

static void bqt_strip_line(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

static bool bqt_valid_column(const char *s) {
    if (!s || !*s || (!isalpha((unsigned char)*s) && *s != '_')) return false;
    for (s++; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_') return false;
    return true;
}

static bool bqt_table_path(const char *name, char *path, size_t size,
                           char *project, size_t ps, char *dataset, size_t ds, char *table, size_t ts) {
    if (!bq_table_parts(name, project, ps, dataset, ds, table, ts)) return false;
    snprintf(path, size, "`%s.%s.%s`", project, dataset, table);
    return true;
}

static char *bqt_url_encode(const char *s) {
    static const char hex[] = "0123456789ABCDEF"; size_t n = strlen(s);
    char *out = (char *)malloc(n * 3 + 1); if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[j++] = (char)c;
        else { out[j++] = '%'; out[j++] = hex[c >> 4]; out[j++] = hex[c & 15]; }
    }
    out[j] = '\0'; return out;
}

typedef struct { char *line; char *values[1024]; } BqtRow;

static bool bqt_insert_batch(const char *project, const char *dataset, const char *table,
                             char **columns, BqtRow *rows, size_t row_count, size_t column_count,
                             bool skip_invalid, long *inserted, long *rejected,
                             char *err, size_t err_size) {
    char *p = bqt_url_encode(project), *d = bqt_url_encode(dataset), *t = bqt_url_encode(table);
    if (!p || !d || !t) { free(p); free(d); free(t); snprintf(err, err_size, "Out of memory"); return false; }
    char path[1024]; snprintf(path, sizeof(path), "/bigquery/v2/projects/%s/datasets/%s/tables/%s/insertAll", p, d, t);
    free(p); free(d); free(t);
    SfBuf body = {NULL,0,0,false};
    sf_buf_printf(&body,"{\"skipInvalidRows\":%s,\"rows\":[",skip_invalid?"true":"false");
    for (size_t r = 0; r < row_count; r++) {
        if (r) sf_buf_str(&body, ",");
        sf_buf_str(&body, "{\"json\":{");
        for (size_t c = 0; c < column_count; c++) {
            if (c) sf_buf_str(&body, ",");
            sf_buf_str(&body, "\""); sf_buf_json_escaped(&body, columns[c]);
            sf_buf_str(&body, "\":\""); sf_buf_json_escaped(&body, rows[r].values[c]); sf_buf_str(&body, "\"");
        }
        sf_buf_str(&body, "}}");
    }
    sf_buf_str(&body, "]}"); char *json = sf_buf_take(&body);
    if (!json) { snprintf(err, err_size, "Out of memory"); return false; }

    JsonValue *root = bq_api_json("POST", path, json, err, err_size); free(json);
    if (!root) return false;
    const JsonValue *errors = json_object_get(root, "insertErrors");
    if (errors && errors->type == JSON_ARRAY && errors->u.array.count) {
        *rejected = 0;
        for (size_t i=0; i<errors->u.array.count; i++) {
            const JsonValue *entry=errors->u.array.items[i];
            const JsonValue *index=json_object_get(entry,"index");
            if (index && index->type==JSON_NUMBER) (*rejected)++;
            const JsonValue *reason=json_object_get(entry,"errors");
            const char *message=reason&&reason->type==JSON_ARRAY&&reason->u.array.count
                ? json_string_value(json_object_get(reason->u.array.items[0],"message")):NULL;
            if (i==0) snprintf(err,err_size,"BigQuery rejected an imported row: %s",message?message:"insertErrors returned");
        }
        *inserted=(long)row_count-*rejected;
        json_free(root);
        return skip_invalid;
    }
    *inserted=(long)row_count; *rejected=0; json_free(root); return true;
}

DbQueryResult db_export_data(const char *target, const char *destination_path, const char *format,
                             const char *compression, bool header, const char *delimiter) {
    (void)compression;
    if (!target || !*target || !destination_path || !*destination_path) return bq_fail_external("target and destination_path are required");
    const char *fmt = format ? format : "csv";
    if (strcmp(fmt,"csv") && strcmp(fmt,"tsv") && strcmp(fmt,"json"))
        return bq_fail_external("BigQuery export supports CSV, TSV, and JSON (SQL dump is not supported)");
    char sql[2048];
    if (!strncmp(target, "SELECT", 6) || !strncmp(target, "select", 6) ||
        !strncmp(target, "WITH", 4) || !strncmp(target, "with", 4)) snprintf(sql, sizeof(sql), "%s", target);
    else {
        char fq[768], project[160], dataset[256], table[256];
        if (!bqt_table_path(target, fq, sizeof(fq), project, sizeof(project), dataset, sizeof(dataset), table, sizeof(table)))
            return bq_fail_external("Use table, dataset.table, or project.dataset.table for export");
        snprintf(sql, sizeof(sql), "SELECT * FROM %s", fq);
    }
    DbQueryResult data = bq_run(sql);
    if (!data.success) return data;
    int exported_rows = data.rows_affected;
    FILE *f = fopen(destination_path, "wb");
    if (!f) { db_free_result(&data); return bq_fail_external("Cannot open destination file for writing"); }
    if (strcmp(fmt, "json") == 0) fputs(data.json_structured ? data.json_structured : "[]", f);
    else {
        JsonValue *rows = json_parse(data.json_structured ? data.json_structured : "[]");
        if (!rows || rows->type != JSON_ARRAY) { json_free(rows); fclose(f); db_free_result(&data); return bq_fail_external("Could not parse query results for export"); }
        char dc = delimiter && delimiter[0] ? delimiter[0] : (strcmp(fmt, "tsv") == 0 ? '\t' : ',');
        const JsonValue *first = rows->u.array.count ? rows->u.array.items[0] : NULL;
        if (header && first && first->type == JSON_OBJECT) {
            for (size_t c=0; c<first->u.object.count; c++) { if (c) fputc(dc,f); bqt_write_cell(f, first->u.object.keys[c], dc); }
            fputc('\n', f);
        }
        for (size_t r=0; r<rows->u.array.count; r++) {
            const JsonValue *row=rows->u.array.items[r];
            if (!row || row->type!=JSON_OBJECT) continue;
            for (size_t c=0; c<row->u.object.count; c++) {
                const JsonValue *v=row->u.object.values[c];
                if (c) fputc(dc,f);
                if (v && v->type==JSON_STRING) bqt_write_cell(f,v->u.string,dc);
                else if (v && v->type==JSON_NUMBER) fprintf(f,"%.15g",v->u.number);
                else if (v && v->type==JSON_BOOL) fputs(v->u.boolean?"true":"false",f);
            }
            fputc('\n',f);
        }
        json_free(rows);
    }
    fclose(f);
    DbQueryResult out; memset(&out,0,sizeof(out)); SfBuf md={NULL,0,0,false},js={NULL,0,0,false};
    sf_buf_printf(&md,"Exported up to %d rows to %s.",data.rows_affected,destination_path);
    sf_buf_str(&js,"{\"status\":\"success\",\"engine\":\"bigquery\",\"operation\":\"export\",\"rows_processed\":");
    sf_buf_printf(&js,"%d,\"files_created_or_read\":[\"",data.rows_affected); sf_buf_json_escaped(&js,destination_path); sf_buf_str(&js,"\"]}");
    out.markdown_table=sf_buf_take(&md); out.json_structured=sf_buf_take(&js); out.success=out.markdown_table&&out.json_structured;
    db_free_result(&data); if (!out.success) { free(out.markdown_table); free(out.json_structured); return bq_fail_external("Out of memory"); }
    out.rows_affected=exported_rows; return out;
}

DbQueryResult db_import_data(const char *source_path, const char *destination_table, const char *format,
                             const char *compression, bool header, const char *delimiter,
                             const char *on_error, bool auto_create_table, bool optimize_speed) {
    (void)optimize_speed;
    const char *fmt = format ? format : "csv";
    if (bq_is_read_only()) return bq_fail_external("Server is READONLY (BIGQUERY_ACCESS_MODE)");
    if (!source_path || !*source_path || !destination_table || !*destination_table) return bq_fail_external("source_path and destination_table are required");
    if (compression && *compression && strcmp(compression,"none")) return bq_fail_external("Compressed BigQuery imports are not supported");
    if (strcmp(fmt,"csv") && strcmp(fmt,"tsv")) return bq_fail_external("BigQuery import supports CSV and TSV files");
    char fq[768], project[160], dataset[256], table[256];
    if (!bqt_table_path(destination_table,fq,sizeof(fq),project,sizeof(project),dataset,sizeof(dataset),table,sizeof(table)))
        return bq_fail_external("Use dataset.table or project.dataset.table for import; a table-only name requires BIGQUERY_DATASET");
    FILE *f=fopen(source_path,"rb"); if (!f) return bq_fail_external("Cannot open source file for reading");
    char line[65536]; if (!fgets(line,sizeof(line),f)) { fclose(f); return bq_fail_external("Empty source file"); }
    bqt_strip_line(line); char dc=delimiter&&delimiter[0]?delimiter[0]:(strcmp(fmt,"tsv")==0?'\t':',');
    size_t col_count=1; for (char *p=line;*p;p++) if (*p==dc) col_count++;
    if (col_count>1024) { fclose(f); return bq_fail_external("Import supports at most 1024 columns"); }
    char **columns=(char **)calloc(col_count,sizeof(char *));
    if (!columns) { fclose(f); return bq_fail_external("Out of memory"); }
    char *header_copy=bqt_dup(line); char *header_vals[1024];
    if(!header_copy){free(columns);fclose(f);return bq_fail_external("Out of memory");}
    bqt_parse_line(header_copy,dc,header_vals,col_count);
    if (header) {
        for (size_t c=0;c<col_count;c++) {
            if (auto_create_table && !bqt_valid_column(header_vals[c])) { for(size_t j=0;j<c;j++)free(columns[j]); free(header_copy); free(columns); fclose(f); return bq_fail_external("auto_create_table requires simple identifier names in the CSV header"); }
            columns[c]=bqt_dup(header_vals[c]);
            if(!columns[c]){for(size_t j=0;j<c;j++)free(columns[j]);free(header_copy);free(columns);fclose(f);return bq_fail_external("Out of memory");}
        }
    } else {
        DbQueryResult schema=db_describe_table(destination_table);
        if (!schema.success) { free(header_copy); free(columns); fclose(f); return schema; }
        JsonValue *schema_rows=json_parse(schema.json_structured); db_free_result(&schema);
        if (!schema_rows || schema_rows->type!=JSON_ARRAY || schema_rows->u.array.count<col_count) { json_free(schema_rows); free(header_copy); free(columns); fclose(f); return bq_fail_external("Could not read destination table columns"); }
        for (size_t c=0;c<col_count;c++) {
            const char *col=json_string_value(json_object_get(schema_rows->u.array.items[c],"column_name"));
            columns[c]=col?bqt_dup(col):NULL;
            if(!columns[c]){for(size_t j=0;j<c;j++)free(columns[j]);json_free(schema_rows);free(header_copy);free(columns);fclose(f);return bq_fail_external("Could not read destination table columns");}
        }
        json_free(schema_rows); rewind(f);
    }
    free(header_copy);
    if (auto_create_table) {
        if (!header) { for(size_t c=0;c<col_count;c++) free(columns[c]); free(columns); fclose(f); return bq_fail_external("auto_create_table requires a CSV header row"); }
        SfBuf create={NULL,0,0,false}; sf_buf_printf(&create,"CREATE TABLE IF NOT EXISTS %s (",fq);
        for(size_t c=0;c<col_count;c++){ if(c)sf_buf_str(&create,","); sf_buf_str(&create,"`"); sf_buf_str(&create,columns[c]); sf_buf_str(&create,"` STRING"); }
        sf_buf_str(&create,")"); char *sql=sf_buf_take(&create);
        if (!sql) { for(size_t c=0;c<col_count;c++)free(columns[c]); free(columns); fclose(f); return bq_fail_external("Out of memory"); }
        DbQueryResult created=bq_run(sql); free(sql);
        if (!created.success) { for(size_t c=0;c<col_count;c++)free(columns[c]); free(columns); fclose(f); return created; }
        db_free_result(&created);
    }
    if (header) { /* consume the already-read header before the data loop */ }
    else rewind(f);
    long rows=0, errors=0; bool continue_on_error=on_error&&strcmp(on_error,"continue")==0;
    char err[512]=""; BqtRow batch[10]; memset(batch,0,sizeof(batch)); size_t batch_count=0; bool failed=false;
    while (fgets(line,sizeof(line),f)) {
        bqt_strip_line(line); if (!line[0]) continue;
        batch[batch_count].line=bqt_dup(line);
        if (!batch[batch_count].line) { snprintf(err,sizeof(err),"Out of memory"); failed=true; break; }
        bqt_parse_line(batch[batch_count].line,dc,batch[batch_count].values,col_count);
        batch_count++;
        if (batch_count==sizeof(batch)/sizeof(batch[0])) {
            long inserted=0,rejected=0;
            if(!bqt_insert_batch(project,dataset,table,columns,batch,batch_count,col_count,continue_on_error,&inserted,&rejected,err,sizeof(err))) { errors+=rejected?rejected:1; failed=true; }
            else { rows+=inserted; errors+=rejected; }
            for(size_t i=0;i<batch_count;i++)free(batch[i].line);
            memset(batch,0,sizeof(batch));batch_count=0;
            if(failed)break;
        }
    }
    if(!failed&&batch_count){long inserted=0,rejected=0;
        if(!bqt_insert_batch(project,dataset,table,columns,batch,batch_count,col_count,continue_on_error,&inserted,&rejected,err,sizeof(err))) {errors+=rejected?rejected:1;failed=true;}
        else {rows+=inserted;errors+=rejected;}
    }
    for(size_t i=0;i<batch_count;i++)free(batch[i].line);
    fclose(f); for(size_t c=0;c<col_count;c++)free(columns[c]); free(columns);
    if (failed) return bq_fail_external(err[0]?err:"BigQuery import failed");
    DbQueryResult out; memset(&out,0,sizeof(out)); SfBuf md={NULL,0,0,false},js={NULL,0,0,false};
    sf_buf_printf(&md,"Imported %ld rows into %s (%ld row errors).",rows,destination_table,errors);
    sf_buf_str(&js,"{\"status\":\"success\",\"engine\":\"bigquery\",\"operation\":\"import\",\"target\":\""); sf_buf_json_escaped(&js,destination_table);
    sf_buf_printf(&js,"\",\"rows_processed\":%ld,\"files_created_or_read\":[\"",rows); sf_buf_json_escaped(&js,source_path); sf_buf_str(&js,"\"],\"warnings\":[]}");
    out.markdown_table=sf_buf_take(&md); out.json_structured=sf_buf_take(&js); out.rows_affected=(int)rows;
    out.success=out.markdown_table&&out.json_structured; if(!out.success){free(out.markdown_table);free(out.json_structured);return bq_fail_external("Out of memory");} return out;
}
