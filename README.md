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
