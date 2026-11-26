#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "db_athena.h"
#include "rdbms_tools.h"
#include "mcp_loop.h"
static const char*envv(const char*n){const char*v=getenv(n);return v&&*v?v:NULL;}
static int envi(const char*n,int d){const char*v=envv(n);return v?atoi(v):d;}
static bool readonly(void){const char*v=envv("ATHENA_ACCESS_MODE");char b[32];size_t i=0;if(!v||strlen(v)>=sizeof(b))return false;for(;v[i];i++)b[i]=(char)toupper((unsigned char)v[i]);b[i]=0;return !strcmp(b,"READONLY")||!strcmp(b,"READ_ONLY");}
int main(void){mcp_configure_stdio();fprintf(stderr,"[INFO] Starting AWS Athena MCP Server (SigV4 / AWS JSON API)...\n");McpProfile p={"athena-mcp-server","AWS Athena","Check Athena API connectivity, catalog, database, workgroup, and region",false};tools_set_profile(&p);AthenaConfig c;memset(&c,0,sizeof(c));c.region=envv("ATHENA_REGION");if(!c.region)c.region=envv("AWS_REGION");if(!c.region)c.region=envv("AWS_DEFAULT_REGION");c.database=envv("ATHENA_DATABASE");c.catalog=envv("ATHENA_CATALOG");c.workgroup=envv("ATHENA_WORKGROUP");c.output_location=envv("ATHENA_OUTPUT_LOCATION");c.profile=envv("AWS_PROFILE");c.read_only=readonly();c.timeout_seconds=envi("ATHENA_TIMEOUT_SECONDS",300);char err[1024];if(!athena_db_init(&c,err,sizeof(err))){fprintf(stderr,"[FATAL] Athena configuration error: %s\n",err);return EXIT_FAILURE;}fprintf(stderr,"[INFO] Athena region %s, catalog %s, database %s, workgroup %s (%s).\n",c.region,c.catalog?c.catalog:"AwsDataCatalog",c.database?c.database:"default",c.workgroup?c.workgroup:"primary",c.read_only?"READONLY":"READWRITE");int rc=mcp_serve_stdio();athena_db_close();fprintf(stderr,"[INFO] AWS Athena MCP Server stopped.\n");return rc;}
