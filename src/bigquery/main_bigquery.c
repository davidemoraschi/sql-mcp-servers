/* BigQuery MCP server entry point. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "db_bigquery.h"
#include "rdbms_tools.h"
#include "mcp_loop.h"

static const char *env_value(const char *name) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

static int env_int(const char *name, int fallback) {
    const char *v = env_value(name);
    return v ? atoi(v) : fallback;
}

static bool env_readonly(const char *name) {
    const char *v = env_value(name);
    char normalized[32]; size_t i = 0;
    if (!v || strlen(v) >= sizeof(normalized)) return false;
    for (; v[i]; i++) normalized[i] = (char)toupper((unsigned char)v[i]);
    normalized[i] = '\0';
    return strcmp(normalized, "READONLY") == 0 || strcmp(normalized, "READ_ONLY") == 0;
}

int main(void) {
    mcp_configure_stdio();
    fprintf(stderr, "[INFO] Starting BigQuery MCP Server (GoogleSQL / OAuth 2.0)...\n");

    McpProfile profile = {
        "bigquery-mcp-server", "BigQuery",
        "Check BigQuery API connectivity, current project, user, and location", false
    };
    tools_set_profile(&profile);

    BqConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host = env_value("BIGQUERY_HOST");
    cfg.project_id = env_value("BIGQUERY_PROJECT_ID");
    cfg.dataset = env_value("BIGQUERY_DATASET");
    cfg.location = env_value("BIGQUERY_LOCATION");
    cfg.oauth_client_id = env_value("BIGQUERY_OAUTH_CLIENT_ID");
    cfg.oauth_client_secret = env_value("BIGQUERY_OAUTH_CLIENT_SECRET");
    cfg.oauth_refresh_token = env_value("BIGQUERY_OAUTH_REFRESH_TOKEN");
    cfg.read_only = env_readonly("BIGQUERY_ACCESS_MODE");
    cfg.timeout_seconds = env_int("BIGQUERY_TIMEOUT_SECONDS", 60);

    char err[512];
    if (!bq_db_init(&cfg, err, sizeof(err))) {
        fprintf(stderr, "[FATAL] BigQuery configuration error: %s\n", err);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "[INFO] BigQuery project %s (%s). User OAuth sign-in is performed on first use when no refresh token is cached.\n",
            cfg.project_id, cfg.read_only ? "READONLY" : "READWRITE");

    int rc = mcp_serve_stdio();
    bq_db_close();
    fprintf(stderr, "[INFO] BigQuery MCP Server stopped.\n");
    return rc;
}
