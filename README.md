# Native ANSI C MCP Servers for SQLite, PostgreSQL, Snowflake, and BigQuery

A high-performance, zero-dependency Model Context Protocol (MCP) server written in C11. Operating over Standard I/O (`stdio`), it provides local AI desktop hosts (such as Studio, Claude Desktop, and Claude Code) with secure, high-speed access to SQLite databases.

Built strictly in accordance with the **MCP 2026-07-28 Specification** and **JSON-RPC 2.0**.

---

## Key Features

- **Zero External Dependencies**: Statically compiled for Linux (64-bit ELF) and Windows (64-bit PE32+).
- **Strict Line Budget**: Every source file (`.c` / `.h`) is strictly under 500 lines of code.
- **Environment Credentials**: DB path, access mode, busy timeout, and encryption key are loaded strictly from environment variables.
- **SQL Injection Defense**: Strict parameter binding and table/column name whitelisting.
- **Shared Database Tools**: Servers expose the common query, schema, relationship, ERD, import/export, and health tools. Database-specific unsupported tools are omitted.

---

## Environment Variable Configuration

Configure database settings via environment variables:

| Variable | Type | Default | Description |
|---|---|---|---|
| `SQLITE_DB_PATH` | String | `./database.db` | Absolute or relative path to target SQLite database file. |
| `SQLITE_OPEN_MODE` | String | `READWRITE` | Database access mode (`READONLY` or `READWRITE`). |
| `SQLITE_BUSY_TIMEOUT_MS` | Integer | `5000` | Busy timeout in milliseconds. |
| `SQLITE_ENCRYPTION_KEY` | String | `NULL` | Optional encryption key (memory wiped after init). |

---

## Building

### Windows
Run the automated Windows batch script from **Developer Command Prompt for Visual Studio**:
```cmd
build.bat
```
Alternatively, build with CMake (supports both Debug and Release configurations):
```cmd
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### Linux
Run the automated build script:
```bash
./build.sh
```
Or use `make`:
```bash
make
```

---

## Host Configuration (`claude_desktop_config.json`)

```json
{
  "mcpServers": {
    "sqlite": {
      "command": "/path/to/sqlite-mcp-server",
      "env": {
        "SQLITE_DB_PATH": "/path/to/my_database.db",
        "SQLITE_OPEN_MODE": "READWRITE",
        "SQLITE_BUSY_TIMEOUT_MS": "5000"
      }
    }
  }
}
```

---

## License
MIT License

---

## PostgreSQL MCP Server (`postgres-mcp-server`)

Same MCP loop and tool layer as the SQLite server, with a PostgreSQL backend. Connects via a raw TCP socket or Unix Domain Socket and speaks the PostgreSQL Frontend/Backend Wire Protocol (protocol version 3.0) directly — no libpq or any other external dependency.

Authentication supports **SCRAM-SHA-256** (PostgreSQL 14+ default), **MD5**, and **cleartext** password methods — all implemented from scratch with no external dependencies.

| Variable | Default | Description |
|---|---|---|
| `PGHOST` / `PG_HOST` | `localhost` | Hostname, IP address, or Unix-socket directory (path starting with `/`). |
| `PGPORT` / `PG_PORT` | `5432` | TCP port or socket file suffix. |
| `PGDATABASE` / `PG_DATABASE` | `postgres` | Target database name. |
| `PGUSER` / `PG_USER` | `postgres` | Login role. |
| `PGPASSWORD` / `PG_PASSWORD` | *(none)* | Password (memory-wiped after connect). |
| `PGSSLMODE` / `PG_SSLMODE` | `prefer` | Reserved for future TLS support; currently plain-text only. |
| `PGCONNECT_TIMEOUT` / `PG_CONNECT_TIMEOUT` | `10` | Connection timeout in seconds. |
| `PG_QUERY_TIMEOUT` | `300` | Reserved placeholder (parity with Snowflake server). |
| `PG_ACCESS_MODE` | `READWRITE` | `READONLY` disables `write_query`. |

Differences from the SQLite tools: `backup_database` is not offered; `check_database_health` reports the database, user, server address, version, size, and server time.

```json
{
  "mcpServers": {
    "postgres": {
      "command": "/path/to/postgres-mcp-server",
      "env": {
        "PG_HOST": "localhost",
        "PG_PORT": "5432",
        "PG_DATABASE": "mydb",
        "PG_USER": "myuser",
        "PG_PASSWORD": "secret",
        "PG_ACCESS_MODE": "READONLY"
      }
    }
  }
}
```

---

## Snowflake MCP Server (`snowflake-mcp-server`)

Same MCP loop and tool layer as the SQLite server, with a Snowflake backend. Authentication is Snowflake external-browser SSO (the sign-in runs on the first tool call, so `initialize` and `tools/list` answer immediately). The SSO URL is also printed to stderr.

Unlike the SQLite server it is **not dependency free**: Windows uses WinHTTP (part of the OS), Linux needs libcurl (`libcurl4-openssl-dev`). If libcurl is missing, `build.sh` and CMake skip this target and still build the SQLite server.

| Variable | Default | Description |
|---|---|---|
| `SNOWFLAKE_ACCOUNT` | required | `ORG-ACCOUNT` or `locator[.region]`. |
| `SNOWFLAKE_USER` | required | Snowflake login name (the SSO user). |
| `SNOWFLAKE_ROLE`, `SNOWFLAKE_WAREHOUSE`, `SNOWFLAKE_DATABASE`, `SNOWFLAKE_SCHEMA` | none | Applied with `USE ...` after sign-in. Unquoted names only. |
| `SNOWFLAKE_ACCESS_MODE` | `READWRITE` | `READONLY` disables `write_query`. Use a read-only Snowflake role for real protection. |
| `SNOWFLAKE_CALLBACK_PORT` | `0` | Fixed loopback port for the SSO redirect (0 = OS-chosen). |
| `SNOWFLAKE_SSO_TIMEOUT_SECONDS` | `120` | How long to wait for the browser sign-in. |
| `SNOWFLAKE_QUERY_TIMEOUT_SECONDS` | `300` | How long to wait for a long-running query. |

Differences from the SQLite tools: `backup_database` is not offered; `check_database_health` reports the session (account, region, user, role, warehouse, database, schema, version); `read_query` accepts a single SELECT/WITH/SHOW/DESCRIBE/EXPLAIN/LIST/VALUES statement; results are capped at 1000 rows and only the first result chunk is fetched.

```json
{
  "mcpServers": {
    "snowflake": {
      "command": "/path/to/snowflake-mcp-server",
      "env": {
        "SNOWFLAKE_ACCOUNT": "MYORG-MYACCOUNT",
        "SNOWFLAKE_USER": "me@example.com",
        "SNOWFLAKE_WAREHOUSE": "COMPUTE_WH",
        "SNOWFLAKE_ACCESS_MODE": "READONLY"
      }
    }
  }
}
```

---

## BigQuery MCP Server (`bigquery-mcp-server`)

Uses the BigQuery REST API with Google **end-user OAuth**. On first use, the server opens the system browser for the user to sign in and grant BigQuery access. It receives the authorization callback over a temporary loopback listener and stores the user's refresh token in that operating-system user's config directory. Later launches reuse the cached user grant. Create an OAuth client with application type **Desktop app**. See Google's [installed-app user authorization guide](https://developers.google.com/identity/protocols/oauth2/native-app) and [BigQuery user authentication guide](https://cloud.google.com/bigquery/docs/authentication/end-user-installed).

| Variable | Default | Description |
|---|---|---|
| `BIGQUERY_PROJECT_ID` | required | Google Cloud project that owns query jobs. |
| `BIGQUERY_OAUTH_CLIENT_ID` | required | Desktop OAuth client ID used for the user's sign-in and consent. |
| `BIGQUERY_OAUTH_CLIENT_SECRET` | none | Optional desktop client secret, if provided by the OAuth client configuration. |
| `BIGQUERY_OAUTH_REFRESH_TOKEN` | none | Optional pre-authorized user refresh token; when omitted, the server starts browser sign-in and saves the returned token locally. Treat it as a password. |
| `BIGQUERY_OAUTH_TOKEN_CACHE` | OS user config path | Optional path to the cached user refresh token. By default it is stored under the current user's local application/config directory. |
| `BIGQUERY_HOST` | `bigquery.googleapis.com` | BigQuery API hostname; provide a hostname only, without `https://` or a path. |
| `BIGQUERY_DATASET` | none | Default dataset for unqualified SQL names and one-part table names. |
| `BIGQUERY_LOCATION` | `US` | Query and metadata region, such as `US`, `EU`, or `us-central1`. |
| `BIGQUERY_ACCESS_MODE` | `READWRITE` | `READONLY` disables `write_query` and `import_data`; use IAM roles for server-side enforcement too. |
| `BIGQUERY_TIMEOUT_SECONDS` | `60` | HTTP and query-job completion timeout. |

The server exposes the shared `read_query`, `write_query`, `explain_query`, `describe_table`, `infer_relationships`, `generate_erd`, `list_tables`, `export_data`, `import_data`, and `check_database_health` tools. BigQuery does not provide the local-file `backup_database` operation, so that tool is not advertised. `read_query` accepts SELECT/WITH/EXPLAIN/SHOW/DESCRIBE statements. Query results and local exports are capped at 1,000 rows. Import accepts CSV/TSV and uses BigQuery `tabledata.insertAll`; inserts are not a single atomic transaction, so a failed import can leave earlier rows written. `auto_create_table` creates STRING columns from a CSV header.

Linux builds require libcurl, like the Snowflake server. Windows uses WinHTTP from the Windows SDK.

```json
{
  "mcpServers": {
    "bigquery": {
      "command": "/path/to/bigquery-mcp-server",
      "env": {
        "BIGQUERY_PROJECT_ID": "my-gcp-project",
        "BIGQUERY_DATASET": "analytics",
        "BIGQUERY_LOCATION": "US",
        "BIGQUERY_OAUTH_CLIENT_ID": "your-desktop-client-id.apps.googleusercontent.com",
        "BIGQUERY_OAUTH_CLIENT_SECRET": "optional-desktop-client-secret",
        "BIGQUERY_ACCESS_MODE": "READONLY"
      }
    }
  }
}
```

---

## AWS Athena MCP Server (`athena-mcp-server`)

Uses the Athena HTTPS JSON API directly with ANSI C implementations of AWS Signature Version 4 and IAM Identity Center device authorization. It does not use the AWS SDK or the AWS CLI. Credentials are resolved from `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` / optional `AWS_SESSION_TOKEN`, then from the selected shared AWS profile. Static, temporary, and `sso-session` profiles are supported. A valid token in the standard AWS SSO cache is reused; otherwise the server opens the browser to authorize the configured account and role.

| Variable | Default | Description |
|---|---|---|
| `ATHENA_REGION` | `AWS_REGION`, then `AWS_DEFAULT_REGION` | AWS region containing the Athena workgroup. One of these variables is required. |
| `ATHENA_DATABASE` | `default` | Default Glue Data Catalog database. |
| `ATHENA_CATALOG` | `AwsDataCatalog` | Athena data catalog. |
| `ATHENA_WORKGROUP` | `primary` | Athena workgroup used for queries. |
| `ATHENA_OUTPUT_LOCATION` | workgroup setting | S3 URI for query results, such as `s3://bucket/athena-results/`. Required when the workgroup does not enforce an output location. |
| `ATHENA_ACCESS_MODE` | `READWRITE` | `READONLY` disables `write_query`. IAM remains the authoritative access control. |
| `ATHENA_TIMEOUT_SECONDS` | `300` | Maximum time to wait for an Athena query. |
| `AWS_PROFILE` | `default` | Shared-credentials profile used when environment credentials are absent. |
| `AWS_ACCESS_KEY_ID` | none | IAM access key ID. |
| `AWS_SECRET_ACCESS_KEY` | none | IAM secret access key. |
| `AWS_SESSION_TOKEN` | none | Session token for temporary credentials. |
| `AWS_SHARED_CREDENTIALS_FILE` | standard AWS path | Optional credentials-file override. |
| `AWS_CONFIG_FILE` | standard AWS path | Optional AWS config-file override; static credentials in profile sections are supported. |

### Authentication

Credential resolution uses this order:

1. `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`, plus `AWS_SESSION_TOKEN` for temporary credentials.
2. Static credentials in the `AWS_PROFILE` section of the shared credentials or config file.
3. An IAM Identity Center profile linked to an `[sso-session ...]` section.

For IAM Identity Center, the profile supplies the account and role while the referenced SSO session supplies the start URL and SSO region:

```ini
[profile analytics]
sso_session = company
sso_account_id = 111122223333
sso_role_name = DataAnalyst
region = eu-west-1

[sso-session company]
sso_start_url = https://company.awsapps.com/start
sso_region = eu-west-1
sso_registration_scopes = sso:account:access
```

Set `AWS_PROFILE=analytics`. The server reuses a matching token from the standard `~/.aws/sso/cache` directory. If the token is absent, expired, or rejected, it starts the IAM Identity Center device-authorization flow, prints the verification URL to stderr, and opens the default browser. It then exchanges the bearer token for temporary credentials for `sso_account_id` and `sso_role_name`. The AWS SDK and AWS CLI are not required.

Do not place long-lived access keys directly in the MCP configuration when a shared profile or IAM Identity Center is available.

### IAM and S3 requirements

The resolved identity normally needs:

- `athena:StartQueryExecution`, `athena:GetQueryExecution`, and `athena:GetQueryResults` on the selected workgroup.
- Glue catalog read permissions such as `glue:GetDatabase`, `glue:GetDatabases`, `glue:GetTable`, `glue:GetTables`, and `glue:GetPartitions` for the queried catalog objects.
- Read access to source data in S3.
- `s3:GetBucketLocation`, `s3:PutObject`, and `s3:GetObject` for the Athena query-results location.
- Relevant KMS permissions when source data or query results use customer-managed KMS keys.

`ATHENA_OUTPUT_LOCATION` may be omitted when the workgroup enforces its own result configuration. Otherwise, set it to a writable `s3://.../` location.

### Tools and behavior

The server exposes `read_query`, `write_query`, `explain_query`, `describe_table`, `infer_relationships`, `generate_erd`, `list_tables`, `export_data`, `import_data`, and `check_database_health`. `backup_database` is not advertised.

- Queries use `StartQueryExecution`, poll `GetQueryExecution`, and retrieve paginated results with `GetQueryResults`.
- A unique Athena `ClientRequestToken` is generated automatically for every query.
- Returned and locally exported results are capped at 1,000 rows.
- `read_query` accepts `SELECT`, `WITH`, `SHOW`, `DESCRIBE`, and `EXPLAIN` statements.
- `ATHENA_ACCESS_MODE=READONLY` rejects `write_query`; IAM policies remain the authoritative protection.
- Local export supports JSON, CSV, and TSV without compression.
- Athena cannot directly upload a local file as table rows. `import_data` returns guidance to upload the data to S3 and create or update a Glue/Athena table.
- Athena/Glue does not expose relational primary-key and foreign-key constraints. Relationship and ERD output therefore cannot infer database-enforced relationships.

### MCP configuration

```json
{
  "mcpServers": {
    "athena": {
      "command": "/path/to/athena-mcp-server",
      "env": {
        "ATHENA_REGION": "eu-west-1",
        "ATHENA_DATABASE": "analytics",
        "ATHENA_WORKGROUP": "primary",
        "ATHENA_OUTPUT_LOCATION": "s3://my-query-results/athena/",
        "ATHENA_ACCESS_MODE": "READONLY",
        "AWS_PROFILE": "default"
      }
    }
  }
}
```

If startup reports that no static credentials were found for an SSO profile, verify that the profile contains `sso_session`, `sso_account_id`, and `sso_role_name`, and that the referenced `[sso-session ...]` section contains `sso_start_url` and `sso_region`. Restart the MCP host after changing either AWS configuration files or MCP environment variables.
