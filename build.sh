#!/usr/bin/env bash
set -e

echo "==================================================="
echo "Building Standalone SQLite MCP Server for Linux"
echo "==================================================="

mkdir -p bin

if command -v gcc >/dev/null 2>&1; then
    CC=gcc
elif command -v clang >/dev/null 2>&1; then
    CC=clang
else
    echo "[ERROR] Neither gcc nor clang was found in PATH."
    exit 1
fi

echo "[INFO] Using compiler: $CC"

$CC -std=c11 -O2  -fomit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAS_EMBEDDED_SQLITE \
    -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
    -Iinclude \
    src/main.c src/mcp_loop.c src/json_rpc.c src/rdbms_tools.c src/db_sqlite.c src/sqlite3.c \
    -Wl,--gc-sections -Wl,-s \
    -o bin/sqlite-mcp-server -lm -lpthread

echo "[SUCCESS] Server compiled: bin/sqlite-mcp-server"

echo "[INFO] Compiling PostgreSQL MCP Server..."
$CC -std=c11 -O2 -fomit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -DNDEBUG -D_POSIX_C_SOURCE=200112L -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
    -Iinclude \
    src/main_postgres.c src/mcp_loop.c src/json_rpc.c src/rdbms_tools.c src/db_postgres.c src/pg_md5.c src/pg_scram.c src/sf_buf.c \
    -Wl,--gc-sections -Wl,-s \
    -o bin/postgres-mcp-server -lm
echo "[SUCCESS] Server compiled: bin/postgres-mcp-server"

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl; then
    echo "[INFO] Compiling Snowflake MCP Server (libcurl found)..."
    $CC -std=c11 -O2  -fomit-frame-pointer \
        -ffunction-sections -fdata-sections \
        -DNDEBUG -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
        -Iinclude $(pkg-config --cflags libcurl) \
        src/main_snowflake.c src/mcp_loop.c src/json_rpc.c src/rdbms_tools.c src/db_snowflake.c \
        src/sf_query.c src/sf_auth.c src/sf_buf.c src/sf_http.c src/sf_loopback.c src/sf_browser.c \
        -Wl,--gc-sections -Wl,-s \
        -o bin/snowflake-mcp-server $(pkg-config --libs libcurl) -lm
    echo "[SUCCESS] Server compiled: bin/snowflake-mcp-server"
else
    echo "[WARN] libcurl development files not found: skipping snowflake-mcp-server (install libcurl4-openssl-dev)."
fi

echo "[INFO] Compiling Test Runner..."
$CC -std=c11 -O2 -fomit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
    -Iinclude \
    src/test_runner.c \
    -Wl,--gc-sections -Wl,-s \
    -o bin/mcp_test_runner -lm -lpthread
echo "[SUCCESS] Test Runner compiled: bin/mcp_test_runner"

echo "[INFO] Compiling PostgreSQL Test Runner..."
$CC -std=c11 -O2 -fomit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
    -Iinclude \
    src/test_runner_postgres.c \
    -Wl,--gc-sections -Wl,-s \
    -o bin/mcp_test_runner_postgres -lm -lpthread
echo "[SUCCESS] PostgreSQL Test Runner compiled: bin/mcp_test_runner_postgres"

#echo "==================================================="
#echo "Running Integration Tests..."
#echo "==================================================="
#./bin/mcp_test_runner ./bin/sqlite-mcp-server
#echo "[SUCCESS] All integration tests passed!"
