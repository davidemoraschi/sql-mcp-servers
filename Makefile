# Cross-Platform Makefile for Standalone SQLite / Snowflake MCP Servers & Test Runner
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -fomit-frame-pointer -ffunction-sections -fdata-sections \
            -Wall -Wextra -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
            -DHAS_EMBEDDED_SQLITE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -Iinclude
LDFLAGS ?= -O2 -Wl,--gc-sections -Wl,-s

SQLITE_SRCS = src/sqlite/main.c src/shared/mcp_loop.c src/shared/json_rpc.c src/shared/rdbms_tools.c \
              src/sqlite/db_sqlite_core.c src/sqlite/db_sqlite_transfer.c src/sqlite/sqlite3.c
SQLITE_OBJS = $(SQLITE_SRCS:.c=.o)

TEST_SRCS = src/shared/test_runner.c
TEST_OBJS = $(TEST_SRCS:.c=.o)

SF_SRCS = src/snowflake/main_snowflake.c src/shared/mcp_loop.c src/shared/json_rpc.c src/shared/rdbms_tools.c \
          src/snowflake/db_snowflake_core.c src/snowflake/db_snowflake_transfer.c \
          src/snowflake/sf_query.c src/snowflake/sf_auth.c src/snowflake/sf_buf.c \
          src/snowflake/sf_http.c src/snowflake/sf_loopback.c src/snowflake/sf_browser.c
SF_OBJS = $(SF_SRCS:.c=.o)

ifeq ($(OS),Windows_NT)
SF_LIBS    = -lwinhttp -lws2_32 -lshell32
SF_CFLAGS  =
SF_ENABLED = yes
else
SF_LIBS    = $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
SF_CFLAGS  = $(shell pkg-config --cflags libcurl 2>/dev/null)
SF_ENABLED = $(shell pkg-config --exists libcurl 2>/dev/null && echo yes)
endif

all: sqlite-mcp-server mcp_test_runner_sqlite $(if $(SF_ENABLED),snowflake-mcp-server)

sqlite-mcp-server: $(SQLITE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread

mcp_test_runner_sqlite: $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread

snowflake-mcp-server: $(SF_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(SF_LIBS) -lm

src/snowflake/sf_http.o: CFLAGS += $(SF_CFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	./mcp_test_runner_sqlite ./sqlite-mcp-server

clean:
	rm -f $(SQLITE_OBJS) $(TEST_OBJS) $(SF_OBJS) \
	      sqlite-mcp-server sqlite-mcp-server.exe \
	      mcp_test_runner_sqlite mcp_test_runner_sqlite.exe \
	      snowflake-mcp-server snowflake-mcp-server.exe

.PHONY: all test clean
