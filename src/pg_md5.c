/*
 * pg_md5.c - Minimal self-contained MD5 for PostgreSQL MD5 password auth
 *
 * Line budget constraint: < 150 lines
 */
#include <string.h>
#include "pg_md5.h"

typedef struct { uint32_t s[4]; uint32_t c[2]; uint8_t buf[64]; } Md5Ctx;

static void md5_init(Md5Ctx *c) {
    c->s[0]=0x67452301; c->s[1]=0xefcdab89;
    c->s[2]=0x98badcfe; c->s[3]=0x10325476;
    c->c[0]=c->c[1]=0;
}

#define F(b,c,d) (((b)&(c))|((~(b))&(d)))
#define G(b,c,d) (((b)&(d))|((c)&(~(d))))
#define H(b,c,d) ((b)^(c)^(d))
#define I(b,c,d) ((c)^((b)|(~(d))))
#define ROL(x,n) (((x)<<(n))|((x)>>(32-(n))))
#define STEP(f,a,b,c,d,x,t,s) (a)+=f((b),(c),(d))+(x)+(t); (a)=ROL(a,s)+(b)

static void md5_block(Md5Ctx *ctx, const uint8_t *blk) {
    uint32_t a=ctx->s[0],b=ctx->s[1],c=ctx->s[2],d=ctx->s[3],x[16];
    for (int i=0;i<16;i++) x[i]=((uint32_t)blk[i*4])|((uint32_t)blk[i*4+1]<<8)|((uint32_t)blk[i*4+2]<<16)|((uint32_t)blk[i*4+3]<<24);
    STEP(F,a,b,c,d,x[ 0],0xd76aa478, 7); STEP(F,d,a,b,c,x[ 1],0xe8c7b756,12);
    STEP(F,c,d,a,b,x[ 2],0x242070db,17); STEP(F,b,c,d,a,x[ 3],0xc1bdceee,22);
    STEP(F,a,b,c,d,x[ 4],0xf57c0faf, 7); STEP(F,d,a,b,c,x[ 5],0x4787c62a,12);
    STEP(F,c,d,a,b,x[ 6],0xa8304613,17); STEP(F,b,c,d,a,x[ 7],0xfd469501,22);
    STEP(F,a,b,c,d,x[ 8],0x698098d8, 7); STEP(F,d,a,b,c,x[ 9],0x8b44f7af,12);
    STEP(F,c,d,a,b,x[10],0xffff5bb1,17); STEP(F,b,c,d,a,x[11],0x895cd7be,22);
    STEP(F,a,b,c,d,x[12],0x6b901122, 7); STEP(F,d,a,b,c,x[13],0xfd987193,12);
    STEP(F,c,d,a,b,x[14],0xa679438e,17); STEP(F,b,c,d,a,x[15],0x49b40821,22);
    STEP(G,a,b,c,d,x[ 1],0xf61e2562, 5); STEP(G,d,a,b,c,x[ 6],0xc040b340, 9);
    STEP(G,c,d,a,b,x[11],0x265e5a51,14); STEP(G,b,c,d,a,x[ 0],0xe9b6c7aa,20);
    STEP(G,a,b,c,d,x[ 5],0xd62f105d, 5); STEP(G,d,a,b,c,x[10],0x02441453, 9);
    STEP(G,c,d,a,b,x[15],0xd8a1e681,14); STEP(G,b,c,d,a,x[ 4],0xe7d3fbc8,20);
    STEP(G,a,b,c,d,x[ 9],0x21e1cde6, 5); STEP(G,d,a,b,c,x[14],0xc33707d6, 9);
    STEP(G,c,d,a,b,x[ 3],0xf4d50d87,14); STEP(G,b,c,d,a,x[ 8],0x455a14ed,20);
    STEP(G,a,b,c,d,x[13],0xa9e3e905, 5); STEP(G,d,a,b,c,x[ 2],0xfcefa3f8, 9);
    STEP(G,c,d,a,b,x[ 7],0x676f02d9,14); STEP(G,b,c,d,a,x[12],0x8d2a4c8a,20);
    STEP(H,a,b,c,d,x[ 5],0xfffa3942, 4); STEP(H,d,a,b,c,x[ 8],0x8771f681,11);
    STEP(H,c,d,a,b,x[11],0x6d9d6122,16); STEP(H,b,c,d,a,x[14],0xfde5380c,23);
    STEP(H,a,b,c,d,x[ 1],0xa4beea44, 4); STEP(H,d,a,b,c,x[ 4],0x4bdecfa9,11);
    STEP(H,c,d,a,b,x[ 7],0xf6bb4b60,16); STEP(H,b,c,d,a,x[10],0xbebfbc70,23);
    STEP(H,a,b,c,d,x[13],0x289b7ec6, 4); STEP(H,d,a,b,c,x[ 0],0xeaa127fa,11);
    STEP(H,c,d,a,b,x[ 3],0xd4ef3085,16); STEP(H,b,c,d,a,x[ 6],0x04881d05,23);
    STEP(H,a,b,c,d,x[ 9],0xd9d4d039, 4); STEP(H,d,a,b,c,x[12],0xe6db99e5,11);
    STEP(H,c,d,a,b,x[15],0x1fa27cf8,16); STEP(H,b,c,d,a,x[ 2],0xc4ac5665,23);
    STEP(I,a,b,c,d,x[ 0],0xf4292244, 6); STEP(I,d,a,b,c,x[ 7],0x432aff97,10);
    STEP(I,c,d,a,b,x[14],0xab9423a7,15); STEP(I,b,c,d,a,x[ 5],0xfc93a039,21);
    STEP(I,a,b,c,d,x[12],0x655b59c3, 6); STEP(I,d,a,b,c,x[ 3],0x8f0ccc92,10);
    STEP(I,c,d,a,b,x[10],0xffeff47d,15); STEP(I,b,c,d,a,x[ 1],0x85845dd1,21);
    STEP(I,a,b,c,d,x[ 8],0x6fa87e4f, 6); STEP(I,d,a,b,c,x[15],0xfe2ce6e0,10);
    STEP(I,c,d,a,b,x[ 6],0xa3014314,15); STEP(I,b,c,d,a,x[13],0x4e0811a1,21);
    STEP(I,a,b,c,d,x[ 4],0xf7537e82, 6); STEP(I,d,a,b,c,x[11],0xbd3af235,10);
    STEP(I,c,d,a,b,x[ 2],0x2ad7d2bb,15); STEP(I,b,c,d,a,x[ 9],0xeb86d391,21);
    ctx->s[0]+=a; ctx->s[1]+=b; ctx->s[2]+=c; ctx->s[3]+=d;
}

static void md5_update(Md5Ctx *ctx, const uint8_t *data, size_t len) {
    uint32_t lo = ctx->c[0];
    if ((ctx->c[0] = lo + (uint32_t)(len << 3)) < lo) ctx->c[1]++;
    ctx->c[1] += (uint32_t)(len >> 29);
    uint32_t used = (lo >> 3) & 0x3f;
    if (used) {
        uint32_t avail = 64 - used;
        if (len < avail) { memcpy(ctx->buf + used, data, len); return; }
        memcpy(ctx->buf + used, data, avail);
        md5_block(ctx, ctx->buf);
        data += avail; len -= avail;
    }
    while (len >= 64) { md5_block(ctx, data); data += 64; len -= 64; }
    memcpy(ctx->buf, data, len);
}

static void md5_final(Md5Ctx *ctx, uint8_t out[16]) {
    uint32_t used = (ctx->c[0] >> 3) & 0x3f;
    ctx->buf[used++] = 0x80;
    uint32_t avail = 64 - used;
    if (avail < 8) { memset(ctx->buf + used, 0, avail); md5_block(ctx, ctx->buf); used = 0; avail = 64; }
    memset(ctx->buf + used, 0, avail - 8);
    for (int i = 0; i < 4; i++) {
        ctx->buf[56+i] = (uint8_t)(ctx->c[0] >> (i*8));
        ctx->buf[60+i] = (uint8_t)(ctx->c[1] >> (i*8));
    }
    md5_block(ctx, ctx->buf);
    for (int i = 0; i < 4; i++) {
        out[i*4]   = (uint8_t)(ctx->s[i]);
        out[i*4+1] = (uint8_t)(ctx->s[i] >> 8);
        out[i*4+2] = (uint8_t)(ctx->s[i] >> 16);
        out[i*4+3] = (uint8_t)(ctx->s[i] >> 24);
    }
}

void pg_md5_password(const char *pass, const char *user,
                     const uint8_t salt[4], char out[36]) {
    static const char hex[] = "0123456789abcdef";
    Md5Ctx c; uint8_t d[16]; char inner[32];
    md5_init(&c);
    md5_update(&c, (const uint8_t *)pass, strlen(pass));
    md5_update(&c, (const uint8_t *)user, strlen(user));
    md5_final(&c, d);
    for (int i = 0; i < 16; i++) { inner[i*2] = hex[d[i]>>4]; inner[i*2+1] = hex[d[i]&0xf]; }
    md5_init(&c);
    md5_update(&c, (const uint8_t *)inner, 32);
    md5_update(&c, salt, 4);
    md5_final(&c, d);
    out[0]='m'; out[1]='d'; out[2]='5';
    for (int i = 0; i < 16; i++) { out[3+i*2] = hex[d[i]>>4]; out[3+i*2+1] = hex[d[i]&0xf]; }
    out[35] = '\0';
}
