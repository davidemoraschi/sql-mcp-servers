/*
 * mcp_loop.c - Shared stdio JSON-RPC loop for the MCP server executables
 *
 * Line budget constraint: < 450 lines
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <windows.h>
#endif

#include "json_rpc.h"
#include "rdbms_tools.h"
#include "mcp_loop.h"

#define BUFFER_SIZE (1024 * 1024) /* 1 MB stdin buffer */

void mcp_wipe_memory(void *ptr, size_t len) {
    if (!ptr || len == 0) return;
#if defined(__STDC_LIB_EXT1__) || defined(memset_explicit)
    memset_explicit(ptr, 0, len);
#elif defined(_WIN32)
    SecureZeroMemory(ptr, len);
#else
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) *p++ = 0;
#endif
}

void mcp_configure_stdio(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
}

int mcp_serve_stdio(void) {
    char *line_buffer = (char *)malloc(BUFFER_SIZE);
    if (!line_buffer) {
        fprintf(stderr, "[FATAL] Memory allocation failure for STDIN buffer.\n");
        return EXIT_FAILURE;
    }

    while (fgets(line_buffer, BUFFER_SIZE, stdin) != NULL) {
        size_t len = strlen(line_buffer);
        while (len > 0 && (line_buffer[len - 1] == '\n' || line_buffer[len - 1] == '\r')) {
            line_buffer[--len] = '\0';
        }
        if (len == 0) continue;

        JsonRpcRequest req;
        if (!json_rpc_parse_request(line_buffer, &req)) {
            json_rpc_send_error("null", JSONRPC_ERR_PARSE, "Parse error: Invalid JSON payload", NULL);
            continue;
        }

        if (strcmp(req.method, "initialize") == 0) {
            tools_handle_initialize(req.id);
        } else if (strcmp(req.method, "notifications/initialized") == 0 || strcmp(req.method, "initialized") == 0) {
            fprintf(stderr, "[MCP] Client sent initialized notification.\n");
        } else if (strcmp(req.method, "ping") == 0) {
            json_rpc_send_result(req.id, "{}");
        } else if (strcmp(req.method, "server/discover") == 0) {
            tools_handle_discover(req.id);
        } else if (strcmp(req.method, "tools/list") == 0) {
            tools_handle_list(req.id);
        } else if (strcmp(req.method, "tools/call") == 0) {
            tools_handle_call(req.id, req.params);
        } else {
            json_rpc_send_error(req.id, JSONRPC_ERR_METHOD_NOT_FOUND, "Method not found", NULL);
        }

        json_rpc_free_request(&req);
    }

    free(line_buffer);
    return EXIT_SUCCESS;
}
