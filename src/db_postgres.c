/*
 * db_postgres.c - PostgreSQL wire-protocol backend (no libpq)
 *
 * Connects via TCP or Unix Domain Socket, speaks PostgreSQL
 * Frontend/Backend Protocol version 3.0.
 *
 * Line budget constraint: < 500 lines
 */
#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdbool.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET pg_sock_t;
  #define PG_INVALID_SOCK INVALID_SOCKET
  #define pg_close_sock(s) closesocket(s)
#else
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int pg_sock_t;
  #define PG_INVALID_SOCK (-1)
  #define pg_close_sock(s) close(s)
#endif

#include "db_postgres.h"
#include "pg_md5.h"
#include "pg_scram.h"
#include "sf_buf.h"

static pg_sock_t g_sock      = PG_INVALID_SOCK;
static bool      g_read_only = false;
static bool      g_ready     = false;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
static char *pg_strdup(const char *s) {
    if (!s) return NULL; size_t n=strlen(s)+1; char *d=(char*)malloc(n); if (d) memcpy(d,s,n); return d;
}
static DbQueryResult pg_fail(const char *msg) {
    DbQueryResult r; memset(&r,0,sizeof(r)); r.error_message=pg_strdup(msg); return r;
}
void db_free_result(DbQueryResult *res) {
    if (!res) return; free(res->markdown_table); free(res->json_structured); free(res->error_message); memset(res,0,sizeof(DbQueryResult));
}
bool db_validate_identifier(const char *id) {
    if (!id||!*id) return false;
    if (!isalpha((unsigned char)*id)&&*id!='_') return false;
    for (const char *p=id+1;*p;p++) if (!isalnum((unsigned char)*p)&&*p!='_'&&*p!='$') return false;
    return true;
}
static void put_u32(uint8_t *b,uint32_t v) { b[0]=(uint8_t)(v>>24);b[1]=(uint8_t)(v>>16);b[2]=(uint8_t)(v>>8);b[3]=(uint8_t)v; }
static uint32_t get_u32(const uint8_t *b) { return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|(uint32_t)b[3]; }
static uint16_t get_u16(const uint8_t *b) { return (uint16_t)(((uint16_t)b[0]<<8)|b[1]); }

/* ------------------------------------------------------------------ */
/* Network I/O                                                          */
/* ------------------------------------------------------------------ */
static bool pg_send_all(const void *buf, size_t len) {
    const char *p=(const char*)buf;
    while (len>0) {
#ifdef _WIN32
        int n=send(g_sock,p,(int)len,0);
#else
        ssize_t n=send(g_sock,p,len,0);
#endif
        if (n<=0) return false; p+=n; len-=(size_t)n;
    }
    return true;
}
static bool pg_recv_all(void *buf, size_t len) {
    char *p=(char*)buf;
    while (len>0) {
#ifdef _WIN32
        int n=recv(g_sock,p,(int)len,0);
#else
        ssize_t n=recv(g_sock,p,len,0);
#endif
        if (n<=0) return false; p+=n; len-=(size_t)n;
    }
    return true;
}
static bool pg_send_msg(uint8_t type,const uint8_t *body,uint32_t blen) {
    uint8_t hdr[5]; uint32_t total=blen+4;
    if (type) { hdr[0]=type; put_u32(hdr+1,total); if (!pg_send_all(hdr,5)) return false; }
    else       { put_u32(hdr,total);                if (!pg_send_all(hdr,4)) return false; }
    return blen==0||pg_send_all(body,blen);
}
static bool pg_recv_msg(uint8_t *type,uint8_t **body,uint32_t *blen) {
    uint8_t hdr[5]; if (!pg_recv_all(hdr,5)) return false;
    *type=hdr[0]; uint32_t len=get_u32(hdr+1); if (len<4) return false;
    *blen=len-4; *body=(uint8_t*)malloc(*blen+1); if (!*body) return false;
    (*body)[*blen]='\0'; if (*blen>0&&!pg_recv_all(*body,*blen)) { free(*body); return false; }
    return true;
}

/* ------------------------------------------------------------------ */
/* Socket connect                                                       */
/* ------------------------------------------------------------------ */
static pg_sock_t pg_connect_tcp(const char *host,const char *port,int tmo,char *err,size_t esz) {
    struct addrinfo hints, *res, *rp;
    memset(&hints,0,sizeof(hints)); hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    if (getaddrinfo(host,port,&hints,&res)!=0) { snprintf(err,esz,"getaddrinfo failed for %s:%s",host,port); return PG_INVALID_SOCK; }
    pg_sock_t s=PG_INVALID_SOCK;
    for (rp=res;rp;rp=rp->ai_next) {
        s=socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
        if (s==PG_INVALID_SOCK) continue;
#ifdef _WIN32
        if (tmo>0) { DWORD ms=(DWORD)(tmo*1000); setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&ms,sizeof(ms)); setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&ms,sizeof(ms)); }
#else
        if (tmo>0) { struct timeval tv={tmo,0}; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv)); setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv)); }
#endif
        if (connect(s,rp->ai_addr,(socklen_t)rp->ai_addrlen)==0) break;
        pg_close_sock(s); s=PG_INVALID_SOCK;
    }
    freeaddrinfo(res);
    if (s==PG_INVALID_SOCK) snprintf(err,esz,"Cannot connect to %s:%s",host,port);
    return s;
}
#ifndef _WIN32
static pg_sock_t pg_connect_unix(const char *dir,const char *port,int tmo,char *err,size_t esz) {
    struct sockaddr_un addr; memset(&addr,0,sizeof(addr)); addr.sun_family=AF_UNIX;
    snprintf(addr.sun_path,sizeof(addr.sun_path),"%s/.s.PGSQL.%s",dir,port);
    pg_sock_t s=socket(AF_UNIX,SOCK_STREAM,0);
    if (s==PG_INVALID_SOCK) { snprintf(err,esz,"socket() failed"); return s; }
    if (tmo>0) { struct timeval tv={tmo,0}; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv)); setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv)); }
    if (connect(s,(struct sockaddr*)&addr,sizeof(addr))!=0) { snprintf(err,esz,"Cannot connect to Unix socket %s",addr.sun_path); pg_close_sock(s); return PG_INVALID_SOCK; }
    return s;
}
#endif

/* ------------------------------------------------------------------ */
/* Startup + authentication                                             */
/* ------------------------------------------------------------------ */
static bool pg_error_msg(const uint8_t *msg,uint32_t mlen,char *err,size_t esz) {
    for (uint32_t i=0;i+1<mlen;) { if (msg[i]=='M') { snprintf(err,esz,"%s",(const char*)(msg+i+1)); return true; } size_t fl=strlen((const char*)(msg+i)); i+=(uint32_t)fl+1; if (i>=mlen||msg[i]=='\0') break; i++; }
    snprintf(err,esz,"Unknown error"); return false;
}

static bool pg_do_startup(const PgConfig *cfg,char *err,size_t esz) {
    const char *keys[]={"user","database","application_name",NULL};
    const char *vals[]={cfg->user,cfg->dbname,"postgres-mcp-server",NULL};
    size_t blen=4;
    for (int i=0;keys[i];i++) blen+=strlen(keys[i])+1+strlen(vals[i])+1;
    blen+=1;
    uint8_t *body=(uint8_t*)malloc(blen);
    if (!body) { snprintf(err,esz,"Out of memory"); return false; }
    put_u32(body,196608);
    size_t off=4;
    for (int i=0;keys[i];i++) {
        size_t kl=strlen(keys[i])+1,vl=strlen(vals[i])+1;
        memcpy(body+off,keys[i],kl); off+=kl;
        memcpy(body+off,vals[i],vl); off+=vl;
    }
    body[off]='\0';
    bool ok=pg_send_msg(0,body,(uint32_t)blen); free(body);
    if (!ok) { snprintf(err,esz,"Failed to send startup message"); return false; }
    for (;;) {
        uint8_t type; uint8_t *msg; uint32_t mlen;
        if (!pg_recv_msg(&type,&msg,&mlen)) { snprintf(err,esz,"Connection lost during authentication"); return false; }
        if (type=='E') { pg_error_msg(msg,mlen,err,esz); free(msg); return false; }
        if (type=='Z') { free(msg); return true; }
        if (type=='S'||type=='K') { free(msg); continue; }
        if (type=='R') {
            if (mlen<4) { free(msg); snprintf(err,esz,"Bad AuthRequest"); return false; }
            uint32_t atype=get_u32(msg);
            if (atype==0) { free(msg); continue; }
            if (atype==3) {
                const char *pw=cfg->password?cfg->password:"";
                size_t pl=strlen(pw)+1; uint8_t *pb=(uint8_t*)malloc(pl);
                if (!pb) { free(msg); snprintf(err,esz,"OOM"); return false; }
                memcpy(pb,pw,pl); bool sent=pg_send_msg('p',pb,(uint32_t)pl);
                free(pb); free(msg);
                if (!sent) { snprintf(err,esz,"Failed to send password"); return false; }
                continue;
            }
            if (atype==5&&mlen>=8) {
                char md5pw[36]; pg_md5_password(cfg->password?cfg->password:"",cfg->user?cfg->user:"",msg+4,md5pw);
                bool sent=pg_send_msg('p',(uint8_t*)md5pw,36); free(msg);
                if (!sent) { snprintf(err,esz,"Failed to send MD5 password"); return false; }
                continue;
            }
            if (atype==10) { /* SCRAM-SHA-256 */
                free(msg);
                char cnonce[25]; char *cf=scram_client_first(cnonce);
                if (!cf) { snprintf(err,esz,"SCRAM: OOM"); return false; }
                const char *mech="SCRAM-SHA-256"; size_t ml=strlen(mech)+1,cfl=strlen(cf);
                uint8_t *pbuf=(uint8_t*)malloc(ml+4+cfl);
                if (!pbuf) { free(cf); snprintf(err,esz,"SCRAM: OOM"); return false; }
                memcpy(pbuf,mech,ml); put_u32(pbuf+ml,(uint32_t)cfl); memcpy(pbuf+ml+4,cf,cfl);
                bool sent=pg_send_msg('p',pbuf,(uint32_t)(ml+4+cfl)); free(pbuf);
                if (!sent) { free(cf); snprintf(err,esz,"SCRAM: send failed"); return false; }
                uint8_t t2; uint8_t *m2; uint32_t ml2;
                if (!pg_recv_msg(&t2,&m2,&ml2)||t2!='R'||ml2<4||get_u32(m2)!=11) {
                    free(cf); if (t2=='E') { pg_error_msg(m2,ml2,err,esz); } else snprintf(err,esz,"SCRAM: expected SASLContinue"); free(m2); return false;
                }
                char *sf1=(char*)malloc(ml2-4+1); if (!sf1) { free(cf); free(m2); snprintf(err,esz,"SCRAM: OOM"); return false; }
                memcpy(sf1,m2+4,ml2-4); sf1[ml2-4]='\0'; free(m2);
                uint8_t ssig[32]; char *fin=scram_client_final(sf1,cfg->password?cfg->password:"",cnonce,ssig);
                free(sf1); free(cf);
                if (!fin) { snprintf(err,esz,"SCRAM: failed to compute client-final"); return false; }
                sent=pg_send_msg('p',(uint8_t*)fin,(uint32_t)strlen(fin)); free(fin);
                if (!sent) { snprintf(err,esz,"SCRAM: send client-final failed"); return false; }
                if (!pg_recv_msg(&t2,&m2,&ml2)||t2!='R'||ml2<4||get_u32(m2)!=12) {
                    if (t2=='E') { pg_error_msg(m2,ml2,err,esz); } else snprintf(err,esz,"SCRAM: expected SASLFinal"); free(m2); return false;
                }
                char *sf2=(char*)malloc(ml2-4+1); bool ok2=false;
                if (sf2) { memcpy(sf2,m2+4,ml2-4); sf2[ml2-4]='\0'; ok2=scram_verify_server(sf2,ssig); free(sf2); }
                free(m2); if (!ok2) { snprintf(err,esz,"SCRAM: server signature mismatch"); return false; }
                continue;
            }
            free(msg); snprintf(err,esz,"Unsupported auth method %u",atype); return false;
        }
        free(msg);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */
bool pg_db_init(const PgConfig *cfg, char *err, size_t esz) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2),&wsa)!=0) { snprintf(err,esz,"WSAStartup failed"); return false; }
#endif
    const char *host=cfg->host?cfg->host:"localhost", *port=cfg->port?cfg->port:"5432";
    int tmo=cfg->connect_timeout_seconds>0?cfg->connect_timeout_seconds:10;
#ifndef _WIN32
    g_sock=(host[0]=='/')?pg_connect_unix(host,port,tmo,err,esz):pg_connect_tcp(host,port,tmo,err,esz);
#else
    g_sock=pg_connect_tcp(host,port,tmo,err,esz);
#endif
    if (g_sock==PG_INVALID_SOCK) return false;
    if (!pg_do_startup(cfg,err,esz)) { pg_close_sock(g_sock); g_sock=PG_INVALID_SOCK; return false; }
    g_read_only=cfg->read_only; g_ready=true; return true;
}
void pg_db_close(void) {
    if (g_sock!=PG_INVALID_SOCK) { uint8_t t[5]={'X',0,0,0,4}; pg_send_all(t,5); pg_close_sock(g_sock); g_sock=PG_INVALID_SOCK; }
    g_ready=false;
#ifdef _WIN32
    WSACleanup();
#endif
}

/* ------------------------------------------------------------------ */
/* Simple Query execution                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    char  **col_names;
    int     col_count;
    char ***rows;
    int     row_count;
    int     row_cap;
    char   *error;
    bool    success;
} PgResult;

static void pg_result_free(PgResult *r) {
    if (!r) return;
    for (int i=0;i<r->col_count;i++) free(r->col_names[i]);
    free(r->col_names);
    for (int i=0;i<r->row_count;i++) { for (int j=0;j<r->col_count;j++) free(r->rows[i][j]); free(r->rows[i]); }
    free(r->rows); free(r->error); memset(r,0,sizeof(PgResult));
}

static PgResult pg_exec(const char *sql) {
    PgResult r; memset(&r,0,sizeof(r));
    if (!g_ready||g_sock==PG_INVALID_SOCK) { r.error=pg_strdup("Not connected"); return r; }
    size_t slen=strlen(sql)+1;
    uint8_t *qbuf=(uint8_t*)malloc(slen);
    if (!qbuf) { r.error=pg_strdup("Out of memory"); return r; }
    memcpy(qbuf,sql,slen);
    bool sent=pg_send_msg('Q',qbuf,(uint32_t)slen); free(qbuf);
    if (!sent) { r.error=pg_strdup("Failed to send query"); return r; }

    for (;;) {
        uint8_t type; uint8_t *msg; uint32_t mlen;
        if (!pg_recv_msg(&type,&msg,&mlen)) { r.error=pg_strdup("Connection lost reading response"); return r; }
        if (type=='T') {
            if (mlen<2) { free(msg); continue; }
            r.col_count=(int)get_u16(msg);
            r.col_names=(char**)calloc((size_t)r.col_count,sizeof(char*));
            if (!r.col_names) { free(msg); r.error=pg_strdup("OOM"); return r; }
            uint32_t off=2;
            for (int i=0;i<r.col_count&&off<mlen;i++) { const char *nm=(const char*)(msg+off); r.col_names[i]=pg_strdup(nm); off+=(uint32_t)strlen(nm)+1+18; }
            free(msg);
        } else if (type=='D') {
            if (mlen<2) { free(msg); continue; }
            int ncols=(int)get_u16(msg), ac=r.col_count>0?r.col_count:ncols;
            char **row=(char**)calloc((size_t)ac,sizeof(char*));
            if (!row) { free(msg); r.error=pg_strdup("OOM"); return r; }
            uint32_t off=2;
            for (int i=0;i<ncols&&off+4<=mlen;i++) {
                int32_t vlen=(int32_t)get_u32(msg+off); off+=4;
                if (vlen>=0&&i<r.col_count) { row[i]=(char*)malloc((size_t)vlen+1); if (row[i]) { memcpy(row[i],msg+off,(size_t)vlen); row[i][vlen]='\0'; } off+=(uint32_t)vlen; }
                else if (vlen>=0) off+=(uint32_t)vlen;
            }
            free(msg);
            if (r.row_count>=r.row_cap) { int nc=r.row_cap?r.row_cap*2:64; char ***nr=(char***)realloc(r.rows,(size_t)nc*sizeof(char**)); if (!nr) { free(row); r.error=pg_strdup("OOM"); return r; } r.rows=nr; r.row_cap=nc; }
            r.rows[r.row_count++]=row;
        } else if (type=='E') {
            char emsg[512]="Query error";
            for (uint32_t i=0;i+1<mlen;) { if (msg[i]=='M') { snprintf(emsg,sizeof(emsg),"%s",(char*)(msg+i+1)); break; } size_t fl=strlen((char*)(msg+i)); i+=(uint32_t)fl+1; if (i>=mlen||msg[i]=='\0') break; i++; }
            free(r.error); r.error=pg_strdup(emsg); free(msg);
        } else if (type=='Z') { free(msg); break;
        } else { free(msg); }
    }
    r.success = (r.error == NULL);
    return r;
}

static DbQueryResult pg_format(PgResult *r) {
    if (!r->success) {
        DbQueryResult res = pg_fail(r->error ? r->error : "Query failed");
        pg_result_free(r); return res;
    }
    SfBuf md={NULL,0,0,false}, js={NULL,0,0,false};
    if (r->col_count == 0) {
        sf_buf_str(&md,"| status |\n|---|\n| ok |"); sf_buf_str(&js,"{\"status\":\"ok\"}");
    } else {
        sf_buf_str(&md,"|");
        for (int c=0;c<r->col_count;c++) { sf_buf_str(&md," "); sf_buf_str(&md,r->col_names[c]?r->col_names[c]:"col"); sf_buf_str(&md," |"); }
        sf_buf_str(&md,"\n|");
        for (int c=0;c<r->col_count;c++) sf_buf_str(&md,"---|" );
        sf_buf_str(&js,"[");
        for (int row=0;row<r->row_count;row++) {
            sf_buf_str(&md,"\n|"); sf_buf_str(&js,row?",{":"{");
            for (int c=0;c<r->col_count;c++) {
                const char *v=r->rows[row][c];
                sf_buf_str(&md," "); sf_buf_str(&md,v?v:"NULL"); sf_buf_str(&md," |");
                sf_buf_str(&js,c?",\"":"\""); sf_buf_json_escaped(&js,r->col_names[c]?r->col_names[c]:"col");
                if (v) { sf_buf_str(&js,"\":\""); sf_buf_json_escaped(&js,v); sf_buf_str(&js,"\""); }
                else     sf_buf_str(&js,"\":null");
            }
            sf_buf_str(&js,"}");
        }
        sf_buf_str(&js,"]");
    }
    int rows=r->row_count; pg_result_free(r);
    DbQueryResult res; memset(&res,0,sizeof(res));
    res.markdown_table=sf_buf_take(&md); res.json_structured=sf_buf_take(&js);
    if (!res.markdown_table||!res.json_structured) { free(res.markdown_table); free(res.json_structured); return pg_fail("Out of memory"); }
    res.success=true; res.rows_affected=rows; return res;
}

static DbQueryResult pg_run(const char *sql) {
    PgResult r = pg_exec(sql);
    return pg_format(&r);
}

/* ------------------------------------------------------------------ */
/* Read-only statement guard                                            */
/* ------------------------------------------------------------------ */
static bool is_read_only_sql(const char *sql) {
    static const char *const allowed[] = { "SELECT","WITH","TABLE","EXPLAIN","SHOW","VALUES" };
    while (isspace((unsigned char)*sql)) sql++;
    size_t n = 0;
    while (isalpha((unsigned char)sql[n])) n++;
    for (size_t i = 0; i < sizeof(allowed)/sizeof(allowed[0]); i++) {
        if (strlen(allowed[i]) != n) continue;
        size_t k = 0;
        while (k < n && toupper((unsigned char)sql[k]) == (unsigned char)allowed[i][k]) k++;
        if (k == n) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* db_* interface                                                       */
/* ------------------------------------------------------------------ */
DbQueryResult db_execute_read(const char *sql, const char *params_json) {
    (void)params_json;
    if (!sql || !*sql) return pg_fail("SQL query cannot be null or empty");
    if (!is_read_only_sql(sql))
        return pg_fail("read_query accepts SELECT/WITH/TABLE/EXPLAIN/SHOW/VALUES; use write_query for modifications");
    return pg_run(sql);
}

DbQueryResult db_execute_write(const char *sql, const char *params_json) {
    (void)params_json;
    if (!sql || !*sql) return pg_fail("SQL query cannot be null or empty");
    if (g_read_only) return pg_fail("Server is in READONLY mode; write queries are disabled");
    return pg_run(sql);
}

DbQueryResult db_backup(const char *target_path) {
    (void)target_path;
    return pg_fail("backup_database is not supported for PostgreSQL");
}

DbQueryResult db_explain(const char *sql) {
    if (!sql || !*sql) return pg_fail("SQL query cannot be null or empty");
    SfBuf b = {NULL,0,0,false};
    sf_buf_str(&b, "EXPLAIN "); sf_buf_str(&b, sql);
    char *s = sf_buf_take(&b);
    if (!s) return pg_fail("Out of memory");
    DbQueryResult r = pg_run(s); free(s); return r;
}

DbQueryResult db_describe_table(const char *table_name) {
    if (!table_name || !db_validate_identifier(table_name))
        return pg_fail("Invalid table identifier");
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT column_name,data_type,is_nullable,column_default FROM "
        "information_schema.columns WHERE table_name='%s' "
        "ORDER BY ordinal_position", table_name);
    return pg_run(sql);
}

DbQueryResult db_infer_relationships(void) {
    return pg_run(
        "SELECT tc.table_name AS source_table,kcu.column_name AS foreign_key,"
        "ccu.table_name AS target_table,ccu.column_name AS target_key "
        "FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "ON tc.constraint_name=kcu.constraint_name AND tc.table_schema=kcu.table_schema "
        "JOIN information_schema.constraint_column_usage ccu "
        "ON ccu.constraint_name=tc.constraint_name AND ccu.table_schema=tc.table_schema "
        "WHERE tc.constraint_type='FOREIGN KEY' ORDER BY tc.table_name");
}

DbQueryResult db_list_tables(void) {
    return pg_run(
        "SELECT table_name, table_type AS type "
        "FROM information_schema.tables "
        "WHERE table_schema=current_schema() ORDER BY table_name");
}

DbQueryResult db_export_data(const char *table_name, const char *format) {
    (void)format;
    if (!table_name || !db_validate_identifier(table_name))
        return pg_fail("Invalid table identifier for export");
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s LIMIT 1000", table_name);
    return pg_run(sql);
}

DbQueryResult db_check_health(void) {
    return pg_run(
        "SELECT current_database() AS database, current_user AS user_name, "
        "inet_server_addr()::text AS server_addr, inet_server_port()::text AS server_port, "
        "version() AS version, "
        "pg_size_pretty(pg_database_size(current_database())) AS db_size, "
        "now()::text AS server_time");
}

DbQueryResult db_generate_erd_plantuml(void) {
    PgResult cols = pg_exec(
        "SELECT table_name,column_name,data_type FROM information_schema.columns "
        "WHERE table_schema=current_schema() ORDER BY table_name,ordinal_position");
    PgResult fks = pg_exec(
        "SELECT tc.table_name AS src,kcu.column_name AS fk,ccu.table_name AS tgt "
        "FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "ON tc.constraint_name=kcu.constraint_name AND tc.table_schema=kcu.table_schema "
        "JOIN information_schema.constraint_column_usage ccu "
        "ON ccu.constraint_name=tc.constraint_name AND ccu.table_schema=tc.table_schema "
        "WHERE tc.constraint_type='FOREIGN KEY' ORDER BY tc.table_name");
    SfBuf uml={NULL,0,0,false}, js={NULL,0,0,false}, md={NULL,0,0,false};
    sf_buf_str(&uml, "@startuml\n");
    char prev[256]="";
    for (int r=0; r<cols.row_count; r++) {
        const char *tbl=cols.rows[r][0]?cols.rows[r][0]:"";
        const char *col=cols.rows[r][1]?cols.rows[r][1]:"";
        const char *typ=cols.rows[r][2]?cols.rows[r][2]:"";
        if (strcmp(tbl,prev)!=0) {
            if (prev[0]) sf_buf_str(&uml,"}\n");
            sf_buf_printf(&uml,"entity \"%s\" {\n",tbl);
            snprintf(prev,sizeof(prev),"%s",tbl);
        }
        sf_buf_printf(&uml,"  %s : %s\n",col,typ);
    }
    if (prev[0]) sf_buf_str(&uml,"}\n");
    for (int r=0; r<fks.row_count; r++) {
        const char *src=fks.rows[r][0]?fks.rows[r][0]:"";
        const char *fk =fks.rows[r][1]?fks.rows[r][1]:"fk";
        const char *tgt=fks.rows[r][2]?fks.rows[r][2]:"";
        sf_buf_printf(&uml,"\"%s\" ||--o{ \"%s\" : %s\n",tgt,src,fk);
    }
    sf_buf_str(&uml,"@enduml\n");
    pg_result_free(&cols); pg_result_free(&fks);
    sf_buf_str(&md,"```plantuml\n"); if (uml.p) sf_buf_str(&md,uml.p); sf_buf_str(&md,"```");
    sf_buf_str(&js,"{\"format\":\"plantuml\",\"schema_erd\":\""); if (uml.p) sf_buf_json_escaped(&js,uml.p); sf_buf_str(&js,"\"}");
    sf_buf_free(&uml);
    DbQueryResult res; memset(&res,0,sizeof(res));
    res.markdown_table=sf_buf_take(&md); res.json_structured=sf_buf_take(&js);
    res.success=res.markdown_table&&res.json_structured;
    if (!res.success) res.error_message=pg_strdup("Out of memory");
    return res;
}