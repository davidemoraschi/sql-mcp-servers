# Cross-Platform Makefile for Standalone SQLite MCP Server & Test Runner
CC ?= gcc
CFLAGS ?= -std=c11 -O2  -fomit-frame-pointer -ffunction-sections -fdata-sections -Wall -Wextra -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAS_EMBEDDED_SQLITE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -Iinclude
LDFLAGS ?= -O2  -Wl,--gc-sections -Wl,-s

SRCS = src/main.c src/json_rpc.c src/rdbms_tools.c src/db_sqlite.c src/sqlite3.c
OBJS = $(SRCS:.c=.o)
TARGET = sqlite-mcp-server

TEST_SRCS = src/test_runner.c
TEST_OBJS = $(TEST_SRCS:.c=.o)
TEST_TARGET = mcp_test_runner

all: $(TARGET) $(TEST_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread

$(TEST_TARGET): $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	./$(TEST_TARGET) ./$(TARGET)

clean:
	rm -f $(OBJS) $(TEST_OBJS) $(TARGET) $(TARGET).exe $(TEST_TARGET) $(TEST_TARGET).exe

.PHONY: all test clean
