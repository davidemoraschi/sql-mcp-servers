#ifndef AWS_SIGV4_H
#define AWS_SIGV4_H
#include <stdbool.h>
#include <stddef.h>
#include "aws_credentials.h"
bool aws_sigv4_headers(const AwsCredentials *creds, const char *region,
                       const char *service, const char *host, const char *target,
                       const char *content_type, const char *body,
                       char **headers, char *err, size_t err_size);
#endif
