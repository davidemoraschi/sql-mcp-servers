#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "aws_credentials.h"
#include "aws_sso.h"

static char *dupstr(const char*s){if(!s||!*s)return NULL;size_t n=strlen(s)+1;char*p=(char*)malloc(n);if(p)memcpy(p,s,n);return p;}
static void wipe(char **p){if(*p){volatile char*q=*p;size_t n=strlen(*p);while(n--)*q++=0;free(*p);*p=NULL;}}
void aws_credentials_free(AwsCredentials*c){if(!c)return;free(c->access_key);wipe(&c->secret_key);wipe(&c->session_token);memset(c,0,sizeof(*c));}
static char *trim(char*s){while(isspace((unsigned char)*s))s++;char*e=s+strlen(s);while(e>s&&isspace((unsigned char)e[-1]))*--e=0;return s;}
static bool assign(char **dst,const char*v){free(*dst);*dst=dupstr(v);return *dst!=NULL;}

static bool load_file(const char *path,const char *profile,AwsCredentials*out){
 FILE*f=fopen(path,"rb");if(!f)return false;char line[4096],section[256]="";bool selected=false;
 while(fgets(line,sizeof(line),f)){char*p=trim(line);if(!*p||*p=='#'||*p==';')continue;
  if(*p=='['){char*e=strchr(p,']');if(!e)continue;*e=0;snprintf(section,sizeof(section),"%s",trim(p+1));selected=strcmp(section,profile)==0;continue;}
  if(!selected)continue;char*eq=strchr(p,'=');if(!eq)continue;*eq=0;char*k=trim(p),*v=trim(eq+1);
  if(!strcmp(k,"aws_access_key_id"))assign(&out->access_key,v);
  else if(!strcmp(k,"aws_secret_access_key"))assign(&out->secret_key,v);
  else if(!strcmp(k,"aws_session_token")||!strcmp(k,"aws_security_token"))assign(&out->session_token,v);
 }
 fclose(f);return out->access_key&&out->secret_key;
}

bool aws_credentials_load(const char *profile,AwsCredentials*out,char*err,size_t cap){
 memset(out,0,sizeof(*out));const char*ak=getenv("AWS_ACCESS_KEY_ID"),*sk=getenv("AWS_SECRET_ACCESS_KEY"),*st=getenv("AWS_SESSION_TOKEN");
 if(ak&&*ak&&sk&&*sk){out->access_key=dupstr(ak);out->secret_key=dupstr(sk);out->session_token=dupstr(st);if(out->access_key&&out->secret_key)return true;aws_credentials_free(out);snprintf(err,cap,"Out of memory");return false;}
 const char*p=profile&&*profile?profile:getenv("AWS_PROFILE");if(!p||!*p)p="default";
 const char*override=getenv("AWS_SHARED_CREDENTIALS_FILE");char path[2048];
 if(override&&*override)snprintf(path,sizeof(path),"%s",override);
 else {
#ifdef _WIN32
  const char*home=getenv("USERPROFILE");if(!home)home="";snprintf(path,sizeof(path),"%s\\.aws\\credentials",home);
#else
  const char*home=getenv("HOME");if(!home)home="";snprintf(path,sizeof(path),"%s/.aws/credentials",home);
#endif
 }
 if(load_file(path,p,out))return true;aws_credentials_free(out);
 const char*config_override=getenv("AWS_CONFIG_FILE");char config_path[2048],section[320];
 if(config_override&&*config_override)snprintf(config_path,sizeof(config_path),"%s",config_override);
 else {
#ifdef _WIN32
  const char*home=getenv("USERPROFILE");if(!home)home="";snprintf(config_path,sizeof(config_path),"%s\\.aws\\config",home);
#else
  const char*home=getenv("HOME");if(!home)home="";snprintf(config_path,sizeof(config_path),"%s/.aws/config",home);
#endif
 }
 snprintf(section,sizeof(section),"%s",!strcmp(p,"default")?"default":"");if(strcmp(p,"default"))snprintf(section,sizeof(section),"profile %s",p);
 if(load_file(config_path,section,out))return true;aws_credentials_free(out);
 err[0]=0;if(aws_sso_credentials_load(p,out,err,cap))return true;if(*err)return false;
 snprintf(err,cap,"No static AWS credentials found; set AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY or configure profile '%s' in %s",p,path);return false;
}
