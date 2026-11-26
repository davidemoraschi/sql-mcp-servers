#ifndef AWS_SSO_H
#define AWS_SSO_H
#include <stdbool.h>
#include <stddef.h>
#include "aws_credentials.h"
bool aws_sso_credentials_load(const char *profile, AwsCredentials *out,
                              char *err, size_t err_size);
#endif
