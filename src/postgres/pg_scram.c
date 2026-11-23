/*
 * pg_scram.c - SCRAM-SHA-256 for PostgreSQL wire protocol (RFC 5802)
 *
 * Self-contained: SHA-256, HMAC-SHA-256, PBKDF2-SHA-256, base64.
 * Line budget constraint: < 500 lines
 */
#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "pg_scram.h"

/* ------------------------------------------------------------------ */
/* SHA-256                                                              */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t s[8]; uint64_t len; uint8_t buf[64]; uint32_t blen; } Sha256;

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(e,f,g)  (((e)&(f))^((~(e))&(g)))
#define MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define EP0(a) (ROR32(a,2)^ROR32(a,13)^ROR32(a,22))
#define EP1(e) (ROR32(e,6)^ROR32(e,11)^ROR32(e,25))
#define SIG0(x) (ROR32(x,7)^ROR32(x,18)^((x)>>3))
#define SIG1(x) (ROR32(x,17)^ROR32(x,19)^((x)>>10))

static void sha256_init(Sha256 *c) {
    c->s[0]=0x6a09e667; c->s[1]=0xbb67ae85; c->s[2]=0x3c6ef372; c->s[3]=0xa54ff53a;
    c->s[4]=0x510e527f; c->s[5]=0x9b05688c; c->s[6]=0x1f83d9ab; c->s[7]=0x5be0cd19;
    c->len=0; c->blen=0;
}

static void sha256_block(Sha256 *c, const uint8_t *blk) {
    uint32_t w[64],a,b,d,e,f,g,h,t1,t2;
    uint32_t cc=c->s[2]; /* avoid shadowing 'c' param name */
    for (int i=0;i<16;i++) w[i]=((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|((uint32_t)blk[i*4+2]<<8)|(uint32_t)blk[i*4+3];
    for (int i=16;i<64;i++) w[i]=SIG1(w[i-2])+w[i-7]+SIG0(w[i-15])+w[i-16];
    a=c->s[0];b=c->s[1];cc=c->s[2];d=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for (int i=0;i<64;i++) {
        t1=h+EP1(e)+CH(e,f,g)+K[i]+w[i];
        t2=EP0(a)+MAJ(a,b,cc);
        h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;
    }
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cc;c->s[3]+=d;
    c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}

static void sha256_update(Sha256 *c, const uint8_t *data, size_t len) {
    c->len += len;
    while (len > 0) {
        size_t room = 64 - c->blen, take = len < room ? len : room;
        memcpy(c->buf + c->blen, data, take);
        c->blen += (uint32_t)take; data += take; len -= take;
        if (c->blen == 64) { sha256_block(c, c->buf); c->blen = 0; }
    }
}

static void sha256_final(Sha256 *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    while (c->blen != 56) { pad=0; sha256_update(c,&pad,1); }
    uint8_t lb[8];
    for (int i=7;i>=0;i--) { lb[i]=(uint8_t)(bits&0xff); bits>>=8; }
    sha256_update(c, lb, 8);
    for (int i=0;i<8;i++) { out[i*4]=(uint8_t)(c->s[i]>>24); out[i*4+1]=(uint8_t)(c->s[i]>>16); out[i*4+2]=(uint8_t)(c->s[i]>>8); out[i*4+3]=(uint8_t)c->s[i]; }
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    Sha256 c; sha256_init(&c); sha256_update(&c,data,len); sha256_final(&c,out);
}

/* ------------------------------------------------------------------ */
/* HMAC-SHA-256                                                         */
/* ------------------------------------------------------------------ */
static void hmac_sha256(const uint8_t *key, size_t klen,
                        const uint8_t *msg, size_t mlen, uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], inner[32];
    memset(k,0,64);
    if (klen>64) { sha256(key,klen,k); }
    else memcpy(k,key,klen);
    for (int i=0;i<64;i++) { ipad[i]=k[i]^0x36; opad[i]=k[i]^0x5c; }
    Sha256 c;
    sha256_init(&c); sha256_update(&c,ipad,64); sha256_update(&c,msg,mlen); sha256_final(&c,inner);
    sha256_init(&c); sha256_update(&c,opad,64); sha256_update(&c,inner,32); sha256_final(&c,out);
}

/* ------------------------------------------------------------------ */
/* PBKDF2-SHA-256                                                       */
/* ------------------------------------------------------------------ */
static void pbkdf2_sha256(const uint8_t *pass, size_t plen,
                          const uint8_t *salt, size_t slen,
                          uint32_t iters, uint8_t out[32]) {
    /* Single block (dkLen=32): U1 = HMAC(pass, salt || INT(1)) */
    uint8_t *buf = (uint8_t *)malloc(slen + 4);
    if (!buf) { memset(out,0,32); return; }
    memcpy(buf, salt, slen);
    buf[slen]=0; buf[slen+1]=0; buf[slen+2]=0; buf[slen+3]=1;
    uint8_t u[32], acc[32];
    hmac_sha256(pass, plen, buf, slen+4, u);
    free(buf);
    memcpy(acc, u, 32);
    for (uint32_t i = 1; i < iters; i++) {
        hmac_sha256(pass, plen, u, 32, u);
        for (int j=0;j<32;j++) acc[j]^=u[j];
    }
    memcpy(out, acc, 32);
}

/* ------------------------------------------------------------------ */
/* Base64                                                               */
/* ------------------------------------------------------------------ */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_enc_len(size_t n) { return ((n+2)/3)*4; }

static void b64_encode(const uint8_t *src, size_t slen, char *dst) {
    size_t i=0, o=0;
    for (; i+2<slen; i+=3) {
        dst[o++]=B64[src[i]>>2];
        dst[o++]=B64[((src[i]&3)<<4)|(src[i+1]>>4)];
        dst[o++]=B64[((src[i+1]&0xf)<<2)|(src[i+2]>>6)];
        dst[o++]=B64[src[i+2]&0x3f];
    }
    if (i<slen) {
        dst[o++]=B64[src[i]>>2];
        if (i+1<slen) { dst[o++]=B64[((src[i]&3)<<4)|(src[i+1]>>4)]; dst[o++]=B64[(src[i+1]&0xf)<<2]; }
        else           { dst[o++]=B64[(src[i]&3)<<4]; dst[o++]='='; }
        dst[o++]='=';
    }
    dst[o]='\0';
}

static int b64_char(char c) {
    if (c>='A'&&c<='Z') return c-'A';
    if (c>='a'&&c<='z') return c-'a'+26;
    if (c>='0'&&c<='9') return c-'0'+52;
    if (c=='+') return 62; if (c=='/') return 63; return -1;
}

/* Returns decoded length, or -1 on error. dst must be >= (slen/4)*3 bytes. */
static int b64_decode(const char *src, size_t slen, uint8_t *dst) {
    size_t o=0;
    for (size_t i=0;i+3<slen;i+=4) {
        int a=b64_char(src[i]),b=b64_char(src[i+1]),c=b64_char(src[i+2]),d=b64_char(src[i+3]);
        if (a<0||b<0) return -1;
        dst[o++]=(uint8_t)((a<<2)|(b>>4));
        if (src[i+2]!='=') { if (c<0) return -1; dst[o++]=(uint8_t)((b<<4)|(c>>2)); }
        if (src[i+3]!='=') { if (d<0) return -1; dst[o++]=(uint8_t)((c<<6)|d); }
    }
    return (int)o;
}

/* ------------------------------------------------------------------ */
/* SCRAM-SHA-256 exchange                                               */
/* ------------------------------------------------------------------ */

/* Generate 18 random bytes encoded as base64 (24 chars + NUL). */
static void make_cnonce(char out[25]) {
    uint8_t raw[18];
    /* Use a mix of address entropy + counter as a simple nonce source */
    static uint64_t ctr = 0;
    uint64_t seed = (uint64_t)(uintptr_t)out ^ ++ctr;
    /* xorshift64 */
    for (int i=0;i<18;i++) { seed^=seed<<13; seed^=seed>>7; seed^=seed<<17; raw[i]=(uint8_t)seed; }
    b64_encode(raw, 18, out);
}

char *scram_client_first(char cnonce_out[25]) {
    make_cnonce(cnonce_out);
    /* gs2-header = "n,," ; client-first-body = "n=,r=<cnonce>" */
    size_t len = 3 + 3 + 24 + 1; /* "n,,n=,r=" + cnonce + NUL */
    char *msg = (char *)malloc(len + 8);
    if (!msg) return NULL;
    snprintf(msg, len+8, "n,,n=,r=%s", cnonce_out);
    return msg;
}

char *scram_client_final(const char *server_first, const char *password,
                         const char *cnonce, uint8_t server_sig_out[32]) {
    /* Parse server-first: r=<nonce>,s=<salt-b64>,i=<iters> */
    const char *rp = strstr(server_first, "r=");
    const char *sp = strstr(server_first, "s=");
    const char *ip = strstr(server_first, "i=");
    if (!rp || !sp || !ip) return NULL;

    /* Extract full nonce */
    rp += 2;
    size_t nonce_len = strcspn(rp, ",");
    char *nonce = (char *)malloc(nonce_len+1);
    if (!nonce) return NULL;
    memcpy(nonce, rp, nonce_len); nonce[nonce_len]='\0';

    /* Verify cnonce prefix */
    size_t clen = strlen(cnonce);
    if (nonce_len < clen || memcmp(nonce, cnonce, clen) != 0) { free(nonce); return NULL; }

    /* Extract salt */
    sp += 2;
    size_t salt_b64_len = strcspn(sp, ",");
    uint8_t salt[64]; int salt_len = b64_decode(sp, salt_b64_len, salt);
    if (salt_len < 0) { free(nonce); return NULL; }

    /* Extract iterations */
    uint32_t iters = (uint32_t)atoi(ip+2);
    if (iters == 0) { free(nonce); return NULL; }

    /* SaltedPassword = PBKDF2(password, salt, iters, 32) */
    uint8_t salted_pw[32];
    pbkdf2_sha256((const uint8_t *)password, strlen(password),
                  salt, (size_t)salt_len, iters, salted_pw);

    /* ClientKey = HMAC(SaltedPassword, "Client Key") */
    uint8_t client_key[32], stored_key[32], client_sig[32], client_proof[32];
    hmac_sha256(salted_pw, 32, (const uint8_t *)"Client Key", 10, client_key);
    sha256(client_key, 32, stored_key);

    /* channel-binding = base64("n,,") = "biws" */
    /* client-final-message-without-proof */
    size_t cfwp_len = 8 + nonce_len + 1; /* "c=biws,r=<nonce>" */
    char *cfwp = (char *)malloc(cfwp_len + 8);
    if (!cfwp) { free(nonce); return NULL; }
    snprintf(cfwp, cfwp_len+8, "c=biws,r=%s", nonce);

    /* AuthMessage = client-first-bare + "," + server-first + "," + cfwp */
    size_t cfb_len = 4 + clen; /* "n=,r=<cnonce>" */
    char *cfb = (char *)malloc(cfb_len + 8);
    if (!cfb) { free(nonce); free(cfwp); return NULL; }
    snprintf(cfb, cfb_len+8, "n=,r=%s", cnonce);

    size_t auth_len = strlen(cfb) + 1 + strlen(server_first) + 1 + strlen(cfwp) + 1;
    char *auth_msg = (char *)malloc(auth_len);
    if (!auth_msg) { free(nonce); free(cfwp); free(cfb); return NULL; }
    snprintf(auth_msg, auth_len, "%s,%s,%s", cfb, server_first, cfwp);
    free(cfb);

    /* ClientSignature = HMAC(StoredKey, AuthMessage) */
    hmac_sha256(stored_key, 32, (const uint8_t *)auth_msg, strlen(auth_msg), client_sig);

    /* ClientProof = ClientKey XOR ClientSignature */
    for (int i=0;i<32;i++) client_proof[i] = client_key[i] ^ client_sig[i];

    /* ServerKey = HMAC(SaltedPassword, "Server Key") */
    uint8_t server_key[32];
    hmac_sha256(salted_pw, 32, (const uint8_t *)"Server Key", 10, server_key);
    /* ServerSignature = HMAC(ServerKey, AuthMessage) */
    hmac_sha256(server_key, 32, (const uint8_t *)auth_msg, strlen(auth_msg), server_sig_out);
    free(auth_msg);

    /* Encode proof */
    size_t proof_b64_len = b64_enc_len(32);
    char *proof_b64 = (char *)malloc(proof_b64_len + 1);
    if (!proof_b64) { free(nonce); free(cfwp); return NULL; }
    b64_encode(client_proof, 32, proof_b64);

    /* client-final = cfwp + ",p=" + proof_b64 */
    size_t final_len = strlen(cfwp) + 3 + proof_b64_len + 1;
    char *final_msg = (char *)malloc(final_len);
    if (final_msg) snprintf(final_msg, final_len, "%s,p=%s", cfwp, proof_b64);
    free(nonce); free(cfwp); free(proof_b64);
    return final_msg;
}

bool scram_verify_server(const char *server_final, const uint8_t expected[32]) {
    const char *vp = strstr(server_final, "v=");
    if (!vp) return false;
    vp += 2;
    size_t vlen = strlen(vp);
    uint8_t sig[32];
    int n = b64_decode(vp, vlen, sig);
    return n == 32 && memcmp(sig, expected, 32) == 0;
}
