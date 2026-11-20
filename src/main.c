/*
 * main.c - Native ANSI C / C11 SQLite MCP Server
 *
 * Line budget constraint: < 350 lines
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "db_sqlite.h"
#include "mcp_loop.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    mcp_configure_stdio();

    fprintf(stderr, "[INFO] Starting SQLite MCP Server (Dual-Era 2024-11-05 / 2026-07-28 / JSON-RPC 2.0)...\n");

    const char *env_path = getenv("SQLITE_DB_PATH");
    const char *env_mode = getenv("SQLITE_OPEN_MODE");
    const char *env_timeout = getenv("SQLITE_BUSY_TIMEOUT_MS");
    char *env_key = getenv("SQLITE_ENCRYPTION_KEY");

    DbConfig config;
    config.db_path = env_path ? (char *)env_path : "./database.db";
    config.open_mode = env_mode ? (char *)env_mode : "READWRITE";
    config.busy_timeout_ms = env_timeout ? atoi(env_timeout) : 5000;
    config.encryption_key = env_key;

    if (!db_init(&config)) {
        fprintf(stderr, "[FATAL] Failed to initialize SQLite database at %s\n", config.db_path);
        if (env_key) mcp_wipe_memory(env_key, strlen(env_key));
        return EXIT_FAILURE;
    }

    if (env_key) {
        mcp_wipe_memory(env_key, strlen(env_key));
        fprintf(stderr, "[SECURITY] Encryption key memory wiped.\n");
    }

    fprintf(stderr, "[INFO] Database connected cleanly: %s\n", config.db_path);

    int rc = mcp_serve_stdio();

    db_close();
    fprintf(stderr, "[INFO] SQLite MCP Server stopped.\n");
    return rc;
}
