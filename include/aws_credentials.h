#ifndef AWS_CREDENTIALS_H
#define AWS_CREDENTIALS_H
#include <stdbool.h>
#include <stddef.h>
typedef struct { char *access_key, *secret_key, *session_token; } AwsCredentials;
bool aws_credentials_load(const char *profile, AwsCredentials *out, char *err, size_t err_size);
void aws_credentials_free(AwsCredentials *creds);
#endif
