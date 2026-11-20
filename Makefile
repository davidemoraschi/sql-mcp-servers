# Cross-Platform Makefile for Standalone SQLite / Snowflake MCP Servers & Test Runner
CC ?= gcc
CFLAGS ?= -std=c11 -O2  -fomit-frame-pointer -ffunction-sections -fdata-sections -Wall -Wextra -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAS_EMBEDDED_SQLITE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -Iinclude
LDFLAGS ?= -O2  -Wl,--gc-sections -Wl,-s
SRCS = src/main.c src/mcp_loop.c src/json_rpc.c src/rdbms_tools.c src/db_sqlite.c src/sqlite3.c
OBJS = $(SRCS:.c=.o)
TARGET = sqlite-mcp-server
TEST_SRCS = src/test_runner.c
TEST_OBJS = $(TEST_SRCS:.c=.o)
TEST_TARGET = mcp_test_runner

# Snowflake server: WinHTTP on Windows, libcurl elsewhere.
SF_SRCS = src/main_snowflake.c src/mcp_loop.c src/json_rpc.c src/rdbms_tools.c src/db_snowflake.c src/sf_query.c src/sf_auth.c src/sf_buf.c src/sf_http.c src/sf_loopback.c src/sf_browser.c
SF_OBJS = $(SF_SRCS:.c=.o)
SF_TARGET = snowflake-mcp-server
ifeq ($(OS),Windows_NT)
SF_LIBS = -lwinhttp -lws2_32 -lshell32
SF_CFLAGS =
SF_ENABLED = yes
else
SF_LIBS = $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
SF_CFLAGS = $(shell pkg-config --cflags libcurl 2>/dev/null)
SF_ENABLED = $(shell pkg-config --exists libcurl 2>/dev/null && echo yes)
endif

# 'all' builds the Snowflake server only when libcurl is available;
# 'make snowflake-mcp-server' always tries to build it.
all: $(TARGET) $(TEST_TARGET) $(if $(SF_ENABLED),$(SF_TARGET))
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread
$(TEST_TARGET): $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread
$(SF_TARGET): $(SF_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(SF_LIBS) -lm
src/sf_http.o: CFLAGS += $(SF_CFLAGS)
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@
test: all
	./$(TEST_TARGET) ./$(TARGET)
clean:
	rm -f $(OBJS) $(TEST_OBJS) $(SF_OBJS) $(TARGET) $(TARGET).exe $(TEST_TARGET) $(TEST_TARGET).exe $(SF_TARGET) $(SF_TARGET).exe
.PHONY: all test clean
