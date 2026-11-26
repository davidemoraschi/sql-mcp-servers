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
#include <shellapi.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#endif
#include "aws_sso.h"
#include "sf_http.h"
#include "json_rpc.h"
#include "sf_buf.h"

typedef struct {char session[256],start[1024],region[64],account[64],role[256];} SsoConfig;
static char*dupstr(const char*s){if(!s||!*s)return NULL;size_t n=strlen(s)+1;char*p=(char*)malloc(n);if(p)memcpy(p,s,n);return p;}
static char*trim(char*s){while(isspace((unsigned char)*s))s++;char*e=s+strlen(s);while(e>s&&isspace((unsigned char)e[-1]))*--e=0;return s;}
static const char*home(void){
#ifdef _WIN32
 return getenv("USERPROFILE");
#else
 return getenv("HOME");
#endif
}
static void config_path(char*out,size_t cap){const char*p=getenv("AWS_CONFIG_FILE");if(p&&*p)snprintf(out,cap,"%s",p);else{
#ifdef _WIN32
 snprintf(out,cap,"%s\\.aws\\config",home()?home():"");
#else
 snprintf(out,cap,"%s/.aws/config",home()?home():"");
#endif
}}
static bool read_config(const char*profile,SsoConfig*c){char path[2048],line[4096],wanted[320],session_section[320];config_path(path,sizeof(path));FILE*f=fopen(path,"rb");if(!f)return false;snprintf(wanted,sizeof(wanted),"%s",!strcmp(profile,"default")?"default":"");if(strcmp(profile,"default"))snprintf(wanted,sizeof(wanted),"profile %s",profile);char section[320]="";
 while(fgets(line,sizeof(line),f)){char*p=trim(line);if(*p=='['){char*e=strchr(p,']');if(e){*e=0;snprintf(section,sizeof(section),"%s",trim(p+1));}continue;}char*eq=strchr(p,'=');if(!eq)continue;*eq=0;char*k=trim(p),*v=trim(eq+1);if(!strcmp(section,wanted)){if(!strcmp(k,"sso_session"))snprintf(c->session,sizeof(c->session),"%s",v);else if(!strcmp(k,"sso_account_id"))snprintf(c->account,sizeof(c->account),"%s",v);else if(!strcmp(k,"sso_role_name"))snprintf(c->role,sizeof(c->role),"%s",v);else if(!strcmp(k,"sso_start_url"))snprintf(c->start,sizeof(c->start),"%s",v);else if(!strcmp(k,"sso_region"))snprintf(c->region,sizeof(c->region),"%s",v);}}
 if(c->session[0]){rewind(f);snprintf(session_section,sizeof(session_section),"sso-session %s",c->session);section[0]=0;while(fgets(line,sizeof(line),f)){char*p=trim(line);if(*p=='['){char*e=strchr(p,']');if(e){*e=0;snprintf(section,sizeof(section),"%s",trim(p+1));}continue;}char*eq=strchr(p,'=');if(!eq||strcmp(section,session_section))continue;*eq=0;char*k=trim(p),*v=trim(eq+1);if(!strcmp(k,"sso_start_url"))snprintf(c->start,sizeof(c->start),"%s",v);else if(!strcmp(k,"sso_region"))snprintf(c->region,sizeof(c->region),"%s",v);}}
 fclose(f);return c->start[0]&&c->region[0]&&c->account[0]&&c->role[0];}
static JsonValue*post_json(const char*host,const char*path,const char*body,int*status,char*err,size_t cap){SfHttpRequest rq;SfHttpResponse res;memset(&rq,0,sizeof(rq));rq.method="POST";rq.host=host;rq.path_and_query=path;rq.json_body=body;rq.content_type="application/json";rq.timeout_ms=60000;if(!sf_http_request(&rq,&res)){snprintf(err,cap,"AWS SSO HTTPS request failed: %s",res.error?res.error:"transport error");sf_http_free_response(&res);return NULL;}*status=res.status;JsonValue*root=res.body?json_parse(res.body):NULL;if(!root)snprintf(err,cap,"AWS SSO returned invalid JSON");sf_http_free_response(&res);return root;}
static void open_browser(const char*url){
#ifdef _WIN32
 ShellExecuteA(NULL,"open",url,NULL,NULL,SW_SHOWNORMAL);
#else
 pid_t p=fork();if(p==0){
#ifdef __APPLE__
  execlp("open","open",url,(char*)NULL);
#else
  execlp("xdg-open","xdg-open",url,(char*)NULL);
#endif
  _exit(127);}if(p>0)waitpid(p,NULL,WNOHANG);
#endif
}
static void pause_seconds(int n){
#ifdef _WIN32
 Sleep((DWORD)n*1000);
#else
 sleep((unsigned)n);
#endif
}
static char*interactive_token(const SsoConfig*c,char*err,size_t cap){char hostn[256];snprintf(hostn,sizeof(hostn),"oidc.%s.amazonaws.com",c->region);int status=0;JsonValue*r=post_json(hostn,"/client/register","{\"clientName\":\"sql-mcp-servers\",\"clientType\":\"public\",\"scopes\":[\"sso:account:access\"]}",&status,err,cap);if(!r||status/100!=2){json_free(r);if(!*err)snprintf(err,cap,"AWS SSO client registration failed (HTTP %d)",status);return NULL;}const char*ci=json_string_value(json_object_get(r,"clientId")),*cs=json_string_value(json_object_get(r,"clientSecret"));char*client=dupstr(ci),*secret=dupstr(cs);json_free(r);if(!client||!secret){free(client);free(secret);snprintf(err,cap,"AWS SSO registration returned incomplete credentials");return NULL;}
 SfBuf b={0};sf_buf_str(&b,"{\"clientId\":\"");sf_buf_json_escaped(&b,client);sf_buf_str(&b,"\",\"clientSecret\":\"");sf_buf_json_escaped(&b,secret);sf_buf_str(&b,"\",\"startUrl\":\"");sf_buf_json_escaped(&b,c->start);sf_buf_str(&b,"\"}");char*body=sf_buf_take(&b);r=body?post_json(hostn,"/device_authorization",body,&status,err,cap):NULL;free(body);if(!r||status/100!=2){json_free(r);free(client);free(secret);if(!*err)snprintf(err,cap,"AWS SSO device authorization failed (HTTP %d)",status);return NULL;}const char*dcv=json_string_value(json_object_get(r,"deviceCode")),*urlv=json_string_value(json_object_get(r,"verificationUriComplete")),*base=json_string_value(json_object_get(r,"verificationUri")),*code=json_string_value(json_object_get(r,"userCode"));char*device=dupstr(dcv),*url=dupstr(urlv?urlv:base);int interval=(int)json_number_value(json_object_get(r,"interval"));int expires=(int)json_number_value(json_object_get(r,"expiresIn"));if(interval<1)interval=5;if(expires<60)expires=600;fprintf(stderr,"[INFO] AWS IAM Identity Center authorization%s%s.\n",code?" code: ":"",code?code:"");if(url){fprintf(stderr,"[INFO] Opening AWS sign-in: %s\n",url);open_browser(url);}json_free(r);free(url);if(!device){free(client);free(secret);snprintf(err,cap,"AWS SSO returned no device code");return NULL;}
 char*access=NULL;int attempts=expires/interval+1;for(int i=0;i<attempts;i++){SfBuf t={0};sf_buf_str(&t,"{\"clientId\":\"");sf_buf_json_escaped(&t,client);sf_buf_str(&t,"\",\"clientSecret\":\"");sf_buf_json_escaped(&t,secret);sf_buf_str(&t,"\",\"deviceCode\":\"");sf_buf_json_escaped(&t,device);sf_buf_str(&t,"\",\"grantType\":\"urn:ietf:params:oauth:grant-type:device_code\"}");body=sf_buf_take(&t);r=body?post_json(hostn,"/token",body,&status,err,cap):NULL;free(body);if(r&&status/100==2){access=dupstr(json_string_value(json_object_get(r,"accessToken")));json_free(r);break;}const char*e=r?json_string_value(json_object_get(r,"error")):NULL;if(e&&!strcmp(e,"slow_down"))interval+=5;else if(e&&strcmp(e,"authorization_pending")){snprintf(err,cap,"AWS SSO authorization failed: %s",e);json_free(r);break;}json_free(r);pause_seconds(interval);}free(device);free(client);if(secret){memset(secret,0,strlen(secret));free(secret);}if(!access&&!*err)snprintf(err,cap,"AWS SSO authorization timed out");return access;}
static char*cached_token_file(const char*path,const SsoConfig*c){FILE*f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<=0||n>1024*1024){fclose(f);return NULL;}char*data=(char*)malloc((size_t)n+1);if(!data){fclose(f);return NULL;}size_t got=fread(data,1,(size_t)n,f);fclose(f);data[got]=0;JsonValue*r=json_parse(data);free(data);if(!r)return NULL;const char*start=json_string_value(json_object_get(r,"startUrl")),*session=json_string_value(json_object_get(r,"sessionName"));const char*token=json_string_value(json_object_get(r,"accessToken"));bool match=(start&&!strcmp(start,c->start))||(session&&c->session[0]&&!strcmp(session,c->session));char*out=match&&token?dupstr(token):NULL;json_free(r);return out;}
static char*cached_token(const SsoConfig*c){char dir[2048];
#ifdef _WIN32
 snprintf(dir,sizeof(dir),"%s\\.aws\\sso\\cache\\*",home()?home():"");WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA(dir,&fd);if(h==INVALID_HANDLE_VALUE)return NULL;char*token=NULL;do{if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){char path[2300];snprintf(path,sizeof(path),"%s\\.aws\\sso\\cache\\%s",home()?home():"",fd.cFileName);token=cached_token_file(path,c);}}while(!token&&FindNextFileA(h,&fd));FindClose(h);return token;
#else
 snprintf(dir,sizeof(dir),"%s/.aws/sso/cache",home()?home():"");DIR*d=opendir(dir);if(!d)return NULL;char*token=NULL;struct dirent*e;while(!token&&(e=readdir(d))){if(e->d_name[0]=='.')continue;char path[2300];snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);token=cached_token_file(path,c);}closedir(d);return token;
#endif
}
static bool role_credentials(const SsoConfig*c,const char*token,AwsCredentials*out,char*err,size_t cap){char url[2048],headers[4096];snprintf(url,sizeof(url),"https://portal.sso.%s.amazonaws.com/federation/credentials?account_id=%s&role_name=%s",c->region,c->account,c->role);snprintf(headers,sizeof(headers),"x-amz-sso_bearer_token: %s\r\n",token);SfHttpResponse res;if(!sf_http_get_url(url,headers,false,60000,&res)){snprintf(err,cap,"AWS SSO role request failed: %s",res.error?res.error:"transport error");sf_http_free_response(&res);return false;}JsonValue*r=res.body?json_parse(res.body):NULL;if(res.status/100!=2){const char*m=r?json_string_value(json_object_get(r,"message")):NULL;snprintf(err,cap,"AWS SSO role credentials HTTP %d: %s",res.status,m?m:"access token rejected");json_free(r);sf_http_free_response(&res);return false;}const JsonValue*rc=json_object_get(r,"roleCredentials");out->access_key=dupstr(json_string_value(json_object_get(rc,"accessKeyId")));out->secret_key=dupstr(json_string_value(json_object_get(rc,"secretAccessKey")));out->session_token=dupstr(json_string_value(json_object_get(rc,"sessionToken")));json_free(r);sf_http_free_response(&res);if(!out->access_key||!out->secret_key||!out->session_token){aws_credentials_free(out);snprintf(err,cap,"AWS SSO returned incomplete role credentials");return false;}return true;}
bool aws_sso_credentials_load(const char*profile,AwsCredentials*out,char*err,size_t cap){SsoConfig c;memset(&c,0,sizeof(c));if(!read_config(profile,&c))return false;char*token=cached_token(&c);if(token&&role_credentials(&c,token,out,err,cap)){memset(token,0,strlen(token));free(token);return true;}if(token){memset(token,0,strlen(token));free(token);err[0]=0;}token=interactive_token(&c,err,cap);if(!token)return false;bool ok=role_credentials(&c,token,out,err,cap);memset(token,0,strlen(token));free(token);return ok;}
