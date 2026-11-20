/*
 * mcp_loop.h - Shared stdio JSON-RPC loop for the MCP server executables
 *
 * Line budget constraint: < 150 lines
 */

#ifndef MCP_LOOP_H
#define MCP_LOOP_H

#include <stddef.h>

/* Binary stdin/stdout on Windows, unbuffered streams. Call once at startup. */
void mcp_configure_stdio(void);

/* Zero secret memory in a way the optimiser may not remove. */
void mcp_wipe_memory(void *ptr, size_t len);

/*
 * Read newline-delimited JSON-RPC requests from stdin and dispatch them to the
 * rdbms_tools handlers until EOF. Returns 0 on a clean exit, non-zero if the
 * input buffer could not be allocated.
 */
int mcp_serve_stdio(void);

#endif /* MCP_LOOP_H */
