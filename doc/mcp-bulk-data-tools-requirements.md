# Technical Requirements Specification: Standardized Data Transfer MCP Tools & Cross-RDBMS Verification Suite

## 1. Document Overview & Architectural Intent

This technical specification outlines the engineering requirements for implementing and standardizing bulk data export and import tools across Model Context Protocol (MCP) servers for relational and cloud database management systems. In modern AI-assisted engineering and data platform architectures, large language models (LLMs) frequently need to move, extract, or ingest data datasets across heterogeneous database engines. However, requiring an LLM agent to directly construct engine-specific command-line invocations, manage low-level SQL syntax, tune WAL pragmas, or orchestrate cloud staging pipelines introduces severe operational fragility, prompt context bloat, and execution failure.

To eliminate this complexity, each engine-specific MCP server—covering SQLite, PostgreSQL, and Snowflake—must expose two standardized, high-level tool definitions: `export_data` and `import_data`. From the host LLM client's perspective, these tools present an identical JSON Schema contract regardless of the underlying database engine. Internally, each MCP server acts as a domain-specific adapter, encapsulating native CLI binaries, high-performance streaming protocols, temporary memory allocations, and file staging pipelines.

### Architectural Component Interaction

The end-to-end data flow operates through a clear separation of concerns between the host LLM, the MCP protocol layer, the database-specific MCP servers, and local storage:

1. **LLM Host Layer**: Invokes either `export_data` or `import_data` using standardized JSON parameters, specifying logical targets and local file paths without needing to know database-specific commands.
2. **MCP Protocol Layer**: Transmits JSON-RPC 2.0 requests over standard `stdio` or `streamable-http` transports with input validation against JSON Schemas.
3. **MCP Server Layer (SQLite, PostgreSQL, Snowflake)**: Receives the standardized tool call, translates parameters into optimal engine execution paths, manages local file compression and staging, and executes native operations.
4. **Target Engine & Storage Layer**: Executes the underlying physical data movement directly between the database engine and local disk.

To maximize throughput while avoiding unnecessary cloud platform dependencies, local CSV files (compressed with GZIP or uncompressed) serve as the primary interchange format across all three MCP servers.

---

## 2. Standardized MCP Tool Specifications

To maintain total engine independence for calling applications, all database MCP servers must implement identical tool signatures for `export_data` and `import_data`. Any database-specific behavioral controls must be handled via optional parameters that default to optimal, non-destructive values.

### 2.1 Tool 1: `export_data`

The `export_data` tool extracts data from a specified database table, view, or SQL SELECT statement and writes it to a designated local file path. Local CSV format is the enforced default across all MCP servers.

#### Contextual Framing & Input Parameter Schema

The following JSON Schema defines the required parameters for `export_data`. All MCP servers must validate client requests against this contract.

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "ExportDataInput",
  "type": "object",
  "properties": {
    "target": {
      "type": "string",
      "description": "Name of the database table, view, or raw SQL SELECT query to export."
    },
    "destination_path": {
      "type": "string",
      "description": "Absolute or relative local file path (or stage location) where the exported file will be written."
    },
    "format": {
      "type": "string",
      "enum": ["csv", "tsv", "json", "sql_dump", "parquet"],
      "default": "csv",
      "description": "Data serialization format. Defaults to 'csv'."
    },
    "compression": {
      "type": "string",
      "enum": ["none", "gzip"],
      "default": "none",
      "description": "Compression algorithm to apply. Defaults to 'none'."
    },
    "header": {
      "type": "boolean",
      "default": true,
      "description": "Whether to include column headers in the output file. Defaults to true."
    },
    "delimiter": {
      "type": "string",
      "default": ",",
      "description": "Field delimiter character for text formats. Defaults to comma ','."
    },
    "max_file_size_mb": {
      "type": "integer",
      "default": 250,
      "description": "Optional maximum file size in MB before splitting output into multiple files (used for large cloud unloads)."
    }
  },
  "required": ["target", "destination_path"]
}
```

### 2.2 Tool 2: `import_data`

The `import_data` tool ingests data from a specified local file into a target database table. Pre-existing database import tools in existing MCP implementations must be adapted to conform to this unified signature.

#### Contextual Framing & Input Parameter Schema

The following JSON Schema defines the required parameters for `import_data`, supporting optional schema evolution and speed optimization flags.

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "ImportDataInput",
  "type": "object",
  "properties": {
    "source_path": {
      "type": "string",
      "description": "Absolute or relative local file path (or stage location) of the file to ingest."
    },
    "destination_table": {
      "type": "string",
      "description": "Target database table name into which data will be imported."
    },
    "format": {
      "type": "string",
      "enum": ["csv", "tsv", "json", "sql_dump", "parquet"],
      "default": "csv",
      "description": "Data serialization format of the source file. Defaults to 'csv'."
    },
    "compression": {
      "type": "string",
      "enum": ["none", "gzip"],
      "default": "none",
      "description": "Compression algorithm of the source file. Defaults to 'none'."
    },
    "header": {
      "type": "boolean",
      "default": true,
      "description": "Whether the source file includes a header line. Defaults to true."
    },
    "delimiter": {
      "type": "string",
      "default": ",",
      "description": "Field delimiter character for text files. Defaults to comma ','."
    },
    "on_error": {
      "type": "string",
      "enum": ["abort", "continue", "skip_file"],
      "default": "abort",
      "description": "Action to take when row parsing errors occur. Defaults to 'abort'."
    },
    "auto_create_table": {
      "type": "boolean",
      "default": false,
      "description": "If true, automatically creates the target table based on column header inspection or schema inference."
    },
    "optimize_speed": {
      "type": "boolean",
      "default": true,
      "description": "If true, applies high-performance engine pragmas, transaction batching, or temporary index deferrals."
    }
  },
  "required": ["source_path", "destination_table"]
}
```

### 2.3 Standardized Response Contract

To ensure host agents receive consistent execution telemetry across all database types, both tools must return a standardized JSON payload inside the MCP `TextContent` block.

```json
{
  "status": "success",
  "engine": "sqlite",
  "operation": "import",
  "target": "benchmark_orders",
  "rows_processed": 500000,
  "bytes_processed": 18450000,
  "duration_seconds": 1.42,
  "files_created_or_read": ["/tmp/mcp_exports/orders_batch.csv"],
  "warnings": []
}
```

---

## 3. Engine-Specific Execution Requirements

Although the public-facing MCP tool signatures are identical, each target RDBMS requires specialized internal implementation details to maximize transfer performance and enforce transactional integrity.

### 3.1 SQLite MCP Server Execution Requirements

SQLite is an embedded, single-file relational engine. As documented in official SQLite CLI specifications, raw per-row `INSERT` operations incur high disk I/O bottlenecks due to default autocommit behavior. The SQLite MCP server must implement internal optimizations to bypass these limitations.

#### Internal Export Execution
1. **Delimited Text Export**: For CSV or TSV exports, the server configures output mode via `.mode csv` (or `.mode tabs`), turns headers on via `.headers on`, redirects standard output to the specified `destination_path` via `.output`, and executes the query.
2. **Full Database SQL Dump**: When `format: "sql_dump"` is requested, the server invokes the native `.dump` CLI command to generate UTF-8 executable SQL script backups.

#### Internal Import Execution & Performance Optimizations
1. **Explicit Transaction Boundaries**: To avoid individual commit write locks on every row, the server must wrap all bulk ingestion commands inside an explicit transaction block (`BEGIN TRANSACTION; ... COMMIT;`).
2. **Speed-Optimization Pragmas**: When `optimize_speed: true` is enabled, the server temporarily executes performance pragmas before ingestion:
   ```sql
   PRAGMA journal_mode = WAL;
   PRAGMA synchronous = OFF;
   ```
   Upon completing the load, the server must restore `PRAGMA synchronous = NORMAL;`.
3. **High-Speed Ingestion**: Ingests files using the native `.import --csv --skip 1` CLI directive or optimized C/Python parameterized batch execution.

### 3.2 PostgreSQL MCP Server Execution Requirements

PostgreSQL supports high-performance SQL-level data streaming via the `COPY` command as well as parallel logical backups using `pg_dump` and `pg_restore`.

#### Internal Export Execution
1. **SQL Streaming (`COPY TO`)**: For CSV exports, the server executes PostgreSQL's native SQL `COPY` command:
   ```sql
   COPY (SELECT * FROM target_table) TO '/path/to/destination.csv' WITH (FORMAT csv, HEADER true, DELIMITER ',');
   ```
2. **Logical Backups (`pg_dump`)**: When full schema/data dumps or archive formats are requested, the server executes `pg_dump` with custom (`-Fc`) or directory (`-Fd`) formats, utilizing parallel worker jobs (`-j <cores>`) where available.

#### Internal Import Execution & Performance Tuning
1. **SQL Direct Ingestion (`COPY FROM`)**: Data is ingested directly into target tables via SQL `COPY`:
   ```sql
   COPY destination_table FROM '/path/to/source.csv' WITH (FORMAT csv, HEADER true, DELIMITER ',');
   ```
2. **Session Memory Adjustments**: As recommended in PostgreSQL database population documentation, the server temporarily increases session maintenance memory during large ingestion jobs:
   ```sql
   SET maintenance_work_mem = '1GB';
   SET max_wal_size = '16GB';
   ```
3. **Index and Constraint Management**: When populating new tables or performing full replacements, the server drops non-essential indexes and foreign key constraints prior to loading, recreating them in bulk afterwards.
4. **Planner Statistics Update**: Automatically issues `ANALYZE destination_table;` upon completion to ensure query optimizer statistics remain accurate.

### 3.3 Snowflake MCP Server Execution Requirements

Snowflake utilizes a decoupled architecture where virtual warehouses (compute) process data stored in internal or external cloud stages. Executing local CSV file exports and imports requires a transparent two-step pipeline managed internally by the Snowflake MCP server.

#### Stepwise Cloud Staging & Local Transfer Pipeline

The following table details how local files move through Snowflake internal stage locations during export and import operations:

| Phase | Pipeline Step | SQL & Driver Execution Details | Storage Location |
| :--- | :--- | :--- | :--- |
| **Export Step 1** | Stage Unload | `COPY INTO @~/mcp_stage/exp_ FROM table FILE_FORMAT = (TYPE = CSV COMPRESSION = GZIP HEADER = TRUE) SINGLE = FALSE MAX_FILE_SIZE = 262144000;` | Snowflake Internal User Stage (`@~`) |
| **Export Step 2** | Local Download | Driver `GET @~/mcp_stage/exp_* file:///tmp/mcp_exports/` followed by `REMOVE @~/mcp_stage/;` | Local Client Disk |
| **Import Step 1** | Local Upload | Driver `PUT file:///tmp/mcp_imports/local.csv.gz @~/mcp_stage/ AUTO_COMPRESS = TRUE;` | Snowflake Internal User Stage (`@~`) |
| **Import Step 2** | Table Bulk Load | `COPY INTO destination_table FROM @~/mcp_stage/ FILE_FORMAT = (TYPE = CSV FIELD_DELIMITER = ',' SKIP_HEADER = 1) ON_ERROR = CONTINUE;` | Snowflake Target Table |

#### Internal Performance & Sizing Requirements
1. **File Chunk Optimization**: In accordance with Snowflake bulk loading guidelines, large local CSV files must be split into 100 MB–250 MB compressed GZIP chunks prior to executing `PUT` commands. This maximizes parallel thread ingestion across virtual warehouse nodes.
2. **Schema Inference**: When `auto_create_table: true` is set, the server executes `INFER_SCHEMA` on the staged CSV files and builds the destination table using `CREATE TABLE ... USING TEMPLATE` before invoking `COPY INTO`.

---

## 4. Cross-RDBMS Test Application Requirements

To verify that the standardized `export_data` and `import_data` tools operate reliably and maintain strict data fidelity across all three database systems, a dedicated test application must be built.

### 4.1 Test Harness Architecture

The test harness must function as an autonomous MCP client connecting simultaneously to the SQLite, PostgreSQL, and Snowflake MCP servers via standard transport protocols (`stdio` or HTTP `streamable-http`).

### 4.2 Synthetic Test Dataset Specification

The test harness must create a benchmark dataset (`benchmark_orders`) populated with edge-case data types designed to test serialization, escaping, timestamp handling, and character encodings:

- **`order_id`** (Integer): 64-bit unique identifiers.
- **`customer_name`** (Text): UTF-8 encoded text containing international characters, accents, and embedded commas (e.g., `"Müller, René & Sons"`).
- **`order_date`** (Timestamp): ISO-8601 formatted datetime strings (`YYYY-MM-DD HH:MM:SS.sss`).
- **`total_amount`** (Decimal): Fixed-precision numeric values (e.g., `1499.99`).
- **`is_fulfilled`** (Boolean): Boolean truth values (`true`/`false`).
- **`notes`** (Text): Complex multiline text strings containing embedded line breaks (`\r\n`), quotes (`"`), and commas.
- **`metadata_json`** (JSON / Text): Escaped JSON strings representing nested attributes.

### 4.3 Test Matrix Specifications

The test application must execute three distinct test suites:

#### Suite 1: Single-Engine Roundtrip Verification
1. **SQLite Roundtrip**: Ingest dataset into SQLite table -> Export to `/tmp/sqlite_test.csv` via `export_data` -> Import into `sqlite_restored` via `import_data` -> Assert row count and value checksum equality.
2. **PostgreSQL Roundtrip**: Repeat full export/import roundtrip cycle on PostgreSQL.
3. **Snowflake Roundtrip**: Repeat full export/import roundtrip cycle on Snowflake via local staging.

#### Suite 2: Cross-Engine Migration Matrix
The test suite must execute all six directional cross-database migration paths to validate format interoperability:

1. **SQLite to PostgreSQL**: `export_data` (SQLite) -> `import_data` (PostgreSQL).
2. **PostgreSQL to Snowflake**: `export_data` (PostgreSQL) -> `import_data` (Snowflake).
3. **Snowflake to SQLite**: `export_data` (Snowflake) -> `import_data` (SQLite).
4. **SQLite to Snowflake**: `export_data` (SQLite) -> `import_data` (Snowflake).
5. **PostgreSQL to SQLite**: `export_data` (PostgreSQL) -> `import_data` (SQLite).
6. **Snowflake to PostgreSQL**: `export_data` (Snowflake) -> `import_data` (PostgreSQL).

#### Suite 3: Error Handling & Boundary Testing
1. **Malformed File Handling**: Ingest a CSV containing invalid data types under `on_error: "abort"` (must fail cleanly with `isError: true`) versus `on_error: "continue"` (must load valid rows and return detailed warning logs).
2. **Schema Creation Test**: Import CSV data into a non-existent table with `auto_create_table: true` (must infer schema and succeed).

### 4.4 Automated Validation Assertions

For every execution path in the test matrix, the test application must automatically assert:

1. **Exact Row Count Parity**: Verify `COUNT(*)` matches perfectly between source and destination tables.
2. **Numeric Precision Integrity**: Verify `SUM(total_amount)` matches to within `0.0001` precision across engines.
3. **NULL Value Preservation**: Assert that `NULL` database values are preserved as true `NULL`s, rather than converting into empty strings `""` or text literals `"NULL"`.
4. **Special Character & Multiline Integrity**: Verify multiline string notes and international UTF-8 characters match byte-for-byte.

---

## 5. Security, Governance, and Feature Summary

### 5.1 Security Requirements

1. **Filesystem Sandboxing**: All MCP servers must sanitize `destination_path` and `source_path` inputs, restricting file operations to designated export directories (e.g. `/tmp/mcp_workspace/`) to prevent path traversal attacks (`../`).
2. **Shell Command Sanitization**: When invoking CLI sub-processes (such as `pg_dump` or SQLite CLI), parameters must be passed via secure argument arrays rather than raw shell string interpolation to prevent shell injection.
3. **Credential Security**: Database credentials, Snowflake stage keys, and cloud tokens must be loaded strictly from server environment variables or storage integrations, never exposed in client-facing MCP tool definitions.

### 5.2 RDBMS Feature Implementation Matrix

The following matrix compares how each database MCP server translates the standardized `export_data` and `import_data` tools into native execution mechanics:

| Architectural Metric / Feature | SQLite MCP Server | PostgreSQL MCP Server | Snowflake MCP Server |
| :--- | :--- | :--- | :--- |
| **Native Export Mechanism** | CLI `.mode csv` + `.output` or SQL query | SQL `COPY ... TO` file stream or `pg_dump` | SQL `COPY INTO @stage` + local `GET` download |
| **Native Import Mechanism** | CLI `.import --csv` or batch `INSERT` | SQL `COPY ... FROM` file stream or `pg_restore` | Local `PUT` to stage + `COPY INTO <table>` |
| **Default Interchange Format** | Local CSV (uncompressed or GZIP) | Local CSV (uncompressed or GZIP) | Local CSV staged via 100-250MB GZIP chunks |
| **Performance Tuning Flags** | `BEGIN TRANSACTION`, `WAL` mode, `synchronous=OFF` | Temporary `maintenance_work_mem`, defer indexes, `ANALYZE` | File splitting, parallel virtual warehouse nodes |
| **Schema Evolution Support** | Dynamic `CREATE TABLE` from CSV headers | Dynamic `CREATE TABLE` or custom archive restore | `INFER_SCHEMA` + `CREATE TABLE ... USING TEMPLATE` |
| **Error Handling Options** | Transaction rollback on abort | `ON_ERROR ignore/stop` with notice logs | `ON_ERROR = CONTINUE / ABORT_STATEMENT / SKIP_FILE` |
