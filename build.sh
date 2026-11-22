#!/usr/bin/env bash
set -e

echo "==================================================="
echo "Building Standalone SQLite MCP Server for Linux"
echo "==================================================="

mkdir -p bin

if command -v gcc >/dev/null 2>&1; then CC=gcc
elif command -v clang >/dev/null 2>&1; then CC=clang
else echo "[ERROR] Neither gcc nor clang was found in PATH." && exit 1
fi
echo "[INFO] Using compiler: $CC"

$CC -std=c11 -O2 -fomit-frame-pointer -ffunction-sections -fdata-sections \
    -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAS_EMBEDDED_SQLITE \
    -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
    -Iinclude \
    src/sqlite/main.c src/shared/mcp_loop.c src/shared/json_rpc.c src/shared/rdbms_tools.c \
    src/sqlite/db_sqlite_core.c src/sqlite/db_sqlite_transfer.c src/sqlite/sqlite3.c \
    -Wl,--gc-sections -Wl,-s \
    -o bin/sqlite-mcp-server -lm -lpthread
echo "[SUCCESS] Server compiled: bin/sqlite-mcp-server"

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl; then
    echo "[INFO] Compiling Snowflake MCP Server (libcurl found)..."
    $CC -std=c11 -O2 -fomit-frame-pointer -ffunction-sections -fdata-sections \
        -DNDEBUG -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
        -Iinclude $(pkg-config --cflags libcurl) \
        src/snowflake/main_snowflake.c src/shared/mcp_loop.c src/shared/json_rpc.c src/shared/rdbms_tools.c \
        src/snowflake/db_snowflake_core.c src/snowflake/db_snowflake_transfer.c \
        src/snowflake/sf_query.c src/snowflake/sf_auth.c src/snowflake/sf_buf.c \
        src/snowflake/sf_http.c src/snowflake/sf_loopback.c src/snowflake/sf_browser.c \
        -Wl,--gc-sections -Wl,-s \
        -o bin/snowflake-mcp-server $(pkg-config --libs libcurl) -lm
    echo "[SUCCESS] Server compiled: bin/snowflake-mcp-server"
else
    echo "[WARN] libcurl not found: skipping snowflake-mcp-server (install libcurl4-openssl-dev)."
fi

echo "[INFO] Compiling Test Runner..."
$CC -std=c11 -O2 -fomit-frame-pointer -ffunction-sections -fdata-sections \
    -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
    -Iinclude \
    src/shared/test_runner.c \
    -Wl,--gc-sections -Wl,-s \
    -o bin/mcp_test_runner_sqlite -lm -lpthread
echo "[SUCCESS] Test Runner compiled: bin/mcp_test_runner"
