/*
 * main_postgres.c - Native C PostgreSQL MCP Server
 *
 * Reuses the MCP loop and tool layer; only the backend (db_postgres.c)
 * and this configuration entry point differ.
 * Connects via raw TCP or Unix Domain Socket using the PostgreSQL
 * Frontend/Backend Wire Protocol (no libpq).
 *
 * Line budget constraint: < 350 lines
 */
#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "db_postgres.h"
#include "rdbms_tools.h"
#include "mcp_loop.h"

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : fallback;
}

static int env_int(const char *name, int fallback) {
    const char *v = getenv(name);
    return (v && v[0]) ? atoi(v) : fallback;
}

static bool iequals(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    mcp_configure_stdio();

    fprintf(stderr, "[INFO] Starting PostgreSQL MCP Server (2026-07-28 / JSON-RPC 2.0)...\n");

    McpProfile profile;
    profile.server_name = "postgres-mcp-server";
    profile.db_label    = "PostgreSQL";
    profile.health_desc = "Check PostgreSQL connectivity and report database, user, server address, version, size, and time";
    profile.has_backup  = false;
    tools_set_profile(&profile);

    const char *mode = getenv("PG_ACCESS_MODE");
    bool read_only = mode && iequals(mode, "READONLY");

    /* Accept both standard libpq names (PGHOST etc.) and PG_* prefixed names.
       Standard names take priority when both are set. */
    PgConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host                    = env_or("PGHOST",            env_or("PG_HOST",     "localhost"));
    cfg.port                    = env_or("PGPORT",            env_or("PG_PORT",     "5432"));
    cfg.dbname                  = env_or("PGDATABASE",        env_or("PG_DATABASE", "postgres"));
    cfg.user                    = env_or("PGUSER",            env_or("PG_USER",     "postgres"));
    cfg.password                = env_or("PGPASSWORD",        env_or("PG_PASSWORD", NULL));
    cfg.sslmode                 = env_or("PGSSLMODE",         env_or("PG_SSLMODE",  "prefer"));
    cfg.connect_timeout_seconds = env_int("PGCONNECT_TIMEOUT",env_int("PG_CONNECT_TIMEOUT", 10));
    cfg.query_timeout_seconds   = env_int("PG_QUERY_TIMEOUT", 300);
    cfg.read_only               = read_only;

    char err[512];
    if (!pg_db_init(&cfg, err, sizeof(err))) {
        fprintf(stderr, "[FATAL] PostgreSQL connection failed: %s\n", err);
        if (cfg.password) mcp_wipe_memory((void *)cfg.password, strlen(cfg.password));
        return EXIT_FAILURE;
    }

    if (cfg.password) {
        mcp_wipe_memory((void *)cfg.password, strlen(cfg.password));
        fprintf(stderr, "[SECURITY] Password memory wiped.\n");
    }

    fprintf(stderr, "[INFO] Connected to PostgreSQL: %s@%s:%s/%s (%s)\n",
            cfg.user, cfg.host, cfg.port, cfg.dbname,
            read_only ? "READONLY" : "READWRITE");

    int rc = mcp_serve_stdio();

    pg_db_close();
    fprintf(stderr, "[INFO] PostgreSQL MCP Server stopped.\n");
    return rc;
}
