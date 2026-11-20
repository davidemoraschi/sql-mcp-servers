/*
 * main_snowflake.c - Native ANSI C / C11 Snowflake MCP Server
 *
 * Reuses the MCP loop and tool layer of the SQLite server; only the
 * backend (db_snowflake.c) and this configuration entry point differ.
 * Authentication is Snowflake external-browser SSO, run on the first query.
 *
 * Line budget constraint: < 350 lines
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "db_snowflake.h"
#include "rdbms_tools.h"
#include "mcp_loop.h"

static const char *env_or_null(const char *name) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

static int env_int(const char *name, int fallback) {
    const char *v = env_or_null(name);
    return v ? atoi(v) : fallback;
}

static bool iequals(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return false;
    }
    return *a == *b;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    mcp_configure_stdio();

    fprintf(stderr, "[INFO] Starting Snowflake MCP Server (Dual-Era 2024-11-05 / 2026-07-28 / JSON-RPC 2.0)...\n");

    McpProfile profile;
    profile.server_name = "snowflake-mcp-server";
    profile.db_label = "Snowflake";
    profile.health_desc = "Check Snowflake connectivity and report the current account, region, user, role, warehouse, database, schema and version";
    profile.has_backup = false;
    tools_set_profile(&profile);

    SfSessionConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.account = env_or_null("SNOWFLAKE_ACCOUNT");
    cfg.user = env_or_null("SNOWFLAKE_USER");
    cfg.role = env_or_null("SNOWFLAKE_ROLE");
    cfg.warehouse = env_or_null("SNOWFLAKE_WAREHOUSE");
    cfg.database = env_or_null("SNOWFLAKE_DATABASE");
    cfg.schema = env_or_null("SNOWFLAKE_SCHEMA");
    cfg.callback_port = (unsigned short)env_int("SNOWFLAKE_CALLBACK_PORT", 0);
    cfg.sso_timeout_seconds = env_int("SNOWFLAKE_SSO_TIMEOUT_SECONDS", 120);
    cfg.query_timeout_seconds = env_int("SNOWFLAKE_QUERY_TIMEOUT_SECONDS", 300);

    const char *mode = env_or_null("SNOWFLAKE_ACCESS_MODE");
    bool read_only = mode && iequals(mode, "READONLY");

    char err[256];
    if (!sf_db_init(&cfg, read_only, err, sizeof(err))) {
        fprintf(stderr, "[FATAL] Snowflake configuration error: %s\n", err);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "[INFO] Snowflake account %s, user %s (%s). Browser sign-in happens on the first query.\n",
            cfg.account, cfg.user, read_only ? "READONLY" : "READWRITE");

    int rc = mcp_serve_stdio();

    sf_db_close();
    fprintf(stderr, "[INFO] Snowflake MCP Server stopped.\n");
    return rc;
}
