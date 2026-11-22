/*
 * pg_scram.h - SCRAM-SHA-256 authentication for PostgreSQL wire protocol
 *
 * Line budget constraint: < 150 lines
 */
#ifndef PG_SCRAM_H
#define PG_SCRAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Build the client-first-message and return it as a malloc'd string.
 * cnonce_out receives the 18-byte base64 client nonce (NUL-terminated, 25 chars).
 * Caller frees the returned string.
 */
char *scram_client_first(char cnonce_out[25]);

/*
 * Given the server-first-message, the password, and the cnonce used in
 * client-first, compute and return the client-final-message as a malloc'd
 * string.  server_sig_out receives the expected server signature (32 bytes).
 * Returns NULL on error.
 */
char *scram_client_final(const char *server_first, const char *password,
                         const char *cnonce, uint8_t server_sig_out[32]);

/* Verify the server signature from AuthenticationSASLFinal. */
bool scram_verify_server(const char *server_final, const uint8_t expected[32]);

#endif /* PG_SCRAM_H */
