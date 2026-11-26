#ifndef AWS_SHA256_H
#define AWS_SHA256_H
#include <stddef.h>
#include <stdint.h>
void aws_sha256(const unsigned char *data, size_t len, unsigned char out[32]);
void aws_hmac_sha256(const unsigned char *key, size_t key_len,
                     const unsigned char *data, size_t len, unsigned char out[32]);
void aws_hex(const unsigned char *data, size_t len, char *out);
#endif
