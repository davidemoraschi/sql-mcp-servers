# Snowflake Result Pagination

## Background

When Snowflake executes a query it serialises the result into one or more
**partitions**. Partition 0 is returned inline in the JSON response body
(`data.rowset`). If the result is large, Snowflake splits the remainder into
additional partitions that must be fetched separately.

This is completely independent of any SQL `LIMIT`/`OFFSET` you write. A single
`SELECT * FROM t LIMIT 1000` can still produce multiple partitions if Snowflake
decides the 1 000-row payload is large enough to split.

---

## What the API Returns

### Query response (`POST /queries/v1/query-request`)

```json
{
  "success": true,
  "data": {
    "queryResultFormat": "json",
    "rowtype": [ { "name": "col1", ... }, ... ],
    "rowset": [ [...], [...], ... ],          // partition 0 rows (inline)
    "statementHandle": "01b3f...-0001",       // handle for fetching more
    "partitionInfo": [                        // present only when total > 1
      { "rowCount": 1500, "uncompressedSize": 204800 },   // partition 1
      { "rowCount": 843,  "uncompressedSize": 114688 }    // partition 2
    ]
  }
}
```

- `rowset` — the rows for partition 0, always present.
- `statementHandle` — opaque identifier for the executed statement.
- `partitionInfo` — array describing partitions **1 through N**. Its length is
  `partition_count - 1`. If absent or empty, partition 0 is the only partition.

### Partition fetch (`GET /api/v2/statements/{handle}?partition={n}`)

Returns a simpler envelope:

```json
{
  "rowset": [ [...], [...], ... ]
}
```

No `rowtype` — reuse the column descriptors from the original query response.

---

## How `SfQueryResult` Exposes This

```c
typedef struct {
    bool         success;
    char        *error_message;
    JsonValue   *root;              // owns rowtype, rowset, everything below
    const JsonValue *rowtype;       // column descriptors (partition 0 only)
    const JsonValue *rowset;        // rows for the current partition
    bool         more_chunks;       // true  → additional partitions exist
    int          partition_count;   // total partitions (1 = inline only)
    char         statement_handle[64]; // needed to fetch partitions 1..N
} SfQueryResult;
```

`sf_session_query()` fills all fields from the initial response.
`sf_session_fetch_partition(handle, n)` fills only `rowset` (and `root`).

---

## The Bug That Was Fixed

The original export used `LIMIT`/`OFFSET` pagination:

```c
// WRONG — a page of exactly PAGE_SIZE rows may still be split into partitions.
// sf_row_count() only counts the inline rowset, so it returns < PAGE_SIZE
// even though more rows exist in partition 1+. The loop stops too early.
for (long offset = 0; ; offset += PAGE_SIZE) {
    SfQueryResult q = sf_session_query("SELECT * FROM t LIMIT 100 OFFSET %ld", offset);
    size_t rows = sf_row_count(&q);   // may be 0 or < 100 due to partitioning
    if (rows < PAGE_SIZE) break;      // ← exits before all rows are written
}
```

Symptom: exports stopped at ~2 400 rows (the inline portion of the last
LIMIT/OFFSET page) even though the table had more rows.

---

## The Correct Pattern

Issue **one query**, write partition 0, then fetch the rest:

```c
SfQueryResult q = sf_session_query("SELECT * FROM t");

// Partition 0 — inline in the response
write_rowset(f, q.rowset, ...);

// Partitions 1..N — fetched individually
if (q.more_chunks && q.statement_handle[0]) {
    for (int p = 1; p < q.partition_count; p++) {
        SfQueryResult pq = sf_session_fetch_partition(q.statement_handle, p);
        if (pq.success)
            write_rowset(f, pq.rowset, ...);
        sf_query_free(&pq);
    }
}

sf_query_free(&q);
```

This is what `db_export_data` in `src/snowflake/db_snowflake_transfer.c` does.

---

## Key Rules

| Rule | Reason |
|---|---|
| Never use `LIMIT`/`OFFSET` for full-table export | Snowflake may split any page into partitions, making `sf_row_count()` unreliable as a termination signal |
| Always check `more_chunks` before assuming one partition | Even small result sets can be split on busy warehouses |
| Reuse `rowtype` from partition 0 for all subsequent partitions | Partition fetch responses do not include column metadata |
| Call `sf_query_free()` on every `SfQueryResult` | Each partition fetch allocates its own `root` JSON tree |
| `partition_count` is the **total** count including partition 0 | Loop from `p = 1` to `p < partition_count` |
