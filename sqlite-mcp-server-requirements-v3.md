# Requirements Specification: Standalone ANSI C SQLite Model Context Protocol (MCP) Server

## 1. System Purpose and Scope

This specification establishes the mandatory technical, architectural, security, and performance requirements for a native, zero-dependency Model Context Protocol (MCP) server written in ANSI C / C11. The application provides local desktop AI hosts—such as Claude Desktop, Claude Code, and Studio—with secure, high-speed access to SQLite relational databases exclusively over Standard Input/Output (`stdio`) Inter-Process Communication (IPC) transport.

Operating under the stateless protocol architecture introduced in the **MCP 2026-07-28 Specification** and powered by **JSON-RPC 2.0**, the server processes every incoming request independently without relying on hidden transport-layer sessions. The system is engineered to achieve sub-15 millisecond tool call latency, complete immunity to SQL parameter injection, a minimal binary footprint under 2.5 megabytes, and seamless native execution across 64-bit Linux and Windows operating systems without requiring third-party runtime dynamic libraries.

---

## 2. Platform, Target Architecture, and Binary Constraints

The server must operate directly as an isolated child process spawned by the host application. To ensure universal compatibility across developer environments, the build system and executable architecture must fulfill strict cross-compilation and dependency isolation standards.

```
[Host AI Application (Studio / Claude)]
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
* **Embedded Database Engine**: The server must statically compile the official SQLite single-file amalgamation (`sqlite3.c` and `sqlite3.h`) directly into the executable binary. The runtime must not attempt to load `libsqlite3.so` or `sqlite3.dll` dynamically at runtime via `dlopen()` or `LoadLibrary()`.
* **System Library Bounds**: Operating system linkages are restricted exclusively to standard platform libraries:
  * Linux: `libc.so`, `libm.so`, `libpthread.so` (or static libc equivalent).
  * Windows: `kernel32.dll`, `ucrtbase.dll` / `msvcrt.dll`.

### 2.3 Standard I/O (STDIO) Transport Isolation Guardrails
Because MCP `stdio` transport relies on newline-delimited (`\n`) JSON-RPC messages transferred across operating system pipes, standard output (`stdout`) contamination is a critical operational failure:
* **Standard Output (`stdout`) Restriction**: Standard output is strictly allocated to outbound JSON-RPC framing. Writing plain debug strings, non-JSON output, or raw standard library prints (`printf`, `puts`, `putchar`) to `stdout` is forbidden.
* **Standard Error (`stderr`) Redirection**: All application logging, diagnostic traces, and operational warnings must be directed exclusively to standard error via `fprintf(stderr, ...)`.
* **Windows Binary Stream Mode**: On Windows platforms, process initialization must immediately configure standard input and standard output file streams to binary mode using `_setmode(_fileno(stdout), _O_BINARY)` and `_setmode(_fileno(stdin), _O_BINARY)` to prevent newline translation (`\n` to `\r\n`) from corrupting JSON payloads.

---

## 3. Configuration and Environment Credentials

In compliance with local process security standards, credentials and database connection settings must never be passed as command-line execution arguments or JSON-RPC tool parameters. Command-line parameters are visible in plain text to unprivileged system users via process listing tools (`ps aux` on Linux, Task Manager / Process Explorer on Windows), and tool parameters expose credentials to language model prompt contexts and transcript logs.

### 3.1 Environment Variable Specification
The C server must read configuration settings exclusively from operating system environment variables during process initialization inside `main()` using standard library `getenv()`:

| Environment Variable | Target Data Type | Default Fallback | Purpose & Specification |
| :--- | :--- | :--- | :--- |
| `SQLITE_DB_PATH` | File Path String | `./database.db` | Absolute or relative file path to the target SQLite database file. |
| `SQLITE_OPEN_MODE` | String Enum (`READONLY` / `READWRITE`) | `READWRITE` | Controls SQLite database connection access mode (`SQLITE_OPEN_READONLY` vs `SQLITE_OPEN_READWRITE \| SQLITE_OPEN_CREATE`). |
| `SQLITE_BUSY_TIMEOUT_MS` | Integer (Milliseconds) | `5000` | Sets database lock acquisition wait timeout via `sqlite3_busy_timeout()`. |
| `SQLITE_ENCRYPTION_KEY` | Plaintext String | `NULL` | Optional passphrase for encrypted SQLite extensions (SQLCipher / SEE). |

### 3.2 Credential Memory Hygiene
If `SQLITE_ENCRYPTION_KEY` is supplied, the server must wipe the key string from stack and heap memory immediately after executing `sqlite3_key()` or opening the database handle. Memory clearing must be performed using `memset_explicit()` (as specified in ISO C23 / Annex K) or platform-specific non-elided memory wiping functions (`SecureZeroMemory` on Windows) to prevent compiler optimization passes from eliding the memory erasure.

---

## 4. Protocol and Specification Compliance

The server must implement a stateless JSON-RPC 2.0 processing engine that adheres fully to the MCP 2026-07-28 specification revision.

### 4.1 JSON-RPC 2.0 Message Rules
All incoming and outgoing payloads must conform to the JSON-RPC 2.0 standard:
* **Inbound Request Processing**: Every incoming message must be a JSON object containing `"jsonrpc": "2.0"`, a string `"method"`, optional `"params"`, and a valid correlation `"id"` (string or integer).
* **Outbound Response Framing**: Every response object must mirror the exact `"id"` of the corresponding request and contain `"jsonrpc": "2.0"`. A response must contain either `"result"` or `"error"`, but never both.
* **Standard JSON-RPC Error Handling**:
  * Code `-32700` (**Parse Error**): Emitted when invalid JSON text is received by the input buffer.
  * Code `-32600` (**Invalid Request**): Emitted when the JSON envelope violates JSON-RPC 2.0 schema structure.
  * Code `-32601` (**Method Not Found**): Emitted when an unsupported RPC method is invoked.
  * Code `-32602` (**Invalid Params**): Emitted when method parameter types or arguments fail validation.
  * Code `-32603` (**Internal Error**): Emitted upon internal database execution or allocation failures.

### 4.2 MCP 2026-07-28 Specification Rules
In accordance with the 2026-07-28 specification release, the server operates under a stateless core architecture:
* **Stateless Request Handling**: The server must process each incoming request independently. It must not require or expect legacy handshake initialization (`initialize`/`initialized`) or session headers (`Mcp-Session-Id`).
* **Request Metadata (`_meta`) Processing**: The server must inspect the `params._meta` object attached to each request:
  * Validate `io.modelcontextprotocol/protocolVersion` (accepting `"2026-07-28"`).
  * Inspect `io.modelcontextprotocol/clientCapabilities` to confirm caller feature support.
* **Protocol Error Codes**:
  * Code `-32021` (**MissingRequiredClientCapability**): Returned if a tool invocation requires a capability omitted from `_meta.io.modelcontextprotocol/clientCapabilities`.
  * Code `-32022` (**UnsupportedProtocolVersion**): Returned if the request specifies an unsupported protocol version, returning data indicating supported versions (`["2026-07-28"]`).
* **Mandatory Server Methods**:
  1. `server/discover`: Returns server implementation metadata (`name: "sqlite-mcp-server"`, `version: "1.0.0"`) and capability definitions (`tools: { listChanged: false }`).
  2. `tools/list`: Returns advertised tool definitions in a deterministic order with cache headers (`ttlMs: 300000`, `cacheScope: "public"`).
  3. `tools/call`: Executes database queries and returns content structures.

---

## 5. Security Architecture and Parameter Injection Defense

Because LLM tools process unstructured model-generated inputs, security validation must occur at the server interface boundary to prevent Arbitrary Code Execution (ACE) and SQL Parameter Injection.

### 5.1 Parameter Injection Defense Mechanics
* **Strict Parameterized Queries**: Direct SQL string formatting or concatenation (`sprintf(sql, "SELECT * FROM %s WHERE id = '%s'", ...)`), raw string interpolation, and unescaped input inclusion are strictly forbidden.
* **Extended Query Protocol Binding**: All query parameters passed via `tools/call` must be mapped to parameterized placeholders (`?1`, `:name`, `$name`) and bound exclusively through SQLite's static binding API:
  * `sqlite3_bind_text()` for string arguments.
  * `sqlite3_bind_int64()` for integer arguments.
  * `sqlite3_bind_double()` for floating-point arguments.
  * `sqlite3_bind_null()` for null values.
* **Schema Identifier Whitelisting**: If a tool accepts dynamic table or column identifiers, the server must validate the identifier string against a strict regular expression pattern (`^[a-zA-Z_][a-zA-Z0-9_]*$`) and verify the identifier against `sqlite_master` schema table metadata before executing statement preparation.

### 5.2 CERT C Secure Coding Rules
The C source code implementation must demonstrate strict compliance with the CERT C Coding Standard:
* **DCL30-C (Storage Durations)**: Never return or store pointers to automatic stack variables outside their defining function scope.
* **EXP34-C (Null Pointer Dereference)**: All pointer allocations and parameter references must be validated for `NULL` prior to dereferencing.
* **STR31-C (Buffer Overflow Safeguards)**: String operations must enforce explicit length bounds. Unbounded standard string copy functions (`strcpy`, `strcat`, `sprintf`) are forbidden; fixed-width alternatives (`snprintf`, `memcpy`) must be used.
* **ARR30-C (Array Bounds Isolation)**: All array subscripting and pointer offset arithmetic must verify upper and lower array boundary limits.
* **MEM34-C (Dynamic Memory Hygiene)**: Memory allocated via `malloc()` or `calloc()` must be checked for allocation failure and freed reliably along all execution paths.

---

## 6. Core Tool Definitions and Payload Formatting

The server must expose core query tools via `tools/list` and return dual-channel structured payloads via `tools/call`.

### 6.1 Core Advertised Query Tools

#### 1. `read_query`
* **Description**: Executes read-only SQL queries (`SELECT`, `EXPLAIN`) against the database.
* **Input Schema**:
  * `sql` (string, required): Parameterized SQL statement.
  * `params` (array \| object, optional): Parameter bindings.
* **Annotations**: `"readOnlyHint": true`.

#### 2. `write_query`
* **Description**: Executes data modification statements (`INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`) within a database transaction.
* **Input Schema**:
  * `sql` (string, required): Parameterized SQL modification statement.
  * `params` (array \| object, optional): Parameter bindings.
* **Annotations**: `"idempotentHint": false`.

### 6.2 Dual-Channel Payload Formatting (`tools/call`)
Tool responses must deliver dual-channel content to ensure human-readable rendering in LLM context windows as well as programmatic rendering in host client UI components:

```json
{
  "jsonrpc": "2.0",
  "id": 101,
  "result": {
    "resultType": "complete",
    "content": [
      {
        "type": "text",
        "text": "| id | username | email | status |\n|---|---|---|---|\n| 101 | alice | alice@example.com | active |\n| 102 | bob | bob@example.com | active |\n\n(2 rows returned)"
      }
    ],
    "structuredContent": [
      { "id": 101, "username": "alice", "email": "alice@example.com", "status": "active" },
      { "id": 102, "username": "bob", "email": "bob@example.com", "status": "active" }
    ]
  }
}
```

---

## 7. Generic RDBMS Database Management & Inspection Tool Catalog

To support future migration across relational database engines (e.g., PostgreSQL, MySQL, SQL Server, Oracle, and SQLite), all database management, inspection, and analysis tools exposed by the server must be strictly database-agnostic. Tool names, descriptions, input schemas, and payload representations must target abstract RDBMS concepts rather than engine-specific pragmas or system table naming conventions.

### 7.1 Portable RDBMS Tool Matrix

| Tool Name | Operation Category | Read-Only | Primary RDBMS Purpose |
| :--- | :--- | :---: | :--- |
| `list_tables` | Schema Metadata | Yes | Enumerates user tables, views, and materializations across database schemas. |
| `describe_table` | Schema Metadata | Yes | Inspects table structure, column types, default values, nullability, primary keys, and index metadata. |
| `explain_query` | Performance Analysis | Yes | Obtains query execution plan tree to identify scan operators, cost projections, and index hits. |
| `infer_relationships` | Schema Analysis | Yes | Analyzes foreign key constraints and naming conventions to map table entity relationships. |
| `generate_erd` | Visualization | Yes | Builds Entity-Relationship Diagrams as visual image payloads or structured diagram specifications. |
| `backup_database` | Maintenance & Safety | No | Creates a consistent point-in-time snapshot or hot backup of the active database. |
| `export_data` | Data Transport | Yes | Extracts data from tables or custom queries into portable formats (CSV, JSON) with pagination. |
| `check_database_health` | Operations & Integrity | Yes | Performs diagnostic checks on index integrity, storage allocation, and foreign key consistency. |

---

### 7.2 Detailed Portable Tool Specifications

#### 1. `list_tables` Tool
* **Description**: Lists all user-defined tables, views, and relational objects present within the active database connection.
* **Input Schema**:
  ```json
  {
    "type": "object",
    "properties": {
      "schema_name": { "type": "string", "description": "Optional schema scope filter (defaults to active schema)." },
      "include_views": { "type": "boolean", "description": "Flag to include database views alongside standard tables (default: true)." }
    },
    "additionalProperties": false
  }
  ```
* **Annotations**: `"readOnlyHint": true`.

#### 2. `describe_table` Tool
* **Description**: Retrieves comprehensive schema metadata for a specified table, including column definitions, data types, nullability, default values, primary keys, unique constraints, and associated indexes.
* **Input Schema**:
  ```json
  {
    "type": "object",
    "properties": {
      "table_name": { "type": "string", "description": "Target relational table name to inspect." },
      "schema_name": { "type": "string", "description": "Optional schema namespace filter." }
    },
    "required": ["table_name"],
    "additionalProperties": false
  }
  ```
* **Annotations**: `"readOnlyHint": true`.

#### 3. `explain_query` Tool
* **Description**: Generates the execution plan for a SQL query, surfacing plan nodes, join algorithms, table access paths, and estimated execution costs to guide performance optimization.
* **Input Schema**:
  ```json
  {
    "type": "object",
    "properties": {
      "sql": { "type": "string", "description": "SQL SELECT statement to evaluate." },
      "params": { "type": "array", "description": "Optional parameterized parameter bindings." }
    },
    "required": ["sql"],
    "additionalProperties": false
  }
  ```
* **Annotations**: `"readOnlyHint": true`.

#### 4. `infer_relationships` Tool
* **Description**: Analyzes explicit database foreign key constraints alongside implicit column naming patterns (e.g., `user_id` referencing `users.id`) to map relationship graph edges and cardinalities (one-to-one, one-to-many, many-to-many).
* **Input Schema**:
  ```json
  {
    "type": "object",
    "properties": {
      "table_names": {
        "type": "array",
        "items": { "type": "string" },
        "description": "Optional list of table names to restrict relationship discovery. Omit to evaluate entire database schema."
      },
      "include_implicit": { "type": "boolean", "description": "Infers relationships based on naming conventions when explicit foreign keys are missing (default: true)." }
    },
    "additionalProperties": false
  }
  ```
* **Annotations**: `"readOnlyHint": true`.

#### 5. `generate_erd` Tool
* **Description**: Renders a complete or partial Entity-Relationship Diagram (ERD). Can return a visual image payload (PNG/SVG) inline or structured text diagram markup (such as Mermaid ERD text syntax) for host UI client rendering.
* **Input Schema**:
  ```json
  {
    "type": "object",
    "properties": {
      "table_names": {
        "type": "array",
        "items": { "type": "string" },
        "description": "Target subset of tables to include in the diagram. Omit for full schema."
      },
      "format": {
        "type": "string",
        "enum": ["mermaid", "image/png", "image/svg+xml"],
        "description": "Desired output format (default: 'mermaid')."
      }
    },
    "additionalProperties": false
  }
  ```
* **Annotations**: `"readOnlyHint": true`.

#### 6. `backup_database` Tool
* **Description**: Creates a point-in-time online copy or snapshot backup of the database to a designated file target path or location.
* **Input Schema**:
  ```json
  {
    "type": "object",
    "properties": {
      "target_path": { "type": "string", "description": "Destination file system path for the backup file." },
      "overwrite": { "type": "boolean", "description": "Allows replacing an existing file at target_path (default: false)." }
    },
    "required": ["target_path"],
    "additionalProperties": false
  }
  ```
* **Annotations**: `"idempotentHint": false`.

#### 7. `export_data` Tool
* **Description**: Streams or exports database rows from a table or custom query into structured file payloads (CSV, JSON) with pagination controls to avoid context window overflow.
* **Input Schema**:
  ```json
  {
    "type": "object",
    "properties": {
      "source_table": { "type": "string", "description": "Table name to export." },
      "format": { "type": "string", "enum": ["csv", "json"], "description": "Export serialization format." },
      "limit": { "type": "integer", "description": "Maximum rows to export per batch (default: 1000)." },
      "offset": { "type": "integer", "description": "Row offset for paginated exports (default: 0)." }
    },
    "required": ["source_table", "format"],
    "additionalProperties": false
  }
  ```
* **Annotations**: `"readOnlyHint": true`.

#### 8. `check_database_health` Tool
* **Description**: Performs a comprehensive database diagnostic check, verifying index integrity, checking foreign key consistency, scanning for orphaned records, and measuring fragmentation.
* **Input Schema**:
  ```json
  {
    "type": "object",
    "properties": {
      "thorough": { "type": "boolean", "description": "Performs exhaustive page-level checks if set to true (default: false)." }
    },
    "additionalProperties": false
  }
  ```
* **Annotations**: `"readOnlyHint": true`.

---

## 8. Modular Code Architecture and Line Budget

To ensure long-term maintainability, rapid compilation, and straightforward code auditing, the C code repository must enforce strict file size boundaries.

### 8.1 Line-Budget Constraint
No individual application source file (`.c`) or header file (`.h`) in the server repository shall exceed **500 total lines of code** (excluding third-party amalgamation files `sqlite3.c` and `sqlite3.h`).

### 8.2 Modular Source File Layout
The implementation must be organized into five compact, single-responsibility modules:

```
src/
├── main.c              <-- Process entry point, env config, STDIO main loop (< 350 lines)
├── json_rpc.c          <-- JSON-RPC 2.0 lexer, parser, & response formatter (< 450 lines)
├── json_rpc.h          <-- Protocol structures and parser definitions (< 150 lines)
├── mcp_tools.c         <-- MCP 2026-07-28 tool handlers & schema descriptors (< 450 lines)
├── mcp_tools.h         <-- Tool callback definitions & routing interfaces (< 150 lines)
├── db_sqlite.c         <-- SQLite handle management & parameter binding engine (< 450 lines)
├── db_sqlite.h         <-- Database wrapper signatures & result structures (< 150 lines)
├── rdbms_tools.c       <-- Portable RDBMS management tool handlers (describe, ERD, backup) (< 450 lines)
└── rdbms_tools.h       <-- Generic RDBMS interface abstractions (< 150 lines)
```

---

## 9. Optimization, Compiling, and Performance Specifications

### 9.1 Performance Metric Targets
* **Execution Overhead**: Median server tool-call overhead must not exceed **15 milliseconds** per request.
* **Executable Binary Size**: Total static binary size must not exceed **2.5 Megabytes** on Linux and Windows.
* **Memory Footprint**: Peak Resident Set Size (RSS) memory consumption during query processing must remain below **16 Megabytes**.

### 9.2 SQLite Engine Performance Tuning Pragmas
Upon opening the SQLite database handle during process startup, the C server must execute the following database performance tuning pragmas:

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA temp_store = MEMORY;
PRAGMA cache_size = -64000; -- Allocate 64 MB page cache
PRAGMA foreign_keys = ON;
```

### 9.3 Compiler and Linker Build Configurations

#### Linux Compilation (GCC / Clang)
```bash
# Compile step with size and speed optimization flags
gcc -std=c11 -O2 -flto -fomit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
    -Iinclude -c src/*.c

# Link step with dead-code elimination and symbol stripping
gcc -O2 -flto -Wl,--gc-sections -Wl,-s -o sqlite-mcp-server *.o -lm -lpthread
```

#### Windows Compilation (MSVC `cl.exe` / `clang-cl`)
```cmd
:: Release compilation using MSVC toolchain
cl.exe /nologo /std:c11 /O2 /GL /Gy /Gw /MD ^
       /DNDEBUG /DSQLITE_THREADSAFE=0 /DSQLITE_OMIT_LOAD_EXTENSION ^
       /Iinclude src\*.c /link /LTCG /OPT:REF /OPT:ICF /OUT:sqlite-mcp-server.exe
```

---

## 10. Verification and Compliance Matrix

The server design must be validated against the formal compliance checklist prior to release:

| Requirement Area | Inspection Test | Verification Procedure | Pass Criteria |
| :--- | :--- | :--- | :--- |
| **Cross-Platform Build** | Binary Compilation | Compile target on Ubuntu 24.04 LTS (x86_64) and Windows 11 (x86_64). | Zero compiler warnings (`-Wall -Wextra`), successful static executable generation. |
| **Dependency Isolation** | Dependency Inspection | Run `ldd sqlite-mcp-server` (Linux) / `dumpbin /dependents` (Windows). | Zero third-party dynamic `.so` or `.dll` library dependencies. |
| **Line Budget Ceiling** | Code Line Count | Execute `wc -l src/*.[ch]` across all application files. | Every source and header file is strictly under 500 lines of code. |
| **SQL Injection Defense** | Vulnerability Test | Issue `read_query` tool calls containing malicious payloads (`' OR '1'='1`, `'; DROP TABLE users;--`). | Input is safely bound as literal query parameters; zero SQL injection path. |
| **STDIO Stream Isolation** | Stream Framing | Execute sequence of 1,000 tool calls while capturing raw `stdout`. | `stdout` contains valid line-delimited JSON-RPC frames exclusively; all logs appear on `stderr`. |
| **MCP Protocol Integrity** | MCP Inspector Protocol Test | Execute `server/discover`, `tools/list`, and `tools/call` requests over STDIO pipe. | 100% compliance with MCP 2026-07-28 and JSON-RPC 2.0 schemas. |
| **RDBMS Tool Portability** | Schema Neutrality Check | Verify tool parameter names across `describe_table`, `explain_query`, `generate_erd`, and `backup_database`. | Zero engine-specific keywords in tool schemas; 100% portable across PostgreSQL, MySQL, and SQLite. |


---

## 10. Known Errors and Regression Prevention Log

To prevent re-introducing architectural, build, or syntax bugs that were previously diagnosed and resolved during development, the codebase and build scripts must adhere to the following historical error prevention rules:

### 10.1 Control Character Escaping in Script-Generated Code
* **Historical Issue**: Path strings containing backslashes followed by specific letters (e.g., ) were processed by script generators where  was evaluated as an ASCII Carriage Return (). This corrupted  by truncating the path to , causing fatal MSVC error .
* **Mandatory Rule**: All script-generated file paths and string literals in generator scripts must explicitly double-escape backslashes ( or forward slashes ) to prevent string generator interpreters from expanding control characters (, , , ).

### 10.2 JSON String Literal Quote Escaping in C
* **Historical Issue**: Static C string literals containing raw unescaped double quotes inside embedded JSON strings (e.g., ) caused MSVC compilation errors (, , ) due to prematurely terminated C strings.
* **Mandatory Rule**: All embedded JSON strings in C code must use strict backslash escaping () for internal keys and string values, or use string concatenation arrays where every internal quote is properly escaped.

### 10.3 Compiler Flag Conflicts ( vs )
* **Historical Issue**: Hardcoding  (Release speed optimization) in  caused MSVC Debug builds to fail with error  ( run-time checks and  are incompatible).
* **Mandatory Rule**: Compiler optimization flags must be scoped conditionally using CMake generator expressions (e.g., ). Optimization flags must never be applied unconditionally to Debug configurations.

### 10.4 POSIX  Deprecation and Missing Prototypes
* **Historical Issue**: Using  under strict  on Linux GCC caused  errors (), and MSVC emitted deprecation warning .
* **Mandatory Rule**: The repository must use a lightweight, portable ISO C11 wrapper  ( + ), eliminating POSIX feature macro requirements and MSVC deprecation warnings.

### 10.5 Windows Batch Script Parentheses Parsing
* **Historical Issue**: Multi-line parenthesized blocks in Windows  () caused syntax parsing errors when nested with carets ().
* **Mandatory Rule**: Windows  must use clean label-based execution branching (, ) rather than deeply nested parenthesized blocks.

---

## 11. Dual-Era Protocol Handshake and Version Fallback Compatibility

To ensure seamless interoperability across both legacy MCP hosts (such as Visual Studio, older Claude Desktop releases, and legacy Agent SDKs) and modern stateless MCP hosts (Claude Code v2 runtime, modern Studio), the server must operate as a **Dual-Era MCP Server**.



### 11.1 Version Negotiation and Fallback Mechanics
1. **Legacy Handshake Support ()**:
   * ** Method**: Upon receiving , the server must respond with a valid  initialize result payload containing , , and .
   * **Notifications & Ping**: The server must accept , , and , returning an empty result  for ping without throwing error  (Method Not Found).
2. **Modern Stateless Support ()**:
   * ** RPC**: Returns server capabilities, identity, and supported protocol versions ().
   * **Per-Request  Metadata**: Processes per-request  declarations ().
3. **Preservation of Modern 2026-07-28 Capabilities**:
   * Dual-era fallback must NOT compromise modern features. When connected to a modern host, the server must preserve all 2026-07-28 capabilities, including dual-channel payload responses ( + ), , cache headers (, ), and stateless handling.

---

## 12. Automated Integration Test Harness Console Application

The codebase must include a self-contained console test runner () designed to execute automated integration tests against the compiled  binary over  IPC.



### 12.1 Test Suite Scope and Verification Matrix
The test application must spawn the server process, execute a sequence of JSON-RPC requests over , parse responses from , and assert strict compliance:

| Test Case ID | Target RPC / Tool | Payload Condition | Expected Response / Code | Pass Criteria |
| :--- | :--- | :--- | :--- | :--- |
| **TC-01** |  | Legacy 2025-11-25 Handshake | Result Object () | Successful handshake response; no  error. |
| **TC-02** |  | System Ping |  Empty Result Object | Returns success result . |
| **TC-03** |  | Modern 2026-07-28 Discovery |  () | Returns valid server capabilities and protocol version list. |
| **TC-04** |  | Tool Discovery | Tool Array + Cache Metadata | Returns 10 advertised tools, , and . |
| **TC-05** |  | Valid SQL () | Dual-channel ( + ) | , valid Markdown table and JSON array. |
| **TC-06** |  | Missing / Null SQL Parameter | JSON-RPC Error  or Tool Execution Error | Consistent error payload reporting parameter validation failure. |
| **TC-07** |  | Invalid Identifier () | Identifier Validation Error | Sanitization block intercepts invalid table name before statement prep. |
| **TC-08** |  | Unsupported RPC Method | JSON-RPC Error  (Method Not Found) | Code  returned cleanly without server crash. |

### 12.2 Integration Test Runner Specifications
* **Executable Target**: Built as part of the project ( on Linux,  on Windows).
* **Process Lifecycle**: Spawns  as a child process via OS pipe API ( / ), pipes JSON-RPC strings line-by-line, and captures output.
* **Exit Status**: Returns  if 100% of test cases pass; returns  and outputs failure diagnostics to  if any assertion fails.
