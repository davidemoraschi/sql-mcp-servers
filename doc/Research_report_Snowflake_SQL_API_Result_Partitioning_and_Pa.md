# Researcher: Snowflake SQL API Result Partitioning and Pagination

We found **3 sources** detailing Snowflake SQL API query result handling, pagination, partition metadata, and response fetching.

---

## 🔧 Technical & Specs (3)

| # | Title | Source | Date |
|---|-------|--------|------|
| 1 | [Handling responses | Snowflake Documentation](https://docs.snowflake.com/en/developer-guide/sql-api/handling-responses) | Snowflake Official Docs | 2026 |
| 2 | [SQL API reference | Snowflake Documentation](https://docs.snowflake.com/en/developer-guide/sql-api/reference) | Snowflake Official Docs | 2026 |
| 3 | [Faster way to run Pagination Query in Snowflake](https://stackoverflow.com/questions/72393478/faster-way-to-run-pagination-query-in-snowflake) | Stack Overflow | 2024 |

**(1)** Detailed guide explaining how the Snowflake SQL API returns statement results in partitions. It details the `partitionInfo` array returned in the initial response metadata, row counts per partition, and how to issue subsequent `GET /api/v2/statements/{handle}?partition={partition_number}` requests to retrieve compressed (gzip) result partitions.
**(2)** Endpoint reference for Snowflake's REST-based SQL API detailing statement handles, HTTP status codes (200, 202, 422), and response data structures.
**(3)** Discussion on SQL-level pagination alternatives (e.g. `LIMIT` / `OFFSET` and `COUNT(*) OVER()`) versus API-level partition iteration.

---

## Key Themes Across Sources

- **Partition-Based Result Delivery**: Rather than streaming a single monolithic payload, Snowflake's SQL REST API splits query results into numbered partitions. The initial response returns metadata for all partitions (`partitionInfo`) along with partition 0 data.
- **Subsequent Partition Fetching**: Subsequent partitions are fetched via `GET /api/v2/statements/<handle>?partition=<partition_number>`, which returns gzip-compressed JSON data without duplicating column metadata.
- **Asynchronous Execution & Handles**: Long-running queries (>45s) return HTTP 202 with a `statementHandle` and `statementStatusUrl` to poll for completion before partition retrieval begins.

---

*Research conducted on November 22, 2026.*