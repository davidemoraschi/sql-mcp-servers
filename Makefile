# Cross-Platform Makefile for SQLite / Snowflake / PostgreSQL MCP Servers & Test Runners
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -fomit-frame-pointer -ffunction-sections -fdata-sections \
            -Wall -Wextra -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
            -DHAS_EMBEDDED_SQLITE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -Iinclude
LDFLAGS ?= -O2 -Wl,--gc-sections -Wl,-s

SQLITE_SRCS = src/sqlite/main.c src/shared/mcp_loop.c src/shared/json_rpc.c src/shared/rdbms_tools.c \
              src/sqlite/db_sqlite_core.c src/sqlite/db_sqlite_transfer.c src/sqlite/sqlite3.c
SQLITE_OBJS = $(SQLITE_SRCS:.c=.o)

TEST_SQLITE_SRCS = src/sqlite/test_runner_sqlite.c
TEST_SQLITE_OBJS = $(TEST_SQLITE_SRCS:.c=.o)

SF_SRCS = src/snowflake/main_snowflake.c src/shared/mcp_loop.c src/shared/json_rpc.c src/shared/rdbms_tools.c \
          src/snowflake/db_snowflake_core.c src/snowflake/db_snowflake_transfer.c \
          src/snowflake/sf_query.c src/snowflake/sf_auth.c src/snowflake/sf_buf.c \
          src/snowflake/sf_http.c src/snowflake/sf_loopback.c src/snowflake/sf_browser.c
SF_OBJS = $(SF_SRCS:.c=.o)

PG_SRCS = src/postgres/main_postgres.c src/shared/mcp_loop.c src/shared/json_rpc.c src/shared/rdbms_tools.c \
          src/postgres/db_postgres.c src/postgres/db_postgres_transfer.c src/postgres/pg_md5.c src/postgres/pg_scram.c src/snowflake/sf_buf.c
PG_OBJS = $(PG_SRCS:.c=.o)

BQ_SRCS = src/bigquery/main_bigquery.c src/bigquery/bq_oauth.c src/bigquery/db_bigquery.c src/bigquery/db_bigquery_schema.c src/bigquery/db_bigquery_transfer.c \
          src/shared/mcp_loop.c src/shared/json_rpc.c src/shared/rdbms_tools.c src/snowflake/sf_buf.c src/snowflake/sf_http.c
BQ_OBJS = $(BQ_SRCS:.c=.o)

ATHENA_SRCS = src/athena/main_athena.c src/athena/aws_sha256.c src/athena/aws_credentials.c src/athena/aws_sigv4.c \
              src/athena/db_athena.c src/athena/db_athena_schema.c src/athena/db_athena_transfer.c \
              src/shared/mcp_loop.c src/shared/json_rpc.c src/shared/rdbms_tools.c src/snowflake/sf_buf.c src/snowflake/sf_http.c
ATHENA_OBJS = $(ATHENA_SRCS:.c=.o)

TEST_PG_SRCS = src/postgres/test_runner_postgres.c
TEST_PG_OBJS = $(TEST_PG_SRCS:.c=.o)

ifeq ($(OS),Windows_NT)
SF_LIBS    = -lwinhttp -lws2_32 -lshell32
    BQ_LIBS    = -lwinhttp -lws2_32 -lshell32 -lbcrypt
ATHENA_LIBS = -lwinhttp
PG_LIBS    = -lws2_32
SF_CFLAGS  =
SF_ENABLED = yes
else
SF_LIBS    = $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
    BQ_LIBS    = $(SF_LIBS)
ATHENA_LIBS = $(SF_LIBS)
SF_CFLAGS  = $(shell pkg-config --cflags libcurl 2>/dev/null)
SF_ENABLED = $(shell pkg-config --exists libcurl 2>/dev/null && echo yes)
PG_LIBS    =
endif

all: sqlite-mcp-server mcp_test_runner_sqlite postgres-mcp-server mcp_test_runner_postgres $(if $(SF_ENABLED),snowflake-mcp-server bigquery-mcp-server athena-mcp-server)

sqlite-mcp-server: $(SQLITE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread

mcp_test_runner_sqlite: $(TEST_SQLITE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread

snowflake-mcp-server: $(SF_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(SF_LIBS) -lm

postgres-mcp-server: $(PG_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(PG_LIBS) -lm -lpthread

bigquery-mcp-server: $(BQ_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(BQ_LIBS) -lm

athena-mcp-server: $(ATHENA_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(ATHENA_LIBS) -lm

mcp_test_runner_postgres: $(TEST_PG_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread

src/snowflake/sf_http.o: CFLAGS += $(SF_CFLAGS)
src/bigquery/db_bigquery.o src/bigquery/db_bigquery_transfer.o src/athena/db_athena.o src/snowflake/sf_http.o: CFLAGS += $(SF_CFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	./mcp_test_runner_sqlite ./sqlite-mcp-server

clean:
	rm -f $(SQLITE_OBJS) $(TEST_SQLITE_OBJS) $(SF_OBJS) $(PG_OBJS) $(ATHENA_OBJS) $(TEST_PG_OBJS) \
	      sqlite-mcp-server sqlite-mcp-server.exe \
	      mcp_test_runner_sqlite mcp_test_runner_sqlite.exe \
	      snowflake-mcp-server snowflake-mcp-server.exe \
	      bigquery-mcp-server bigquery-mcp-server.exe \
	      athena-mcp-server athena-mcp-server.exe \
	      postgres-mcp-server postgres-mcp-server.exe \
	      mcp_test_runner_postgres mcp_test_runner_postgres.exe

.PHONY: all test clean
