# Requirements Specification: Standalone ANSI C SQLite Model Context Protocol (MCP) Server (v5)

## 1. System Purpose and Scope

This specification establishes the mandatory technical, architectural, security, and performance requirements for a native, zero-dependency Model Context Protocol (MCP) server written in ANSI C / C11. The application provides local desktop AI hosts—such as Visual Studio, Claude Desktop, Claude Code, and Studio—with secure, high-speed access to SQLite relational databases exclusively over Standard Input/Output (`stdio`) Inter-Process Communication (IPC) transport.

Operating under a **Dual-Era Protocol Engine** supporting both the **MCP 2025-11-25 Specification** (legacy handshakes) and **MCP 2026-07-28 Specification** (stateless discovery) powered by **JSON-RPC 2.0**, the server processes every incoming request cleanly while maintaining backward compatibility across diverse host runtimes. The system is engineered to achieve sub-15 millisecond tool call latency, complete immunity to SQL parameter injection, a minimal binary footprint under 2.5 megabytes, and seamless native execution across 64-bit Linux and Windows operating systems without requiring third-party runtime dynamic libraries.

---

## 2. Platform, Target Architecture, and Binary Constraints

The server must operate directly as an isolated child process spawned by the host application. To ensure universal compatibility across developer environments, the build system and executable architecture must fulfill strict cross-compilation and dependency isolation standards.

```
┌─────────────────────────────────────────────────────────┐
│ Host AI Application (Visual Studio / Claude / Studio)   │
└─────────────────────────────────────────────────────────┘
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
   [Local Database File (.db)]
```

### 2.1 Target Operating Systems and Binary Portability
The codebase must target native 64-bit execution environments without requiring managed runtimes, virtual machines, or external interpreter layers:
* **Linux Target**: 64-bit ELF executable (`x86_64-unknown-linux-gnu`), fully compatible with Linux kernel 3.2+ and glibc 2.17+ or musl libc.
* **Windows Target**: 64-bit PE32+ executable (`x86_64-pc-windows-msvc` or `x86_64-w64-mingw32`), fully compatible with Windows 10, Windows 11, and Windows Server 2019+.

### 2.2 Dependency Isolation & Engine Embedding
To guarantee zero-dependency deployment, the server must eliminate external shared object (`.so`) and dynamic-link library (`.dll`) dependencies:
* **Embedded Database Engine**: The server must statically compile the official SQLite single-file amalgamation (`sqlite3.c` and `sqlite3.h`) directly into the executable binary when enabled, or operate against abstract db interfaces.
* **System Library Bounds**: Operating system linkages are restricted exclusively to standard platform libraries:
  * Linux: `libc.so`, `libm.so`, `libpthread.so` (or static libc equivalent).
  * Windows: `kernel32.dll`, `ucrtbase.dll` / `msvcrt.dll`.

### 2.3 Standard I/O (STDIO) Transport Isolation Guardrails
Because MCP `stdio` transport relies on newline-delimited (`
`) JSON-RPC messages transferred across operating system pipes, standard output (`stdout`) contamination is a critical operational failure:
* **Standard Output (`stdout`) Restriction**: Standard output is strictly allocated to outbound JSON-RPC framing. Writing plain debug strings, non-JSON output, or raw standard library prints (`printf`, `puts`, `putchar`) to `stdout` is forbidden.
* **Standard Error (`stderr`) Redirection**: All application logging, diagnostic traces, and operational warnings must be directed exclusively to standard error via `fprintf(stderr, ...)`.
* **Windows Binary Stream Mode**: On Windows platforms, process initialization must immediately configure standard input and standard output file streams to binary mode using `_setmode(_fileno(stdout), _O_BINARY)` and `_setmode(_fileno(stdin), _O_BINARY)` to prevent newline translation (`
` to `
`) from corrupting JSON payloads.

---

## 3. Configuration and Environment Credentials

In compliance with local process security standards, credentials and database connection settings must never be passed as command-line execution arguments or JSON-RPC tool parameters.

### 3.1 Environment Variable Specification
The C server must read configuration settings exclusively from operating system environment variables during process initialization inside `main()` using standard library `getenv()`:

| Environment Variable | Target Data Type | Default Fallback | Purpose & Specification |
| :--- | :--- | :--- | :--- |
| `SQLITE_DB_PATH` | File Path String | `./database.db` | Absolute or relative file path to the target SQLite database file. |
| `SQLITE_OPEN_MODE` | String Enum (`READONLY` / `READWRITE`) | `READWRITE` | Controls SQLite database connection access mode (`SQLITE_OPEN_READONLY` vs `SQLITE_OPEN_READWRITE \| SQLITE_OPEN_CREATE`). |
| `SQLITE_BUSY_TIMEOUT_MS` | Integer (Milliseconds) | `5000` | Sets database lock acquisition wait timeout via `sqlite3_busy_timeout()`. |
| `SQLITE_ENCRYPTION_KEY` | Plaintext String | `NULL` | Optional passphrase for encrypted SQLite extensions (SQLCipher / SEE). |

### 3.2 Credential Memory Hygiene
If `SQLITE_ENCRYPTION_KEY` is supplied, the server must wipe the key string from stack and heap memory immediately after opening the database handle using `memset_explicit()` or `SecureZeroMemory` on Windows.

---

## 4. Protocol and Specification Compliance

The server implements a dual-era JSON-RPC 2.0 processing engine compliant with both **MCP 2025-11-25** and **MCP 2026-07-28**.

### 4.1 JSON-RPC 2.0 Message Rules
* **Inbound Request Processing**: Every incoming message must be a JSON object containing `"jsonrpc": "2.0"`, a string `"method"`, optional `"params"`, and a valid correlation `"id"` (string or integer).
* **Outbound Response Framing**: Every response object must mirror the exact `"id"` of the corresponding request and contain `"jsonrpc": "2.0"`. A response must contain either `"result"` or `"error"`, but never both.
* **Standard JSON-RPC Error Handling**:
  * Code `-32700` (**Parse Error**): Emitted when invalid JSON text is received by the input buffer.
  * Code `-32600` (**Invalid Request**): Emitted when the JSON envelope violates JSON-RPC 2.0 schema structure.
  * Code `-32601` (**Method Not Found**): Emitted when an unsupported RPC method is invoked.
  * Code `-32602` (**Invalid Params**): Emitted when method parameter types or arguments fail validation.
  * Code `-32603` (**Internal Error**): Emitted upon internal database execution or allocation failures.

---

## 5. Security Architecture and Parameter Injection Defense

* **Strict Parameterized Queries**: Direct SQL string formatting or concatenation (`sprintf(sql, "SELECT * FROM %s WHERE id = '%s'", ...)`), raw string interpolation, and unescaped input inclusion are strictly forbidden.
* **Extended Query Protocol Binding**: All query parameters passed via `tools/call` must be mapped to parameterized placeholders (`?1`, `:name`, `$name`) and bound exclusively through SQLite's static binding API (`sqlite3_bind_text()`, `sqlite3_bind_int64()`, etc.).
* **Schema Identifier Whitelisting**: Dynamic table or column identifiers must be validated against a strict regular expression pattern (`^[a-zA-Z_][a-zA-Z0-9_]*$`).

---

## 6. Core & Generic RDBMS Tool Catalog

The server exposes 10 database-agnostic tools:
1. `read_query`: Execute read-only SQL queries (`SELECT`, `EXPLAIN`).
2. `write_query`: Execute SQL modification queries (`INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`).
3. `backup_database`: Create an online point-in-time snapshot backup.
4. `explain_query`: Inspect query execution plan paths.
5. `describe_table`: Describe table structure, columns, types, nullability, and primary keys.
6. `infer_relationships`: Infer primary/foreign key relationships across tables.
7. `generate_erd`: Generate PlantUML/Mermaid ERD schema diagrams.
8. `list_tables`: List all database tables and views.
9. `export_data`: Export table rows in CSV or JSON format.
10. `check_database_health`: Run diagnostic integrity and key constraint checks.

---

## 7. Modular Code Architecture and Line Budget

No individual application source file (`.c`) or header file (`.h`) shall exceed **500 total lines of code**:
* `src/main.c`: Process entry point, env parser, stdio loop (< 350 lines).
* `src/json_rpc.c` & `include/json_rpc.h`: JSON-RPC lexer, parser, serializer (< 450 lines).
* `src/rdbms_tools.c` & `include/rdbms_tools.h`: Dual-era MCP handlers & tool descriptors (< 450 lines).
* `src/db_sqlite.c` & `include/db_sqlite.h`: SQLite engine wrapper & result formatters (< 450 lines).
* `src/test_runner.c`: Standalone integration test harness console app (< 450 lines).

---

## 8. Known Errors and Regression Prevention Log

To prevent re-introducing architectural, build, or syntax bugs that were previously diagnosed and resolved, the codebase must strictly adhere to the following rules:

### 8.1 Control Character Escaping in Script-Generated Code
* **Historical Issue**: Path strings containing backslashes followed by specific letters (e.g., `src
dbms_tools.c`) were evaluated as ASCII control characters (`
` as Carriage Return `0x0D`), corrupting file paths to `srcdbms_tools.c` and triggering MSVC error `C1083: Cannot open source file`.
* **Mandatory Rule**: All script-generated file paths must explicitly double-escape backslashes (`src\rdbms_tools.c`) or use raw byte arrays to prevent string generators from expanding control characters.

### 8.2 JSON String Literal Quote Escaping in C
* **Historical Issue**: Raw unescaped double quotes inside C string literals caused premature string termination, leading to MSVC syntax errors `C2146`, `C2065`, and `C2143`.
* **Mandatory Rule**: All internal quotes in JSON C strings must be escaped (`"`).

### 8.3 Compiler Flag Scoping (`/O2` vs `/RTC1`)
* **Historical Issue**: Hardcoding `/O2` in CMake caused MSVC Debug builds to fail with error `D8016: '/RTC1' and '/O2' command-line options are incompatible`.
* **Mandatory Rule**: Compiler optimization flags must be conditionally scoped using CMake generator expressions (`$<$<CONFIG:Release>:/O2 ...>`).

### 8.4 Portable ISO C11 String Duplication
* **Historical Issue**: Using POSIX `strdup()` under `-std=c11` caused Linux GCC implicit declaration errors (`-Wimplicit-function-declaration`) and MSVC `C4996` deprecation warnings.
* **Mandatory Rule**: The repository uses a portable ISO C11 helper `safe_strdup()` (`malloc` + `memcpy`), eliminating POSIX macro dependencies.

### 8.5 Windows Batch Script Branching
* **Historical Issue**: Multi-line parenthesized blocks in Windows `build.bat` triggered syntax parsing errors when combined with caret line continuations (`^`).
* **Mandatory Rule**: Windows `build.bat` uses clean label-based branching (`:TRY_CMAKE`, `:NO_TOOLS`) instead of nested parenthesized `if` blocks.

---

## 9. Dual-Era Protocol Handshake and Version Fallback Compatibility

To ensure seamless compatibility across both legacy hosts (Visual Studio, older Claude Desktop releases) and modern stateless hosts (Claude Code, modern Studio), the server operates as a **Dual-Era MCP Server**:

```
                              Incoming JSON-RPC Request
                                         │
                   ┌─────────────────────┴─────────────────────┐
                   ▼                                           ▼
         Method == "initialize"                      Method == "server/discover"
      (Legacy 2025-11-25 Handshake)               (Modern 2026-07-28 Stateless)
                   │                                           │
   Respond: "protocolVersion": "2025-11-25"    Respond: "protocolVersion": "2026-07-28"
   Capabilities: {"tools": {...}}              Capabilities: {"tools": {...}}
   Handle "ping" -> {}                         Handle per-request _meta
```

1. **Legacy 2025-11-25 Handshake Support**:
   * Method `"initialize"`: Responds with protocol version `"2025-11-25"`, `serverInfo`, and `capabilities`.
   * Method `"ping"`: Responds with empty result `{}` (preventing `-32601 Method Not Found` errors).
   * Method `"notifications/initialized"` / `"initialized"`: Acknowledged without throwing errors.
2. **Modern 2026-07-28 Stateless Support**:
   * Method `"server/discover"`: Returns capabilities and protocol version `"2026-07-28"`.
   * Per-request `_meta` metadata processing supported.
3. **Capability Preservation**:
   * Dual-era fallback preserves all modern features (dual-channel `content` + `structuredContent`, `outputSchema`, cache headers `ttlMs: 300000`, `cacheScope: "public"`).

---

## 10. Automated Integration Test Harness Console Application

The repository includes a self-contained console application (`mcp_test_runner` / `mcp_test_runner.exe`) built from `src/test_runner.c`.

### 10.1 Test Suite Scope
The test runner spawns `sqlite-mcp-server` as a child process via OS STDIO pipes (`CreatePipe`/`CreateProcess` on Windows, `pipe`/`fork`/`execv` on Linux), issues line-delimited JSON-RPC requests, and verifies responses:

| Test ID | Target RPC / Tool | Input Payload | Expected Assertion | Pass Criteria |
| :--- | :--- | :--- | :--- | :--- |
| **TC-01** | `initialize` | `{"protocolVersion":"2025-11-25"}` | Returns `protocolVersion` & `serverInfo` | Handshake succeeds without error |
| **TC-02** | `ping` | `{}` | Returns `"result":{}` | System ping succeeds |
| **TC-03** | `server/discover` | `{}` | Returns `"2026-07-28"` & `capabilities` | Discovery succeeds |
| **TC-04** | `tools/list` | `{}` | Returns `tools` array & cache metadata | All 10 tools returned |
| **TC-05** | `tools/call` (`read_query`) | `{"sql":"SELECT 1;"}` | Returns `resultType` & dual-channel data | Valid query execution |
| **TC-06** | `tools/call` (missing args) | `{}` | Returns JSON-RPC `error` | Missing params intercepted |
| **TC-07** | `tools/call` (`describe_table`) | `{"table_name":"users; DROP..."}` | Returns error payload | Malicious identifier sanitized |
| **TC-08** | `unknown_method` | `{}` | Returns code `-32601` | Method not found error returned |

### 10.2 CI/CD Integration (.github/workflows/ci.yml)
* **Linux Workflow**: Builds server and test runner via `build.sh` and CMake, then executes `./mcp_test_runner ./sqlite-mcp-server`.
* **Windows Workflow**: Builds server and test runner via `build.bat` and CMake (Release configuration), then executes `Release\mcp_test_runner.exe Release\sqlite-mcp-server.exe`.
* **Exit Code Enforcement**: CI jobs fail if any test case returns a non-zero exit code.
