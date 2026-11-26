#include <string.h>
#include "aws_sha256.h"

typedef struct { uint32_t h[8]; uint64_t bits; unsigned char block[64]; size_t used; } Sha256;
static const uint32_t K[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define R(x,n) ((x>>n)|(x<<(32-n)))
static void transform(Sha256 *s,const unsigned char *p){uint32_t w[64],a,b,c,d,e,f,g,h;
 for(int i=0;i<16;i++)w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
 for(int i=16;i<64;i++){uint32_t x=w[i-15],y=w[i-2];w[i]=(R(x,7)^R(x,18)^(x>>3))+w[i-16]+(R(y,17)^R(y,19)^(y>>10))+w[i-7];}
 a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
 for(int i=0;i<64;i++){uint32_t t1=h+(R(e,6)^R(e,11)^R(e,25))+((e&f)^((~e)&g))+K[i]+w[i];uint32_t t2=(R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
 s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;}
static void init(Sha256*s){uint32_t v[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memcpy(s->h,v,sizeof(v));s->bits=0;s->used=0;}
static void update(Sha256*s,const unsigned char*p,size_t n){s->bits+=(uint64_t)n*8;while(n){size_t take=64-s->used;if(take>n)take=n;memcpy(s->block+s->used,p,take);s->used+=take;p+=take;n-=take;if(s->used==64){transform(s,s->block);s->used=0;}}}
static void finish(Sha256*s,unsigned char out[32]){uint64_t bits=s->bits;s->block[s->used++]=0x80;if(s->used>56){while(s->used<64)s->block[s->used++]=0;transform(s,s->block);s->used=0;}while(s->used<56)s->block[s->used++]=0;for(int i=7;i>=0;i--)s->block[s->used++]=(unsigned char)(bits>>(i*8));transform(s,s->block);for(int i=0;i<8;i++){out[i*4]=(unsigned char)(s->h[i]>>24);out[i*4+1]=(unsigned char)(s->h[i]>>16);out[i*4+2]=(unsigned char)(s->h[i]>>8);out[i*4+3]=(unsigned char)s->h[i];}}
void aws_sha256(const unsigned char*d,size_t n,unsigned char out[32]){Sha256 s;init(&s);update(&s,d,n);finish(&s,out);}
void aws_hmac_sha256(const unsigned char*k,size_t kn,const unsigned char*d,size_t n,unsigned char out[32]){unsigned char key[64]={0},inner[32],ipad[64],opad[64];if(kn>64){aws_sha256(k,kn,key);kn=32;}else memcpy(key,k,kn);for(int i=0;i<64;i++){ipad[i]=key[i]^0x36;opad[i]=key[i]^0x5c;}Sha256 s;init(&s);update(&s,ipad,64);update(&s,d,n);finish(&s,inner);init(&s);update(&s,opad,64);update(&s,inner,32);finish(&s,out);memset(key,0,sizeof(key));}
void aws_hex(const unsigned char*d,size_t n,char*out){static const char h[]="0123456789abcdef";for(size_t i=0;i<n;i++){out[i*2]=h[d[i]>>4];out[i*2+1]=h[d[i]&15];}out[n*2]=0;}
