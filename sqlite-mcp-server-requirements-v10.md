# Requirements Specification: Standalone ANSI C SQLite Model Context Protocol (MCP) Server (v10)

## 1. System Purpose and Scope

This specification establishes the mandatory technical, architectural, security, and performance requirements for a native, zero-dependency Model Context Protocol (MCP) server written in ANSI C / C11. The application provides local desktop AI hosts—such as Claude Desktop, Claude Code, Amazon Q, and Studio—with secure, high-speed access to SQLite relational databases exclusively over Standard Input/Output (`stdio`) Inter-Process Communication (IPC) transport.

Operating under a dual-era architecture supporting both the legacy **MCP 2024-11-05 / 2025-11-25 Specifications** and the modern **MCP 2026-07-28 Specification**, the server processes requests reliably across diverse client implementations over **JSON-RPC 2.0**. The system achieves sub-15ms tool execution latency, complete parameter binding security against SQL parameter injection [251, 254], a minimal binary footprint under 2.5 megabytes, and full database query execution across all advertised tools without static stubs.

---

## 2. Real Database Engine Execution vs. Fallback

### 2.1 Native SQLite Engine Binding
All database tools exposed by the server must perform live database interaction against the connected SQLite database file (`SQLITE_DB_PATH`) using the official SQLite C API (`sqlite3_open_v2`, `sqlite3_prepare_v2`, `sqlite3_step`, `sqlite3_column_*`, `sqlite3_exec`, `sqlite3_backup_*`):

1. **`db_init()`**: Opens the target database connection with `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE` (or `SQLITE_OPEN_READONLY` based on `SQLITE_OPEN_MODE`), sets `sqlite3_busy_timeout()`, and initializes connection PRAGMAs:
   * `PRAGMA journal_mode = WAL;` [78, 88, 274, 287]
   * `PRAGMA foreign_keys = ON;` [78, 89, 274, 287]
   * `PRAGMA synchronous = NORMAL;` [274, 287]
2. **`read_query`**: Prepares SQL statements with `sqlite3_prepare_v2()`, steps through result rows with `sqlite3_step()`, extracts column names via `sqlite3_column_name()` and values via `sqlite3_column_text()`, and dynamically constructs human-readable Markdown tables and structured JSON arrays [94, 251, 254].
3. **`write_query`**: Executes DML/DDL statements via `sqlite3_exec()` or parameterized bindings, tracks `sqlite3_changes()`, and returns actual row modification metrics.
4. **`list_tables`**: Executes metadata queries against `sqlite_master` / `sqlite_schema` [16, 17, 46]:
   ```sql
   SELECT name AS table_name, type, (SELECT COUNT(*) FROM sqlite_master) AS total_objects
   FROM sqlite_master WHERE type IN ('table', 'view') AND name NOT LIKE 'sqlite_%'
   ORDER BY name;
   ```
5. **`describe_table`**: Validates table identifiers and executes `PRAGMA table_info('<table_name>')` [230, 271] to return actual column names, types, nullability, default values, and primary key indicators.
6. **`infer_relationships`**: Queries foreign key metadata across all user tables using `PRAGMA foreign_key_list('<table_name>')` [231, 271].
7. **`generate_erd`**: Inspects foreign key constraints across tables and dynamically constructs PlantUML ERD diagram definitions.
8. **`explain_query`**: Evaluates execution plan structures using `EXPLAIN QUERY PLAN <sql>` [231, 298].
9. **`backup_database`**: Uses the SQLite Online Backup API (`sqlite3_backup_init`, `sqlite3_backup_step`, `sqlite3_backup_finish`) [238, 239] to perform hot point-in-time snapshots.
10. **`check_database_health`**: Runs diagnostic checks using `PRAGMA integrity_check` [48, 281], `PRAGMA foreign_key_check`, and calculates database file size from `PRAGMA page_count` * `PRAGMA page_size` [281].

---

## 3. Core Tool Catalog & JSON Schema (`tools/list`)

Every advertised tool in `tools/list` must contain a human-readable `description` and a valid JSON Schema `inputSchema` object. Tools taking zero parameters must provide `{"type":"object","additionalProperties":false}`:

| Tool Name | Operation Category | Read-Only | Primary RDBMS Purpose |
| :--- | :--- | :---: | :--- |
| `read_query` | Query Execution | Yes | Executes read-only SQL queries (`SELECT`, `EXPLAIN`) with parameter binding. |
| `write_query` | Data Modification | No | Executes modification statements (`INSERT`, `UPDATE`, `DELETE`, `CREATE`). |
| `describe_table` | Schema Metadata | Yes | Inspects table column types, nullability, default values, and primary keys. |
| `list_tables` | Schema Metadata | Yes | Lists all database tables, views, and object counts. |
| `infer_relationships` | Schema Analysis | Yes | Discovers foreign key constraints across database tables. |
| `generate_erd` | Visualization | Yes | Generates PlantUML ERD diagrams from schema metadata. |
| `explain_query` | Performance | Yes | Evaluates SQL query execution plans (`EXPLAIN QUERY PLAN`). |
| `backup_database` | Maintenance | No | Performs online point-in-time database backups. |
| `export_data` | Transport | Yes | Exports table data in CSV or JSON format. |
| `check_database_health` | Operations | Yes | Performs integrity, foreign key, and database size checks. |

---

## 4. Dual-Channel Payload Serialization & Object Wrapping (`tools/call`)

Tool responses must deliver dual-channel payloads satisfying both legacy and modern MCP host specifications:

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "result": {
    "resultType": "complete",
    "content": [
      {
        "type": "text",
        "text": "| ArtistId | Name |\n|---|---|\n| 1 | AC/DC |\n| 2 | Accept |"
      }
    ],
    "structuredContent": {
      "rows": [
        { "ArtistId": "1", "Name": "AC/DC" },
        { "ArtistId": "2", "Name": "Accept" }
      ]
    }
  }
}
```

* **MCP 2026-07-28 Rule**: `structuredContent` must be a **JSON Object** (`record`). Root-level arrays (`[...]`) are automatically wrapped inside `{"rows": [...]}` to ensure schema validation passes across all MCP clients (Amazon Q, Claude Code v2, Studio).
* **MCP 2024-11-05 / 2025-11-25 Rule**: Legacy clients consume `content[].text` and ignore `structuredContent`.

---

## 5. Known Errors and Regression Prevention Log

To prevent re-introducing architectural or syntax bugs, the codebase adheres to the following rules:

1. **Control Character Escaping**: File paths in scripts must double-escape backslashes (`src\\rdbms_tools.c`) to prevent string generator expansion of `\r` (Carriage Return) or `\b` (Backspace).
2. **JSON String Quote Escaping**: Internal quotes inside C string literals must be escaped (`\"`).
3. **Compiler Flag Scoping**: Optimization flags like `/O2` must be scoped conditionally using CMake generator expressions (`$<$<CONFIG:Release>:/O2 ...>`).
4. **ISO C11 String Duplication**: Use `safe_strdup()` (`malloc` + `memcpy`) to eliminate POSIX `strdup` warnings (`-Wimplicit-function-declaration`) under `-std=c11`.
5. **Batch Script Branching**: Windows `build.bat` uses label branching (`:TRY_CMAKE`, `:NO_TOOLS`) instead of deeply nested parenthesized `if` blocks.
6. **No Raw Control Bytes in Source**: Source code generation must use explicit escape sequences (`'\\0'`, `'\\n'`, `'\\r'`) to prevent embedding binary NULL bytes (`U+0000`).
7. **AST Node Detachment & Memory Hygiene**: `json_rpc_parse_request` detaches `v_params` from the root JSON AST before calling `json_free(root)`, preventing use-after-free pointer errors.
8. **Single-Line Response Formatting**: All raw newlines in Markdown strings must be escaped (`\n` -> `\n`) inside JSON responses to preserve line-delimited `stdio` framing.
9. **Mandatory `inputSchema`**: All advertised tools in `tools/list` must include `inputSchema` definitions.
10. **`structuredContent` Object Wrapping**: `structuredContent` must be serialized as a JSON Object (`{"rows": [...]}`) rather than a top-level JSON Array.

---

## 6. Modular Code Architecture and Line Budget

No individual source file (`.c`) or header file (`.h`) shall exceed its line budget constraint:

* `include/json_rpc.h`: **73 lines** (budget < 150)
* `include/db_sqlite.h`: **53 lines** (budget < 150)
* `include/rdbms_tools.h`: **17 lines** (budget < 150)
* `src/main.c`: **118 lines** (budget < 350)
* `src/json_rpc.c`: **248 lines** (budget < 450)
* `src/rdbms_tools.c`: **175 lines** (budget < 450)
* `src/db_sqlite.c`: **442 lines** (budget < 450)
