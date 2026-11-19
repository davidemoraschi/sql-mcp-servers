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

$CC -std=c11 -O2 -flto -fomit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
    -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE \
    -Iinclude \
    src/main.c src/json_rpc.c src/rdbms_tools.c src/db_sqlite.c src/sqlite3.c \
    -Wl,--gc-sections -Wl,-s \
    -o bin/sqlite-mcp-server -lm -lpthread

echo "[SUCCESS] Binary compiled successfully: bin/sqlite-mcp-server"
