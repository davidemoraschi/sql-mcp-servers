/*
 * pg_md5.h - Minimal self-contained MD5 for PostgreSQL MD5 password auth
 *
 * Line budget constraint: < 150 lines
 */
#ifndef PG_MD5_H
#define PG_MD5_H

#include <stdint.h>
#include <stddef.h>

/* Compute md5(md5(password + user) + salt) and write "md5<hex32>\0" to out[36]. */
void pg_md5_password(const char *pass, const char *user,
                     const uint8_t salt[4], char out[36]);

#endif /* PG_MD5_H */
