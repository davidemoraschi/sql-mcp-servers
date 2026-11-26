#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "db_athena.h"
#include "athena_internal.h"
#include "aws_credentials.h"
#include "aws_sigv4.h"
#include "aws_sha256.h"
#include "sf_http.h"
#include "sf_buf.h"

#define ATHENA_CT "application/x-amz-json-1.1"
#define ATHENA_MAX_ROWS 1000
typedef struct {char*region,*database,*catalog,*workgroup,*output,*host;AwsCredentials creds;bool readonly,ready;int timeout;} AthenaSession;
static AthenaSession g;
static char*dupstr(const char*s){if(!s)return NULL;size_t n=strlen(s)+1;char*p=(char*)malloc(n);if(p)memcpy(p,s,n);return p;}
static DbQueryResult fail(const char*m){DbQueryResult r;memset(&r,0,sizeof(r));r.error_message=dupstr(m?m:"Athena operation failed");return r;}
DbQueryResult athena_fail(const char*m){return fail(m);}
void db_free_result(DbQueryResult*r){if(!r)return;free(r->markdown_table);free(r->json_structured);free(r->error_message);memset(r,0,sizeof(*r));}
const char*athena_database(void){return g.database;}const char*athena_catalog(void){return g.catalog;}bool athena_is_read_only(void){return g.readonly;}
bool db_validate_identifier(const char*s){int parts=0;if(!s||!*s)return false;while(*s){if(!isalnum((unsigned char)*s)&&*s!='_')return false;while(isalnum((unsigned char)*s)||*s=='_'||*s=='-')s++;parts++;if(!*s)break;if(*s++!='.'||parts==3||!*s)return false;}return parts>=1&&parts<=3;}

static JsonValue*call(const char*operation,const char*body,char*err,size_t cap){char target[128],*headers=NULL;SfHttpRequest rq;SfHttpResponse res;snprintf(target,sizeof(target),"AmazonAthena.%s",operation);
 if(!aws_sigv4_headers(&g.creds,g.region,"athena",g.host,target,ATHENA_CT,body,&headers,err,cap))return NULL;
 memset(&rq,0,sizeof(rq));rq.method="POST";rq.host=g.host;rq.path_and_query="/";rq.json_body=body;rq.content_type=ATHENA_CT;rq.auth_header=headers;rq.timeout_ms=g.timeout*1000;
 bool sent=sf_http_request(&rq,&res);free(headers);if(!sent){snprintf(err,cap,"Athena HTTPS request failed: %s",res.error?res.error:"transport error");sf_http_free_response(&res);return NULL;}
 JsonValue*root=res.body?json_parse(res.body):NULL;if(res.status<200||res.status>=300){const char*m=root?json_string_value(json_object_get(root,"Message")):NULL;if(!m&&root)m=json_string_value(json_object_get(root,"message"));const char*t=root?json_string_value(json_object_get(root,"__type")):NULL;snprintf(err,cap,"Athena HTTP %d%s%s: %s",res.status,t?" ":"",t?t:"",m?m:"request failed");json_free(root);sf_http_free_response(&res);return NULL;}
 sf_http_free_response(&res);if(!root)snprintf(err,cap,"Athena returned invalid JSON");return root;}

static void sleep_half(void){
#ifdef _WIN32
 Sleep(500);
#else
 struct timespec t={0,500000000L};nanosleep(&t,NULL);
#endif
}
static void request_token(const char*sql,char out[65]){static unsigned long counter=0;char seed[512];unsigned char hash[32];snprintf(seed,sizeof(seed),"%lld:%lu:%lu:%p:%.400s",(long long)time(NULL),(unsigned long)clock(),++counter,(void*)&counter,sql);aws_sha256((const unsigned char*)seed,strlen(seed),hash);aws_hex(hash,32,out);}
static char*start_query(const char*sql,char*err,size_t cap){SfBuf b={0};char token[65];request_token(sql,token);sf_buf_str(&b,"{\"ClientRequestToken\":\"");sf_buf_str(&b,token);sf_buf_str(&b,"\",\"QueryString\":\"");sf_buf_json_escaped(&b,sql);sf_buf_str(&b,"\",\"QueryExecutionContext\":{\"Database\":\"");sf_buf_json_escaped(&b,g.database);sf_buf_str(&b,"\",\"Catalog\":\"");sf_buf_json_escaped(&b,g.catalog);sf_buf_str(&b,"\"},\"WorkGroup\":\"");sf_buf_json_escaped(&b,g.workgroup);sf_buf_str(&b,"\"");if(g.output&&*g.output){sf_buf_str(&b,",\"ResultConfiguration\":{\"OutputLocation\":\"");sf_buf_json_escaped(&b,g.output);sf_buf_str(&b,"\"}");}sf_buf_str(&b,"}");char*body=sf_buf_take(&b);if(!body){snprintf(err,cap,"Out of memory");return NULL;}JsonValue*root=call("StartQueryExecution",body,err,cap);free(body);if(!root)return NULL;const char*id=json_string_value(json_object_get(root,"QueryExecutionId"));char*out=id?dupstr(id):NULL;json_free(root);if(!out)snprintf(err,cap,"Athena returned no query execution ID");return out;}
static bool wait_query(const char*id,char*err,size_t cap){char body[512];snprintf(body,sizeof(body),"{\"QueryExecutionId\":\"%s\"}",id);for(int i=0;i<g.timeout*2;i++){JsonValue*root=call("GetQueryExecution",body,err,cap);if(!root)return false;const JsonValue*q=json_object_get(root,"QueryExecution"),*st=json_object_get(q,"Status");const char*state=json_string_value(json_object_get(st,"State"));if(state&&!strcmp(state,"SUCCEEDED")){json_free(root);return true;}if(state&&(!strcmp(state,"FAILED")||!strcmp(state,"CANCELLED"))){const char*why=json_string_value(json_object_get(st,"StateChangeReason"));snprintf(err,cap,"Athena query %s: %s",state,why?why:"no reason returned");json_free(root);return false;}json_free(root);sleep_half();}snprintf(err,cap,"Athena query timed out after %d seconds",g.timeout);return false;}
static void mdcell(SfBuf*b,const char*s){for(;s&&*s;s++){if(*s=='|')sf_buf_str(b,"\\|");else if((unsigned char)*s<32)sf_buf_str(b," ");else sf_buf_add(b,s,1);}}
static bool append_results(const JsonValue*root,SfBuf*md,SfBuf*js,size_t*shown,bool*header,bool first_page){const JsonValue*rs=json_object_get(root,"ResultSet"),*meta=json_object_get(rs,"ResultSetMetadata"),*cols=json_object_get(meta,"ColumnInfo"),*rows=json_object_get(rs,"Rows");size_t n=cols&&cols->type==JSON_ARRAY?cols->u.array.count:0;if(!*header){sf_buf_str(md,"|");for(size_t c=0;c<n;c++){const char*name=json_string_value(json_object_get(cols->u.array.items[c],"Name"));sf_buf_str(md," ");mdcell(md,name?name:"column");sf_buf_str(md," |");}sf_buf_str(md,"\n|");for(size_t c=0;c<n;c++)sf_buf_str(md,"---|");sf_buf_str(js,"[");*header=true;}if(!rows||rows->type!=JSON_ARRAY)return true;
 for(size_t r=0;r<rows->u.array.count&&*shown<ATHENA_MAX_ROWS;r++){const JsonValue*data=json_object_get(rows->u.array.items[r],"Data");if(first_page&&r==0)continue;sf_buf_str(md,"\n|");sf_buf_str(js,*shown?",{":"{");for(size_t c=0;c<n;c++){const char*name=json_string_value(json_object_get(cols->u.array.items[c],"Name"));const JsonValue*cell=data&&data->type==JSON_ARRAY&&c<data->u.array.count?data->u.array.items[c]:NULL;const char*v=json_string_value(json_object_get(cell,"VarCharValue"));sf_buf_str(md," ");mdcell(md,v?v:"NULL");sf_buf_str(md," |");if(c)sf_buf_str(js,",");sf_buf_str(js,"\"");sf_buf_json_escaped(js,name?name:"column");sf_buf_str(js,"\":");if(v){sf_buf_str(js,"\"");sf_buf_json_escaped(js,v);sf_buf_str(js,"\"");}else sf_buf_str(js,"null");}sf_buf_str(js,"}");(*shown)++;}return !md->oom&&!js->oom;}

DbQueryResult athena_run(const char*sql){char err[2048]="";char*id=start_query(sql,err,sizeof(err));if(!id)return fail(err);if(!wait_query(id,err,sizeof(err))){free(id);return fail(err);}SfBuf md={0},js={0};char*token=NULL;size_t shown=0;bool header=false,ok=true,first=true;
 do{SfBuf b={0};sf_buf_str(&b,"{\"QueryExecutionId\":\"");sf_buf_json_escaped(&b,id);sf_buf_str(&b,"\",\"MaxResults\":1000");if(token){sf_buf_str(&b,",\"NextToken\":\"");sf_buf_json_escaped(&b,token);sf_buf_str(&b,"\"");}sf_buf_str(&b,"}");free(token);token=NULL;char*body=sf_buf_take(&b);JsonValue*root=body?call("GetQueryResults",body,err,sizeof(err)):NULL;free(body);if(!root){ok=false;break;}ok=append_results(root,&md,&js,&shown,&header,first);first=false;const char*next=json_string_value(json_object_get(root,"NextToken"));token=next?dupstr(next):NULL;json_free(root);}while(ok&&token&&shown<ATHENA_MAX_ROWS);free(token);free(id);if(header)sf_buf_str(&js,"]");else{sf_buf_str(&md,"Query succeeded with no result rows.");sf_buf_str(&js,"[]");}if(!ok||md.oom||js.oom){sf_buf_free(&md);sf_buf_free(&js);return fail(err[0]?err:"Out of memory");}DbQueryResult out;memset(&out,0,sizeof(out));out.markdown_table=sf_buf_take(&md);out.json_structured=sf_buf_take(&js);out.success=out.markdown_table&&out.json_structured;out.rows_affected=(int)shown;if(!out.success){db_free_result(&out);return fail("Out of memory");}return out;}

static const char*skip_noise(const char*p){for(;;){while(isspace((unsigned char)*p))p++;if(p[0]=='-'&&p[1]=='-'){while(*p&&*p!='\n')p++;}else if(p[0]=='/'&&p[1]=='*'){p+=2;while(*p&&!(p[0]=='*'&&p[1]=='/'))p++;if(*p)p+=2;}else return p;}}
static bool read_sql(const char*s){const char*p=skip_noise(s);char w[24];size_t n=0;while(isalpha((unsigned char)*p)&&n+1<sizeof(w))w[n++]=(char)toupper((unsigned char)*p++);w[n]=0;return !strcmp(w,"SELECT")||!strcmp(w,"WITH")||!strcmp(w,"SHOW")||!strcmp(w,"DESCRIBE")||!strcmp(w,"EXPLAIN");}
DbQueryResult db_execute_read(const char*s,const char*p){(void)p;if(!s||!*s)return fail("SQL query cannot be empty");if(!read_sql(s))return fail("read_query accepts SELECT, WITH, SHOW, DESCRIBE, or EXPLAIN; use write_query for modifications");return athena_run(s);}
DbQueryResult db_execute_write(const char*s,const char*p){(void)p;if(!s||!*s)return fail("SQL query cannot be empty");if(g.readonly)return fail("Server is READONLY (ATHENA_ACCESS_MODE)");return athena_run(s);}
DbQueryResult db_backup(const char*p){(void)p;return fail("Athena does not support database backup");}
DbQueryResult db_explain(const char*s){if(!s||!*s)return fail("SQL query cannot be empty");SfBuf b={0};sf_buf_str(&b,"EXPLAIN ");sf_buf_str(&b,s);char*q=sf_buf_take(&b);DbQueryResult r=q?athena_run(q):fail("Out of memory");free(q);return r;}

bool athena_db_init(const AthenaConfig*c,char*err,size_t cap){memset(&g,0,sizeof(g));if(!c||!c->region||!*c->region){snprintf(err,cap,"ATHENA_REGION or AWS_REGION is required");return false;}g.region=dupstr(c->region);g.database=dupstr(c->database&&*c->database?c->database:"default");g.catalog=dupstr(c->catalog&&*c->catalog?c->catalog:"AwsDataCatalog");g.workgroup=dupstr(c->workgroup&&*c->workgroup?c->workgroup:"primary");g.output=dupstr(c->output_location);size_t n=strlen(c->region)+32;g.host=(char*)malloc(n);if(g.host)snprintf(g.host,n,"athena.%s.amazonaws.com",c->region);g.readonly=c->read_only;g.timeout=c->timeout_seconds>0?c->timeout_seconds:300;if(!g.region||!g.database||!g.catalog||!g.workgroup||!g.host){snprintf(err,cap,"Out of memory");athena_db_close();return false;}if(!aws_credentials_load(c->profile,&g.creds,err,cap)){athena_db_close();return false;}g.ready=true;return true;}
void athena_db_close(void){free(g.region);free(g.database);free(g.catalog);free(g.workgroup);free(g.output);free(g.host);aws_credentials_free(&g.creds);memset(&g,0,sizeof(g));}
