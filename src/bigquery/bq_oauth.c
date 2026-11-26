/* End-user OAuth 2.0 installed-app flow for BigQuery. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <direct.h>
#define close_socket closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close
#endif
#include "bq_oauth.h"
#include "sf_http.h"
#include "json_rpc.h"

#define OAUTH_SCOPE "https://www.googleapis.com/auth/bigquery"

static char *dupstr(const char *s) { size_t n=strlen(s)+1; char *p=(char*)malloc(n); if(p)memcpy(p,s,n); return p; }

static bool secure_random(unsigned char *out,size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(NULL,out,(ULONG)n,BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0;
#else
    FILE *f=fopen("/dev/urandom","rb");if(!f)return false;bool ok=fread(out,1,n,f)==n;fclose(f);return ok;
#endif
}

static char *url_encode(const char *s) {
    static const char h[]="0123456789ABCDEF"; size_t n=strlen(s),j=0; char *o=(char*)malloc(n*3+1);
    if(!o)return NULL;
    for(size_t i=0;i<n;i++){unsigned char c=(unsigned char)s[i];if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')o[j++]=(char)c;else{o[j++]='%';o[j++]=h[c>>4];o[j++]=h[c&15];}}
    o[j]=0;return o;
}

static void open_browser(const char *url) {
#ifdef _WIN32
    ShellExecuteA(NULL,"open",url,NULL,NULL,SW_SHOWNORMAL);
#else
    pid_t pid=fork();
    if(pid==0){
#ifdef __APPLE__
        execlp("open","open",url,(char*)NULL);
#else
        execlp("xdg-open","xdg-open",url,(char*)NULL);
#endif
        _exit(127);
    }
    if(pid>0)waitpid(pid,NULL,WNOHANG);
#endif
}

static char *query_value(const char *query,const char *key) {
    size_t kl=strlen(key); const char *p=query;
    while(p&&*p){const char *eq=strchr(p,'=');if(!eq)break;const char *end=strchr(p,'&');if(!end)end=p+strlen(p);
        if((size_t)(eq-p)==kl&&!memcmp(p,key,kl)){size_t n=(size_t)(end-eq-1);char *v=(char*)malloc(n+1);if(!v)return NULL;size_t j=0;
            for(size_t i=0;i<n;i++){if(eq[1+i]=='%'&&i+2<n){char hx[3]={eq[2+i],eq[3+i],0};v[j++]=(char)strtol(hx,NULL,16);i+=2;}else if(eq[1+i]=='+')v[j++]=' ';else v[j++]=eq[1+i];}v[j]=0;return v;}
        p=*end?end+1:NULL;
    }return NULL;
}

static bool receive_code(char *state,const char *client_id,int timeout_seconds,char **code,char **redirect,char *err,size_t cap) {
#ifdef _WIN32
    WSADATA wd; if(WSAStartup(MAKEWORD(2,2),&wd)){snprintf(err,cap,"Could not initialize loopback listener");return false;}
#endif
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); if(s==INVALID_SOCKET){snprintf(err,cap,"Could not create OAuth loopback listener");return false;}
    struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;
    if(bind(s,(struct sockaddr*)&a,sizeof(a))==SOCKET_ERROR||listen(s,1)==SOCKET_ERROR){snprintf(err,cap,"Could not bind OAuth loopback listener");close_socket(s);return false;}
#ifdef _WIN32
    int alen=sizeof(a);
#else
    socklen_t alen=sizeof(a);
#endif
    if(getsockname(s,(struct sockaddr*)&a,&alen)==SOCKET_ERROR){snprintf(err,cap,"Could not read OAuth callback port");close_socket(s);return false;}
    char redir[96];snprintf(redir,sizeof(redir),"http://127.0.0.1:%u",(unsigned)ntohs(a.sin_port));*redirect=dupstr(redir);
    char *cid=url_encode(client_id),*rd=url_encode(redir),*sc=url_encode(OAUTH_SCOPE),*st=url_encode(state);
    if(!cid||!rd||!sc||!st||!*redirect){free(cid);free(rd);free(sc);free(st);close_socket(s);return false;}
    size_t un=strlen(cid)+strlen(rd)+strlen(sc)+strlen(st)+256;char *url=(char*)malloc(un);
    if(!url){free(cid);free(rd);free(sc);free(st);close_socket(s);return false;}
    snprintf(url,un,"https://accounts.google.com/o/oauth2/v2/auth?client_id=%s&redirect_uri=%s&response_type=code&scope=%s&access_type=offline&prompt=consent&state=%s",cid,rd,sc,st);
    free(cid);free(rd);free(sc);free(st);
    fprintf(stderr,"[INFO] Opening Google sign-in for BigQuery user authorization.\n");open_browser(url);free(url);
    fd_set set;FD_ZERO(&set);FD_SET(s,&set);struct timeval tv={timeout_seconds,0};int ready=select((int)(s+1),&set,NULL,NULL,&tv);
    if(ready<=0){snprintf(err,cap,"Timed out waiting for Google OAuth sign-in callback");close_socket(s);return false;}
    SOCKET c=accept(s,NULL,NULL);close_socket(s);if(c==INVALID_SOCKET){snprintf(err,cap,"Could not accept Google OAuth callback");return false;}
    char req[8192];int got=recv(c,req,(int)sizeof(req)-1,0);if(got<0)got=0;req[got]=0;
    const char *q=strchr(req,'?'),*sp=q?strchr(q,' '):NULL;char *qs=NULL;
    if(q&&sp){size_t n=(size_t)(sp-q-1);qs=(char*)malloc(n+1);if(qs){memcpy(qs,q+1,n);qs[n]=0;}}
    char *returned_state=qs?query_value(qs,"state"):NULL;*code=qs?query_value(qs,"code"):NULL;char *oauth_error=qs?query_value(qs,"error"):NULL;
    const char *page="HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n<html><body><h2>Google authorization received</h2><p>You can close this window and return to your MCP client.</p></body></html>";
    send(c,page,(int)strlen(page),0);close_socket(c);
    bool ok=returned_state&&strcmp(returned_state,state)==0&&*code;
    if(!ok)snprintf(err,cap,"Google OAuth authorization failed%s%s",oauth_error?": ":"",oauth_error?oauth_error:"");
    free(qs);free(returned_state);free(oauth_error);
#ifdef _WIN32
    WSACleanup();
#endif
    return ok;
}

static char *token_cache_path(void) {
    const char *override=getenv("BIGQUERY_OAUTH_TOKEN_CACHE");if(override&&*override)return dupstr(override);
#ifdef _WIN32
    const char *base=getenv("LOCALAPPDATA");if(!base)return NULL;size_t n=strlen(base)+48;char *p=(char*)malloc(n);if(!p)return NULL;snprintf(p,n,"%s\\sql-mcp-servers\\bigquery-refresh-token",base);return p;
#else
    const char *base=getenv("XDG_CONFIG_HOME");char fallback[1024];if(!base||!*base){const char *home=getenv("HOME");if(!home)return NULL;snprintf(fallback,sizeof(fallback),"%s/.config",home);base=fallback;}
    size_t n=strlen(base)+48;char *p=(char*)malloc(n);if(!p)return NULL;snprintf(p,n,"%s/sql-mcp-servers/bigquery-refresh-token",base);return p;
#endif
}

char *bq_oauth_load_cached_refresh_token(void) {
    char *path=token_cache_path();if(!path)return NULL;FILE *f=fopen(path,"rb");free(path);if(!f)return NULL;
    char b[8192];size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);b[n]=0;while(n&&isspace((unsigned char)b[n-1]))b[--n]=0;return n?dupstr(b):NULL;
}

void bq_oauth_save_cached_refresh_token(const char *token) {
    char *path=token_cache_path();if(!path)return;
#ifdef _WIN32
    char *slash=strrchr(path,'\\');if(slash){*slash=0;_mkdir(path);char *parent=strrchr(path,'\\');if(parent){*parent=0;_mkdir(path);}*slash='\\';}
    FILE *f=fopen(path,"wb");
#else
    char *slash=strrchr(path,'/');if(slash){*slash=0;mkdir(path,0700);*slash='/';}
    FILE *f=fopen(path,"wb");if(f)chmod(path,0600);
#endif
    if(f){fputs(token,f);fclose(f);}free(path);
}

bool bq_oauth_user_login(const char *client_id,const char *client_secret,int timeout_seconds,char **refresh_token,char **access_token,int *expires_in,char *err,size_t err_size) {
    unsigned char random[24];char state[49];static const char hx[]="0123456789abcdef";
    if(!secure_random(random,sizeof(random))){snprintf(err,err_size,"Could not generate secure OAuth state");return false;}
    for(size_t i=0;i<sizeof(random);i++){state[i*2]=hx[random[i]>>4];state[i*2+1]=hx[random[i]&15];}state[48]=0;
    char *code=NULL,*redirect=NULL;if(!receive_code(state,client_id,timeout_seconds,&code,&redirect,err,err_size)){free(code);free(redirect);return false;}
    char *cid=url_encode(client_id),*rd=url_encode(redirect),*cd=url_encode(code),*sec=client_secret?url_encode(client_secret):NULL;
    free(code);free(redirect);if(!cid||!rd||!cd||(client_secret&&!sec)){free(cid);free(rd);free(cd);free(sec);snprintf(err,err_size,"Out of memory during OAuth exchange");return false;}
    size_t n=strlen(cid)+strlen(rd)+strlen(cd)+(sec?strlen(sec):0)+128;char *body=(char*)malloc(n);if(!body){free(cid);free(rd);free(cd);free(sec);snprintf(err,err_size,"Out of memory during OAuth exchange");return false;}
    if(sec)snprintf(body,n,"client_id=%s&client_secret=%s&code=%s&redirect_uri=%s&grant_type=authorization_code",cid,sec,cd,rd);else snprintf(body,n,"client_id=%s&code=%s&redirect_uri=%s&grant_type=authorization_code",cid,cd,rd);
    free(cid);free(rd);free(cd);free(sec);
    SfHttpRequest rq;SfHttpResponse res;memset(&rq,0,sizeof(rq));rq.method="POST";rq.host="oauth2.googleapis.com";rq.path_and_query="/token";rq.content_type="application/x-www-form-urlencoded";rq.json_body=body;rq.timeout_ms=timeout_seconds*1000;
    bool sent=sf_http_request(&rq,&res);memset(body,0,n);free(body);
    if(!sent||res.status<200||res.status>=300||!res.body){snprintf(err,err_size,"Google OAuth code exchange failed%s%s",res.error?": ":"",res.error?res.error:"");sf_http_free_response(&res);return false;}
    JsonValue *root=json_parse(res.body);sf_http_free_response(&res);if(!root){snprintf(err,err_size,"Google OAuth returned invalid token response");return false;}
    const char *access=json_string_value(json_object_get(root,"access_token")),*refresh=json_string_value(json_object_get(root,"refresh_token"));const JsonValue *exp=json_object_get(root,"expires_in");
    *access_token=access?dupstr(access):NULL;*refresh_token=refresh?dupstr(refresh):NULL;*expires_in=exp&&exp->type==JSON_NUMBER?(int)exp->u.number:3600;json_free(root);
    if(!*access_token||!*refresh_token){free(*access_token);free(*refresh_token);*access_token=NULL;*refresh_token=NULL;snprintf(err,err_size,"Google did not return user refresh credentials; verify Desktop OAuth client and consent configuration");return false;}
    return true;
}
