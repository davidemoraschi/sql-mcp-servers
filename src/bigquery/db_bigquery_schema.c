/* BigQuery schema, relationship, health, and ERD tools. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "bigquery_internal.h"
#include "sf_buf.h"

static void bqs_region(char *out, size_t cap) {
    const char *location=bq_location(); size_t i=0;
    if(!location)location="US";
    for(;location[i]&&i+1<cap;i++)out[i]=(char)tolower((unsigned char)location[i]);
    out[i]='\0';
}

DbQueryResult db_describe_table(const char *name) {
    char project[160],dataset[256],table[256],sql[1200];
    if(!bq_table_parts(name,project,sizeof(project),dataset,sizeof(dataset),table,sizeof(table)))
        return bq_fail_external("Use table, dataset.table, or project.dataset.table; table-only names require BIGQUERY_DATASET");
    snprintf(sql,sizeof(sql),"SELECT column_name,data_type,is_nullable,ordinal_position FROM `%s.%s.INFORMATION_SCHEMA.COLUMNS` WHERE table_name='%s' ORDER BY ordinal_position",project,dataset,table);
    return bq_run(sql);
}

DbQueryResult db_list_tables(void) {
    char region[128],sql[1024]; bqs_region(region,sizeof(region));
    snprintf(sql,sizeof(sql),"SELECT table_schema AS dataset_name,table_name,table_type,row_count FROM `%s.region-%s.INFORMATION_SCHEMA.TABLES` ORDER BY table_schema,table_name",bq_project_id(),region);
    return bq_run(sql);
}

DbQueryResult db_infer_relationships(void) {
    char region[128],sql[1024]; bqs_region(region,sizeof(region));
    snprintf(sql,sizeof(sql),"SELECT schema_name AS dataset_name FROM `%s.region-%s.INFORMATION_SCHEMA.SCHEMATA` ORDER BY schema_name",bq_project_id(),region);
    DbQueryResult datasets=bq_run(sql);
    if(!datasets.success)return datasets;
    JsonValue *names=json_parse(datasets.json_structured); db_free_result(&datasets);
    if(!names||names->type!=JSON_ARRAY){json_free(names);return bq_fail_external("Could not read BigQuery datasets");}
    SfBuf md={NULL,0,0,false},js={NULL,0,0,false}; size_t count=0;
    sf_buf_str(&md,"| source | foreign key | target |\n|---|---|---|"); sf_buf_str(&js,"[");
    for(size_t i=0;i<names->u.array.count;i++){
        const char *ds=json_string_value(json_object_get(names->u.array.items[i],"dataset_name"));
        if(!ds||!db_validate_identifier(ds))continue;
        char query[2048];
        snprintf(query,sizeof(query),
            "SELECT k.table_schema AS source_dataset,k.table_name AS source_table,k.column_name AS foreign_key,"
            "ccu.table_schema AS target_dataset,ccu.table_name AS target_table,ccu.column_name AS target_key "
            "FROM `%s.%s.INFORMATION_SCHEMA.KEY_COLUMN_USAGE` k "
            "JOIN `%s.%s.INFORMATION_SCHEMA.TABLE_CONSTRAINTS` tc "
            "USING(constraint_catalog,constraint_schema,constraint_name,table_catalog,table_schema,table_name) "
            "JOIN `%s.%s.INFORMATION_SCHEMA.CONSTRAINT_COLUMN_USAGE` ccu "
            "USING(constraint_catalog,constraint_schema,constraint_name) "
            "WHERE tc.constraint_type='FOREIGN KEY' ORDER BY source_table,k.ordinal_position",
            bq_project_id(),ds,bq_project_id(),ds,bq_project_id(),ds);
        DbQueryResult one=bq_run(query);
        if(!one.success){db_free_result(&one);continue;}
        JsonValue *items=json_parse(one.json_structured); db_free_result(&one);
        if(items&&items->type==JSON_ARRAY)for(size_t j=0;j<items->u.array.count;j++){
            const JsonValue *r=items->u.array.items[j];
            const char *sd=json_string_value(json_object_get(r,"source_dataset")),*st=json_string_value(json_object_get(r,"source_table"));
            const char *fk=json_string_value(json_object_get(r,"foreign_key")),*td=json_string_value(json_object_get(r,"target_dataset"));
            const char *tt=json_string_value(json_object_get(r,"target_table")),*tk=json_string_value(json_object_get(r,"target_key"));
            sf_buf_str(&md,"\n| ");sf_buf_printf(&md,"%s.%s",sd?sd:"",st?st:"");sf_buf_str(&md," | ");sf_buf_str(&md,fk?fk:"");sf_buf_str(&md," | ");
            sf_buf_printf(&md,"%s.%s.%s |",td?td:"",tt?tt:"",tk?tk:"");
            sf_buf_str(&js,count?",{":"{");
            const char *keys[]={"source_dataset","source_table","foreign_key","target_dataset","target_table","target_key"};
            for(int k=0;k<6;k++){const char *v=json_string_value(json_object_get(r,keys[k]));if(k)sf_buf_str(&js,",");sf_buf_str(&js,"\"");sf_buf_str(&js,keys[k]);sf_buf_str(&js,"\":\"");sf_buf_json_escaped(&js,v?v:"");sf_buf_str(&js,"\"");}
            sf_buf_str(&js,"}");count++;
        }
        json_free(items);
    }
    sf_buf_str(&js,"]");json_free(names);
    DbQueryResult out;memset(&out,0,sizeof(out));out.markdown_table=sf_buf_take(&md);out.json_structured=sf_buf_take(&js);out.success=out.markdown_table&&out.json_structured;out.rows_affected=(int)count;
    if(!out.success){free(out.markdown_table);free(out.json_structured);return bq_fail_external("Out of memory");}return out;
}

DbQueryResult db_check_health(void) {
    return bq_run("SELECT @@project_id AS project_id,SESSION_USER() AS user,CURRENT_DATE() AS server_date,CURRENT_TIMESTAMP() AS server_time");
}

DbQueryResult db_generate_erd_plantuml(void) {
    char region[128],sql[1024]; bqs_region(region,sizeof(region));
    snprintf(sql,sizeof(sql),"SELECT table_schema AS dataset_name,table_name,column_name,data_type FROM `%s.region-%s.INFORMATION_SCHEMA.COLUMNS` ORDER BY table_schema,table_name,ordinal_position",bq_project_id(),region);
    DbQueryResult cols=bq_run(sql); if(!cols.success)return cols;
    JsonValue *rows=json_parse(cols.json_structured); db_free_result(&cols);
    if(!rows||rows->type!=JSON_ARRAY){json_free(rows);return bq_fail_external("Could not parse BigQuery schema rows");}
    DbQueryResult rels=db_infer_relationships(); JsonValue *keys=rels.success?json_parse(rels.json_structured):NULL; db_free_result(&rels);
    SfBuf uml={NULL,0,0,false},md={NULL,0,0,false},js={NULL,0,0,false};
    sf_buf_str(&uml,"@startuml\n"); char prev[512]="";
    for(size_t i=0;i<rows->u.array.count;i++){
        const JsonValue *r=rows->u.array.items[i];
        const char *ds=json_string_value(json_object_get(r,"dataset_name")),*tb=json_string_value(json_object_get(r,"table_name"));
        const char *col=json_string_value(json_object_get(r,"column_name")),*type=json_string_value(json_object_get(r,"data_type"));
        char full[512]; snprintf(full,sizeof(full),"%s.%s",ds?ds:"",tb?tb:"");
        if(strcmp(prev,full)){if(prev[0])sf_buf_str(&uml,"}\n");sf_buf_printf(&uml,"entity \"%s\" {\n",full);snprintf(prev,sizeof(prev),"%s",full);}
        sf_buf_printf(&uml,"  %s : %s\n",col?col:"column",type?type:"unknown");
    }
    if(prev[0])sf_buf_str(&uml,"}\n");
    if(keys&&keys->type==JSON_ARRAY)for(size_t i=0;i<keys->u.array.count;i++){
        const JsonValue *r=keys->u.array.items[i];
        const char *sd=json_string_value(json_object_get(r,"source_dataset")),*st=json_string_value(json_object_get(r,"source_table"));
        const char *fk=json_string_value(json_object_get(r,"foreign_key")),*td=json_string_value(json_object_get(r,"target_dataset"));
        const char *tt=json_string_value(json_object_get(r,"target_table")); char src[512],dst[512];
        snprintf(src,sizeof(src),"%s.%s",sd?sd:"",st?st:"");snprintf(dst,sizeof(dst),"%s.%s",td?td:"",tt?tt:"");
        sf_buf_printf(&uml,"\"%s\" ||--o{ \"%s\" : %s\n",dst,src,fk?fk:"fk");
    }
    sf_buf_str(&uml,"@enduml\n");sf_buf_str(&md,"```plantuml\n");if(uml.p)sf_buf_str(&md,uml.p);sf_buf_str(&md,"```");
    sf_buf_str(&js,"{\"format\":\"plantuml\",\"schema_erd\":\"");if(uml.p)sf_buf_json_escaped(&js,uml.p);sf_buf_str(&js,"\"}");
    json_free(rows);json_free(keys);sf_buf_free(&uml);
    DbQueryResult out;memset(&out,0,sizeof(out));out.markdown_table=sf_buf_take(&md);out.json_structured=sf_buf_take(&js);
    out.success=out.markdown_table&&out.json_structured;
    if(!out.success){free(out.markdown_table);free(out.json_structured);return bq_fail_external("Out of memory");}
    return out;
}
