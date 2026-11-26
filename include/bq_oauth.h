#ifndef BQ_OAUTH_H
#define BQ_OAUTH_H

#include <stdbool.h>
#include <stddef.h>

bool bq_oauth_user_login(const char *client_id, const char *client_secret,
                         int timeout_seconds, char **refresh_token,
                         char **access_token, int *expires_in,
                         char *err, size_t err_size);
char *bq_oauth_load_cached_refresh_token(void);
void bq_oauth_save_cached_refresh_token(const char *token);

#endif
