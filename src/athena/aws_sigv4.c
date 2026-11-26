#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aws_sigv4.h"
#include "aws_sha256.h"

static void hmac(const unsigned char*k,size_t kn,const char*s,unsigned char out[32]){aws_hmac_sha256(k,kn,(const unsigned char*)s,strlen(s),out);}
bool aws_sigv4_headers(const AwsCredentials*c,const char*region,const char*service,const char*host,const char*target,const char*ct,const char*body,char**headers,char*err,size_t cap){
 time_t now=time(NULL);struct tm u;
#ifdef _WIN32
 if(gmtime_s(&u,&now)){snprintf(err,cap,"Could not create AWS signing timestamp");return false;}
#else
 if(!gmtime_r(&now,&u)){snprintf(err,cap,"Could not create AWS signing timestamp");return false;}
#endif
 char amz[17],day[9];strftime(amz,sizeof(amz),"%Y%m%dT%H%M%SZ",&u);strftime(day,sizeof(day),"%Y%m%d",&u);
 unsigned char hash[32],kdate[32],kregion[32],kservice[32],ksigning[32],sigraw[32];char payload[65],canonhash[65],signature[65];
 aws_sha256((const unsigned char*)body,strlen(body),hash);aws_hex(hash,32,payload);
 const bool token=c->session_token&&*c->session_token;
 const char*signed_headers=token?"content-type;host;x-amz-content-sha256;x-amz-date;x-amz-security-token;x-amz-target":"content-type;host;x-amz-content-sha256;x-amz-date;x-amz-target";
 size_t cn=strlen(ct)+strlen(host)+strlen(payload)+strlen(amz)+strlen(target)+(token?strlen(c->session_token):0)+256;char*canonical=(char*)malloc(cn);
 if(!canonical){snprintf(err,cap,"Out of memory");return false;}
 if(token)snprintf(canonical,cn,"POST\n/\n\ncontent-type:%s\nhost:%s\nx-amz-content-sha256:%s\nx-amz-date:%s\nx-amz-security-token:%s\nx-amz-target:%s\n\n%s\n%s",ct,host,payload,amz,c->session_token,target,signed_headers,payload);
 else snprintf(canonical,cn,"POST\n/\n\ncontent-type:%s\nhost:%s\nx-amz-content-sha256:%s\nx-amz-date:%s\nx-amz-target:%s\n\n%s\n%s",ct,host,payload,amz,target,signed_headers,payload);
 aws_sha256((const unsigned char*)canonical,strlen(canonical),hash);aws_hex(hash,32,canonhash);free(canonical);
 char scope[256],sts[512];snprintf(scope,sizeof(scope),"%s/%s/%s/aws4_request",day,region,service);snprintf(sts,sizeof(sts),"AWS4-HMAC-SHA256\n%s\n%s\n%s",amz,scope,canonhash);
 size_t skn=strlen(c->secret_key)+5;unsigned char*sk=(unsigned char*)malloc(skn);if(!sk){snprintf(err,cap,"Out of memory");return false;}snprintf((char*)sk,skn,"AWS4%s",c->secret_key);
 hmac(sk,strlen((char*)sk),day,kdate);hmac(kdate,32,region,kregion);hmac(kregion,32,service,kservice);hmac(kservice,32,"aws4_request",ksigning);hmac(ksigning,32,sts,sigraw);aws_hex(sigraw,32,signature);memset(sk,0,skn);free(sk);
 size_t hn=strlen(c->access_key)+strlen(scope)+strlen(signed_headers)+strlen(signature)+strlen(payload)+strlen(amz)+strlen(target)+(token?strlen(c->session_token):0)+256;
 *headers=(char*)malloc(hn);if(!*headers){snprintf(err,cap,"Out of memory");return false;}
 if(token)snprintf(*headers,hn,"Authorization: AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s\r\nX-Amz-Content-Sha256: %s\r\nX-Amz-Date: %s\r\nX-Amz-Security-Token: %s\r\nX-Amz-Target: %s",c->access_key,scope,signed_headers,signature,payload,amz,c->session_token,target);
 else snprintf(*headers,hn,"Authorization: AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s\r\nX-Amz-Content-Sha256: %s\r\nX-Amz-Date: %s\r\nX-Amz-Target: %s",c->access_key,scope,signed_headers,signature,payload,amz,target);
 memset(kdate,0,sizeof(kdate));memset(kregion,0,sizeof(kregion));memset(kservice,0,sizeof(kservice));memset(ksigning,0,sizeof(ksigning));return true;
}
