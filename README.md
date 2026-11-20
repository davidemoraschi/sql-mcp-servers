# Standalone Native ANSI C SQLite MCP Server

A high-performance, zero-dependency Model Context Protocol (MCP) server written in C11. Operating over Standard I/O (`stdio`), it provides local AI desktop hosts (such as Studio, Claude Desktop, and Claude Code) with secure, high-speed access to SQLite databases.

Built strictly in accordance with the **MCP 2026-07-28 Specification** and **JSON-RPC 2.0**.

---

## Key Features

- **Zero External Dependencies**: Statically compiled for Linux (64-bit ELF) and Windows (64-bit PE32+).
- **Strict Line Budget**: Every source file (`.c` / `.h`) is strictly under 500 lines of code.
- **Environment Credentials**: DB path, access mode, busy timeout, and encryption key are loaded strictly from environment variables.
- **SQL Injection Defense**: Strict parameter binding and table/column name whitelisting.
- **Portable RDBMS Tools**: Includes tools for `read_query`, `write_query`, `backup_database`, `explain_query`, `describe_table`, `infer_relationships`, `generate_erd`, `list_tables`, `export_data`, and `check_database_health`.

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
