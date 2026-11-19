# Requirements Specification: Standalone ANSI C SQLite Model Context Protocol (MCP) Server (v8)

## 1. System Purpose and Scope

This specification establishes the mandatory technical, architectural, security, and performance requirements for a native, zero-dependency Model Context Protocol (MCP) server written in ANSI C / C11. The application provides local desktop AI hosts—such as Claude Desktop, Claude Code, and Studio—with secure, high-speed access to SQLite relational databases exclusively over Standard Input/Output (`stdio`) Inter-Process Communication (IPC) transport.

Operating under the stateless protocol architecture introduced in the **MCP 2026-07-28 Specification** alongside backward compatibility for the **MCP 2025-11-25 Specification**, the server processes every incoming request independently without relying on hidden transport-layer sessions. The system is engineered to achieve sub-15 millisecond tool call latency, complete immunity to SQL parameter injection, a minimal binary footprint under 2.5 megabytes, and seamless native execution across 64-bit Linux and Windows operating systems without requiring third-party runtime dynamic libraries.

---

## 2. Platform, Target Architecture, and Binary Constraints

The server must operate directly as an isolated child process spawned by the host application over `stdio` IPC.

```
[Host AI Application (Studio / Claude / VS)]
        │
        ▼ (STDIO Pipe - Plaintext JSON-RPC 2.0)
┌─────────────────────────────────────────────────────────┐
│ Self-Contained 64-Bit Executable (sqlite-mcp-server)   │
│                                                         │
│  ┌────────────────────┐     ┌────────────────────────┐  │
│  │ JSON-RPC Engine    │ ──► │ MCP Tool Handlers      │  │
│  └────────────────────┘     └────────────────────────┘  │
│                                          │              │
│                                          ▼              │
│                             ┌────────────────────────┐  │
│                             │ Embedded SQLite Engine │  │
│                             │ (sqlite3.c / WAL Mode) │  │
│                             └────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
        │
        ▼
   [Local Database File (Chinook_Sqlite.sqlite / .db)]
```

### 2.1 Target Operating Systems and Binary Portability
* **Linux Target**: 64-bit ELF executable (`x86_64-unknown-linux-gnu`), fully compatible with Linux kernel 3.2+ and glibc 2.17+ or musl libc.
* **Windows Target**: 64-bit PE32+ executable (`x86_64-pc-windows-msvc` or `x86_64-w64-mingw32`), fully compatible with Windows 10, Windows 11, and Windows Server 2019+.

### 2.2 Dependency Isolation & Engine Embedding
* **Embedded Database Engine**: The server statically compiles the official SQLite single-file amalgamation (`sqlite3.c` and `sqlite3.h`) directly into the executable binary.
* **System Library Bounds**: Operating system linkages are restricted exclusively to standard platform libraries (`libc.so`, `libm.so`, `libpthread.so` on Linux; `kernel32.dll`, `ucrtbase.dll` / `msvcrt.dll` on Windows).

### 2.3 Standard I/O (STDIO) Transport Isolation Guardrails
* **Standard Output (`stdout`) Restriction**: Standard output is strictly allocated to outbound JSON-RPC framing.
* **Standard Error (`stderr`) Redirection**: All application logging and diagnostic traces are directed to `stderr`.
* **Windows Binary Stream Mode**: Process initialization configures stdin/stdout file streams to binary mode (`_O_BINARY`) on Windows platforms.

---

## 3. Configuration and Environment Credentials

The C server reads configuration settings exclusively from operating system environment variables during process initialization inside `main()`:

| Environment Variable | Target Data Type | Default Fallback | Purpose & Specification |
| :--- | :--- | :--- | :--- |
| `SQLITE_DB_PATH` | File Path String | `./database.db` | Absolute or relative file path to the target SQLite database file (e.g. `Chinook_Sqlite.sqlite`). |
| `SQLITE_OPEN_MODE` | String Enum (`READONLY` / `READWRITE`) | `READWRITE` | Controls SQLite database connection access mode. |
| `SQLITE_BUSY_TIMEOUT_MS` | Integer (Milliseconds) | `5000` | Sets database lock acquisition wait timeout. |
| `SQLITE_ENCRYPTION_KEY` | Plaintext String | `NULL` | Optional passphrase for encrypted SQLite extensions. |

---

## 4. Dual-Era Protocol Handshake & Version Fallback Compliance

The server operates as a **Dual-Era MCP Server** supporting both modern stateless (`2026-07-28`) and legacy stateful (`2025-11-25`) protocol versions:

1. **Legacy Handshake Support (`2025-11-25`)**:
   * Accepts `"method": "initialize"` and responds with a `2025-11-25` initialize result envelope (`protocolVersion: "2025-11-25"`).
   * Handles `"method": "notifications/initialized"`, `"initialized"`, and `"ping"` (returning `{}` for ping without throwing code `-32601`).
2. **Modern Stateless Support (`2026-07-28`)**:
   * Handles `"method": "server/discover"` returning protocol versions `["2026-07-28"]` and server capabilities.
   * Processes per-request `_meta` metadata objects (`io.modelcontextprotocol/protocolVersion`).

---

## 5. Security Architecture and Parameter Injection Defense

* **Strict Parameterized Queries**: All query parameters passed via `tools/call` map to parameterized placeholders (`?1`, `:name`, `$name`) bound statically.
* **Schema Identifier Whitelisting**: Dynamic table and column identifiers validate against a strict regex pattern (`^[a-zA-Z_][a-zA-Z0-9_]*$`).

---

## 6. Core and Generic RDBMS Tool Specifications

The server advertises 10 database tools via `tools/list`:

1. `read_query`: Executes read-only SQL SELECT queries.
2. `write_query`: Executes SQL data modification queries (`INSERT`, `UPDATE`, `DELETE`).
3. `backup_database`: Creates online database backups.
4. `explain_query`: Inspects query execution plans.
5. `describe_table`: Describes table schemas, columns, data types, and key constraints.
6. `infer_relationships`: Infers primary and foreign key entity relationships.
7. `generate_erd`: Generates PlantUML / Mermaid ERD schema diagrams.
8. `list_tables`: Lists all user tables and views.
9. `export_data`: Exports table contents (CSV or JSON).
10. `check_database_health`: Runs diagnostic database integrity and key constraint checks.

---

## 7. Dual-Channel Payload Formatting (`tools/call`)

Tool call execution responses deliver dual-channel content to support both human-readable text rendering in LLM context windows and programmatic structured data rendering in host client UI components:

```json
{
  "jsonrpc": "2.0",
  "id": 105,
  "result": {
    "resultType": "complete",
    "content": [
      {
        "type": "text",
        "text": "| ArtistId | Name |\n|---|---|\n| 1 | AC/DC |\n| 2 | Accept |\n| 3 | Aerosmith |"
      }
    ],
    "structuredContent": [
      { "ArtistId": 1, "Name": "AC/DC" },
      { "ArtistId": 2, "Name": "Accept" },
      { "ArtistId": 3, "Name": "Aerosmith" }
    ]
  }
}
```

---

## 8. Known Errors and Regression Prevention Log

1. **Control Character Escaping**: File paths in generator scripts must explicitly double-escape backslashes (`src\\rdbms_tools.c`) to prevent expansion of `\r` (Carriage Return `0x0D`) or `\b` (Backspace) into binary control characters.
2. **JSON String Quote Escaping**: Internal double quotes in static JSON C strings must be escaped (`\"`). Generator scripts must use raw Python string literals with escaped backslashes to avoid stripping backslashes.
3. **Compiler Flag Scoping**: Optimization flags (`/O2`) must be conditionally scoped via CMake generator expressions (`$<$<CONFIG:Release>:/O2 ...>`) to avoid `/RTC1` Debug flag conflicts (`D8016`).
4. **Portable ISO C11 `safe_strdup()`**: Uses `malloc` + `memcpy` string duplication to eliminate POSIX feature macro requirements and MSVC `C4996` deprecation warnings.
5. **Windows Batch Script Branching**: `build.bat` uses label branching (`:TRY_CMAKE`, `:NO_TOOLS`) instead of multi-line parenthesized blocks to prevent CMD caret continuation syntax errors.
6. **Character Escaping (`U+0000`)**: String literals in source files must not contain raw binary NULL bytes (`U+0000`) or unescaped line breaks.
7. **JSON Payload Single-Line Escaping**: `send_tool_response()` runs strings through `json_escape_string()` to translate raw line breaks into `\n` escape sequences, preserving single-line JSON framing on `stdout`.

---

## 9. Automated Integration Test Harness Console Application

The project includes an automated test runner console application (`mcp_test_runner` / `mcp_test_runner.exe`) configured to test against `Chinook_Sqlite.sqlite`.

### 9.1 Chinook Test Suite & Dual-Channel Payload Verification

The test suite executes 12 automated test cases over STDIO IPC, asserting protocol compliance and dual-channel structure (`content` + `structuredContent`):

| Test ID | Method / Tool Name | Payload / Query | Assertion Criteria |
| :--- | :--- | :--- | :--- |
| **TC-01** | `initialize` | Legacy `2025-11-25` Handshake | Returns `protocolVersion: "2025-11-25"`. |
| **TC-02** | `ping` | System Ping | Returns `{}` result object. |
| **TC-03** | `server/discover` | Modern `2026-07-28` Discovery | Returns `protocolVersion: "2026-07-28"`. |
| **TC-04** | `tools/list` | Tool Discovery | Returns list of 10 advertised tools. |
| **TC-05** | `tools/call` (`read_query`) | `SELECT 1;` | Asserts `resultType: "complete"`, `content` array (`type: "text"`), and `structuredContent`. |
| **TC-06** | `tools/call` (`read_query`) | `SELECT * FROM Artist LIMIT 5;` | Validates Chinook `Artist` table response payload containing Markdown table in `content` and JSON array in `structuredContent`. |
| **TC-07** | `tools/call` (`read_query`) | `SELECT t.Name AS Track, a.Title AS Album FROM Track t JOIN Album a ON t.AlbumId = a.AlbumId LIMIT 5;` | Validates multi-table JOIN query payload structure across `Track` and `Album` tables. |
| **TC-08** | `tools/call` (`describe_table`) | `table_name: "Artist"` | Validates schema metadata inspection payload containing `content` and `structuredContent`. |
| **TC-09** | `tools/call` (`export_data`) | `table_name: "Genre"`, `format: "json"` | Validates data export payload for `Genre` table. |
| **TC-10** | `tools/call` | Missing `params` object | Asserts error payload returned. |
| **TC-11** | `tools/call` (`describe_table`) | Malicious identifier `users; DROP TABLE users;` | Asserts identifier validation intercepts invalid string. |
| **TC-12** | `unknown_method` | Unsupported RPC Method | Asserts error code `-32601` returned. |

---

## 10. CI/CD Integration (`.github/workflows/ci.yml`)

The GitHub Actions workflow downloads `Chinook_Sqlite.sqlite` from the official release repository (`v1.4.5`), sets `SQLITE_DB_PATH`, and executes `mcp_test_runner` on both Linux and Windows runners:

1. **Linux Runner (`ubuntu-latest`)**: Builds binaries via `build.sh` and CMake Release; executes `./mcp_test_runner ./sqlite-mcp-server` against `Chinook_Sqlite.sqlite`.
2. **Windows Runner (`windows-latest`)**: Builds binaries via `./build.bat` and CMake Release; executes `mcp_test_runner.exe sqlite-mcp-server.exe` against `Chinook_Sqlite.sqlite`.
