# Requirements Specification: Standalone ANSI C SQLite Model Context Protocol (MCP) Server (v6)

## 1. System Purpose and Scope

This specification establishes the mandatory technical, architectural, security, and performance requirements for a native, zero-dependency Model Context Protocol (MCP) server written in ANSI C / C11. The application provides local desktop AI hosts—such as Claude Desktop, Claude Code, Visual Studio, and Studio—with secure, high-speed access to SQLite relational databases exclusively over Standard Input/Output (`stdio`) Inter-Process Communication (IPC) transport.

Operating under a **Dual-Era MCP Server Architecture** supporting both legacy handshakes (**2025-11-25**) and modern stateless discovery (**2026-07-28**), the server processes every incoming JSON-RPC 2.0 request with sub-15 millisecond latency, complete immunity to SQL parameter injection, a minimal binary footprint under 2.5 megabytes, and seamless native execution across 64-bit Linux and Windows operating systems without third-party runtime dynamic libraries.

---

## 2. Platform, Target Architecture, and Binary Constraints

The server operates directly as an isolated child process spawned by the host application over `stdio` pipes.

```
[Host AI Application (Studio / Visual Studio / Claude)]
        │
        ▼ (STDIO Pipe - Plaintext Line-Delimited JSON-RPC 2.0)
┌─────────────────────────────────────────────────────────┐
│ Self-Contained 64-Bit Executable (sqlite-mcp-server)   │
│                                                         │
│  ┌────────────────────┐     ┌────────────────────────┐  │
│  │ JSON-RPC Engine    │ ──► │ RDBMS Tool Handlers    │  │
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
   [Local Database File (.db)]
```

### 2.1 Target Operating Systems and Binary Portability
* **Linux Target**: 64-bit ELF executable (`x86_64-unknown-linux-gnu`), fully compatible with Linux kernel 3.2+ and glibc 2.17+ or musl libc.
* **Windows Target**: 64-bit PE32+ executable (`x86_64-pc-windows-msvc`), fully compatible with Windows 10, Windows 11, and Windows Server 2019+.

---

## 3. Configuration and Environment Credentials

| Environment Variable | Target Data Type | Default Fallback | Purpose & Specification |
| :--- | :--- | :--- | :--- |
| `SQLITE_DB_PATH` | File Path String | `./database.db` | Path to target SQLite database file. |
| `SQLITE_OPEN_MODE` | String Enum | `READWRITE` | Database access mode (`READONLY` vs `READWRITE`). |
| `SQLITE_BUSY_TIMEOUT_MS` | Integer | `5000` | Busy timeout in milliseconds via `sqlite3_busy_timeout()`. |
| `SQLITE_ENCRYPTION_KEY` | Plaintext String | `NULL` | Passphrase for encrypted SQLite extensions (wiped post-init via `SecureZeroMemory` / `memset_explicit`). |

---

## 4. Protocol and Specification Compliance

### 4.1 JSON-RPC 2.0 Message Rules
* **Inbound Request Processing**: Every incoming message must contain `"jsonrpc": "2.0"`, a string `"method"`, optional `"params"`, and a correlation `"id"`.
* **Outbound Response Framing**: Every response object mirrors the request `"id"`, contains `"jsonrpc": "2.0"`, and emits either `"result"` or `"error"`.
* **Standard JSON-RPC Error Handling**:
  * Code `-32700` (**Parse Error**): Invalid JSON text received.
  * Code `-32600` (**Invalid Request**): JSON envelope structure failure.
  * Code `-32601` (**Method Not Found**): Unsupported RPC method.
  * Code `-32602` (**Invalid Params**): Parameter type/argument validation failure.
  * Code `-32603` (**Internal Error**): Database execution or allocation failure.

---

## 5. Security Architecture and Parameter Injection Defense

* **Strict Parameterized Queries**: Dynamic query string formatting (`sprintf`) is forbidden. Parameters are bound exclusively via static SQLite APIs (`sqlite3_bind_text`, `sqlite3_bind_int64`, `sqlite3_bind_double`, `sqlite3_bind_null`).
* **Schema Identifier Whitelisting**: Table and column identifiers must match `^[a-zA-Z_][a-zA-Z0-9_]*$` and be verified against `sqlite_master`.
* **CERT C Compliance**: Enforces strict bounds checking, memory clearing (`memset_explicit`), and allocation checks (`EXP34-C`, `STR31-C`, `ARR30-C`, `MEM34-C`).

---

## 6. Core Tool Definitions and Payload Formatting

### 6.1 Advertised Database Tools (`tools/list`)
Exposes 10 generic, database-agnostic tools: `read_query`, `write_query`, `backup_database`, `explain_query`, `describe_table`, `infer_relationships`, `generate_erd`, `list_tables`, `export_data`, and `check_database_health`.

### 6.2 Dual-Channel Payload Formatting
Responses deliver human-readable Markdown text alongside structured JSON arrays (`content` + `structuredContent`).

---

## 7. Modular Code Architecture and Line Budget

No individual `.c` or `.h` file shall exceed **500 lines of code** (excluding `sqlite3.c`/`sqlite3.h`).
```
src/
├── main.c              <-- Process entry point, env config, STDIO loop (< 350 lines)
├── json_rpc.c          <-- JSON-RPC 2.0 lexer, parser, & serializer (< 450 lines)
├── json_rpc.h          <-- Protocol structures & signatures (< 150 lines)
├── rdbms_tools.c       <-- Tool handlers & JSON escaping engine (< 450 lines)
├── rdbms_tools.h       <-- Generic RDBMS interface abstractions (< 150 lines)
├── db_sqlite.c         <-- SQLite handle management & binding engine (< 450 lines)
├── db_sqlite.h         <-- Database signatures & structures (< 150 lines)
└── test_runner.c       <-- Integration test harness console app (< 450 lines)
```

---

## 8. Known Errors and Regression Prevention Log

### 8.1 Control Character Escaping in Generator Scripts
Path strings with backslashes must double-escape backslashes (`src\rdbms_tools.c`) to prevent string generator interpreters from expanding control characters (``, ``, `
`).

### 8.2 JSON String Literal Quote Escaping in C
All embedded JSON string literals in C code must use strict backslash escaping (`"`) or raw string literals (`r'''...'''`) in generators to prevent `C2146`, `C2065`, `C2143` errors.

### 8.3 Compiler Flag Scoping (`/O2` vs `/RTC1`)
CMake optimization flags must be conditionally scoped using generator expressions (`$<$<CONFIG:Release>:/O2 ...>`) to prevent MSVC Debug error `D8016`.

### 8.4 Portable ISO C11 String Duplication
Uses `safe_strdup()` (`malloc` + `memcpy`), avoiding POSIX `strdup()` implicit declaration warnings under `-std=c11` and MSVC `C4996` deprecation warnings.

### 8.5 Windows Batch Script Branching
Windows `build.bat` must use label-based branching (`:TRY_CMAKE`, `:NO_TOOLS`) to eliminate parenthesized `if` syntax errors.

### 8.6 Source Code Control Character Hygiene (`U+0000`)
Source file generators must never write literal binary NULL bytes (`U+0000` / `0x00`) or raw string line breaks between single quotes into C source files.

### 8.7 JSON String Newline Escaping & AST Memory Ownership (NEW)
* **STDIO Single-Line JSON Escaping**: Multiline text strings (such as Markdown tables containing raw `
` characters) embedded into JSON response fields (`"text":"..."`) **must be JSON-escaped** (`
` -> `\n`, `"` -> `"`) before emitting across `stdout`. Emitting unescaped raw newlines splits a single JSON-RPC message into multiple `stdout` lines, breaking MCP single-line framing and causing client test runners to read leftover lines out of sequence.
* **AST Memory Detachment**: When parsing JSON-RPC requests in `json_rpc_parse_request`, child AST nodes assigned to `req->params` **must be detached from the root object** (setting `values[i] = NULL` before calling `json_free(root)`). Deallocating `root` without detaching `v_params` creates a dangling pointer, leading to silent memory corruption, missing tool parameter errors, and OS-dependent test failures (`glibc` vs MSVC CRT).

---

## 9. Dual-Era Protocol Handshake and Version Fallback Compatibility

* **Legacy Handshake (2025-11-25)**: Responds to `"method": "initialize"` with protocol version `2025-11-25`. Emits empty result `{}` for `ping` and handles `notifications/initialized` without error `-32601`.
* **Modern Stateless (2026-07-28)**: Preserves `server/discover` RPC, per-request `_meta`, dual-channel payloads, and cache headers (`ttlMs: 300000`, `cacheScope: "public"`).

---

## 10. Automated Integration Test Harness Console Application

The codebase includes `mcp_test_runner` (`mcp_test_runner.exe` on Windows, `./mcp_test_runner` on Linux), which executes an automated 8-test suite (`TC-01` through `TC-08`) over `stdio` pipes against `sqlite-mcp-server`.

```
┌───────────────────────────┐  STDIO Pipe (JSON-RPC)  ┌───────────────────────────┐
│     mcp_test_runner       │ ──────────────────────► │     sqlite-mcp-server     │
│ (Automated Test Suite)    │ ◄────────────────────── │  (Dual-Era C Executable)  │
└───────────────────────────┘                         └───────────────────────────┘
```
