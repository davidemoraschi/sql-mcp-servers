/*
 * test_runner_postgres.c - Integration Test Harness for postgres-mcp-server
 *
 * Spawns postgres-mcp-server over STDIO IPC, sends JSON-RPC requests,
 * and asserts strict protocol compliance for dual-channel payloads.
 *
 * Usage: test_runner_postgres <path/to/postgres-mcp-server>
 * Connection is configured via the standard PG* environment variables
 * (PGHOST, PGPORT, PGDATABASE, PGUSER, PGPASSWORD) before running.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
  #include <windows.h>
  typedef struct { HANDLE hChildStdInWrite; HANDLE hChildStdOutRead; PROCESS_INFORMATION procInfo; } ProcessPipe;
#else
  #include <unistd.h>
  #include <sys/wait.h>
  #include <sys/types.h>
  typedef struct { int fd_in; int fd_out; pid_t pid; } ProcessPipe;
#endif

static bool spawn_server(const char *exe_path, ProcessPipe *proc) {
    memset(proc, 0, sizeof(ProcessPipe));
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hStdInRead, hStdInWrite, hStdOutRead, hStdOutWrite;
    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) return false;
    if (!SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0)) return false;
    if (!CreatePipe(&hStdInRead, &hStdInWrite, &sa, 0)) return false;
    if (!SetHandleInformation(hStdInWrite, HANDLE_FLAG_INHERIT, 0)) return false;
    STARTUPINFOA si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = hStdOutWrite; si.hStdInput = hStdInRead; si.dwFlags = STARTF_USESTDHANDLES;
    char cmdline[1024]; snprintf(cmdline, sizeof(cmdline), "\"%s\"", exe_path);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &proc->procInfo)) {
        CloseHandle(hStdOutRead); CloseHandle(hStdOutWrite); CloseHandle(hStdInRead); CloseHandle(hStdInWrite);
        return false;
    }
    CloseHandle(hStdOutWrite); CloseHandle(hStdInRead);
    proc->hChildStdInWrite = hStdInWrite; proc->hChildStdOutRead = hStdOutRead;
    return true;
#else
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        dup2(pipe_in[0], STDIN_FILENO); dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[0]); close(pipe_in[1]); close(pipe_out[0]); close(pipe_out[1]);
        char *args[] = { (char *)exe_path, NULL }; execv(exe_path, args); exit(127);
    }
    close(pipe_in[0]); close(pipe_out[1]);
    proc->fd_in = pipe_in[1]; proc->fd_out = pipe_out[0]; proc->pid = pid;
    return true;
#endif
}

static bool send_json_line(ProcessPipe *proc, const char *json) {
    size_t len = strlen(json);
    char *buf = (char *)malloc(len + 2);
    if (!buf) return false;
    snprintf(buf, len + 2, "%s\n", json);
#ifdef _WIN32
    DWORD written = 0;
    BOOL res = WriteFile(proc->hChildStdInWrite, buf, (DWORD)strlen(buf), &written, NULL);
    free(buf); return res && written > 0;
#else
    ssize_t res = write(proc->fd_in, buf, strlen(buf));
    free(buf); return res > 0;
#endif
}

static bool read_json_line(ProcessPipe *proc, char *out_buf, size_t max_len) {
    size_t idx = 0;
    while (idx + 1 < max_len) {
        char ch = 0;
#ifdef _WIN32
        DWORD bytesRead = 0;
        if (!ReadFile(proc->hChildStdOutRead, &ch, 1, &bytesRead, NULL) || bytesRead == 0) break;
#else
        ssize_t res = read(proc->fd_out, &ch, 1); if (res <= 0) break;
#endif
        if (ch == '\r') continue;
        if (ch == '\n') { out_buf[idx] = '\0'; return true; }
        out_buf[idx++] = ch;
    }
    out_buf[idx] = '\0'; return idx > 0;
}

static void close_server(ProcessPipe *proc) {
#ifdef _WIN32
    CloseHandle(proc->hChildStdInWrite); CloseHandle(proc->hChildStdOutRead);
    WaitForSingleObject(proc->procInfo.hProcess, 2000);
    CloseHandle(proc->procInfo.hProcess); CloseHandle(proc->procInfo.hThread);
#else
    close(proc->fd_in); close(proc->fd_out);
    int status; waitpid(proc->pid, &status, 0);
#endif
}

static bool verify_dual_channel(const char *resp, int id) {
    char pat[32]; snprintf(pat, sizeof(pat), "\"id\":%d", id);
    return strstr(resp, pat) && strstr(resp, "\"resultType\":\"complete\"") &&
           strstr(resp, "\"content\":[") && strstr(resp, "\"type\":\"text\"") &&
           strstr(resp, "\"structuredContent\":");
}

#define RUN(id, label, send_expr, pass_expr) do { \
    printf("[TC-%02d] %s... ", (id), (label)); \
    send_expr; \
    if (read_json_line(&proc, resp, sizeof(resp)) && (pass_expr)) { printf("PASS\n"); passed++; } \
    else { printf("FAIL (Resp: %.120s)\n", resp); failed++; } \
} while(0)

int main(int argc, char **argv) {
    const char *server_exe = (argc > 1) ? argv[1] : "./bin/postgres-mcp-server";
    printf("===================================================\n");
    printf("MCP Integration Test Runner (PostgreSQL)\n");
    printf("Target Server: %s\n", server_exe);
    printf("===================================================\n\n");

    ProcessPipe proc;
    if (!spawn_server(server_exe, &proc)) {
        fprintf(stderr, "[FAIL] Could not spawn server executable: %s\n", server_exe);
        return EXIT_FAILURE;
    }

    int passed = 0, failed = 0;
    char resp[8192];

    /* TC-01: Legacy 2025-11-25 Handshake */
    RUN(1, "Legacy initialize handshake (2025-11-25)",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\"}}"),
        strstr(resp, "\"id\":1") && strstr(resp, "protocolVersion"));

    /* TC-02: System Ping */
    RUN(2, "System ping",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}"),
        strstr(resp, "\"id\":2") && strstr(resp, "\"result\":{}"));

    /* TC-03: Modern 2026-07-28 Discovery */
    RUN(3, "Modern server/discover (2026-07-28)",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"server/discover\"}"),
        strstr(resp, "\"id\":3") && strstr(resp, "2026-07-28"));

    /* TC-04: Tool Discovery - verify PG-specific profile (no backup_database) */
    RUN(4, "Tool discovery - postgres profile (no backup)",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\"}"),
        strstr(resp, "\"id\":4") && strstr(resp, "read_query") && !strstr(resp, "backup_database"));

    /* TC-05: Basic read_query dual-channel (SELECT 1) */
    RUN(5, "Basic read_query dual-channel (SELECT 1)",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"read_query\",\"arguments\":{\"sql\":\"SELECT 1\"}}}"),
        verify_dual_channel(resp, 5));

    /* TC-06: PostgreSQL version query */
    RUN(6, "read_query - SELECT version()",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"read_query\",\"arguments\":{\"sql\":\"SELECT version()\"}}}"),
        verify_dual_channel(resp, 6) && strstr(resp, "PostgreSQL"));

    /* TC-07: information_schema tables query */
    RUN(7, "read_query - information_schema.tables",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"read_query\",\"arguments\":{\"sql\":\"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY table_name LIMIT 5\"}}}"),
        verify_dual_channel(resp, 7));

    /* TC-08: list_tables tool */
    RUN(8, "list_tables tool",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"list_tables\",\"arguments\":{}}}"),
        verify_dual_channel(resp, 8));

    /* TC-09: check_database_health - verify PG-specific fields */
    RUN(9, "check_database_health - PG fields",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"check_database_health\",\"arguments\":{}}}"),
        verify_dual_channel(resp, 9) && strstr(resp, "database") && strstr(resp, "version"));

    /* TC-10: explain_query */
    RUN(10, "explain_query - SELECT 1",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"explain_query\",\"arguments\":{\"sql\":\"SELECT 1\"}}}"),
        verify_dual_channel(resp, 10));

    /* TC-11: infer_relationships (may return empty result set - still dual-channel) */
    RUN(11, "infer_relationships",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"infer_relationships\",\"arguments\":{}}}"),
        verify_dual_channel(resp, 11));

    /* TC-12: generate_erd */
    RUN(12, "generate_erd",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"generate_erd\",\"arguments\":{}}}"),
        verify_dual_channel(resp, 12) && strstr(resp, "plantuml"));

    /* TC-13: write_query blocked in READONLY mode detection (server may be READWRITE - just check response is valid) */
    RUN(13, "write_query - CREATE TEMP TABLE",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\",\"params\":{\"name\":\"write_query\",\"arguments\":{\"sql\":\"CREATE TEMP TABLE _mcp_test (id int)\"}}}"),
        strstr(resp, "\"id\":13") && (verify_dual_channel(resp, 13) || strstr(resp, "error")));

    /* TC-14: read_query blocked for write statement */
    RUN(14, "read_query rejects INSERT statement",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\",\"params\":{\"name\":\"read_query\",\"arguments\":{\"sql\":\"INSERT INTO _mcp_test VALUES (1)\"}}}"),
        strstr(resp, "\"id\":14") && strstr(resp, "error"));

    /* TC-15: Identifier sanitization */
    RUN(15, "Identifier sanitization - describe_table with injection attempt",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\",\"params\":{\"name\":\"describe_table\",\"arguments\":{\"table_name\":\"users; DROP TABLE users;\"}}}"),
        strstr(resp, "\"id\":15") && strstr(resp, "error"));

    /* TC-16: backup_database not offered for PostgreSQL */
    RUN(16, "backup_database not available for PostgreSQL",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\",\"params\":{\"name\":\"backup_database\",\"arguments\":{\"target_path\":\"/tmp/test.bak\"}}}"),
        strstr(resp, "\"id\":16") && strstr(resp, "error"));

    /* TC-17: Missing parameters error handling */
    RUN(17, "Missing parameters error handling",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"tools/call\"}"),
        strstr(resp, "\"id\":17") && strstr(resp, "error"));

    /* TC-18: Unknown method error handling */
    RUN(18, "Unknown method error handling",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"unknown_method\"}"),
        strstr(resp, "\"id\":18") && strstr(resp, "error"));

    /* TC-19: Create transfer_test table */
    RUN(19, "Create transfer_test table",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"tools/call\",\"params\":{\"name\":\"write_query\",\"arguments\":{\"sql\":\"CREATE TABLE IF NOT EXISTS transfer_test (id SERIAL PRIMARY KEY, val TEXT, num NUMERIC)\"}}}" ),
        verify_dual_channel(resp, 19));

    /* TC-20: Populate with 1000 rows */
    RUN(20, "Populate transfer_test with 1000 rows",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\",\"params\":{\"name\":\"write_query\",\"arguments\":{\"sql\":\"INSERT INTO transfer_test (val, num) SELECT md5(i::text), round((i * 3.14159)::numeric, 4) FROM generate_series(1,1000) AS i\"}}}" ),
        verify_dual_channel(resp, 20));

    /* TC-21: Export to CSV */
    RUN(21, "Export transfer_test to CSV",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\",\"params\":{\"name\":\"export_data\",\"arguments\":{\"target\":\"transfer_test\",\"destination_path\":\"transfer_test_export.csv\",\"format\":\"csv\"}}}" ),
        verify_dual_channel(resp, 21) && strstr(resp, "success"));

    /* TC-22: Import CSV into transfer_test_imported */
    RUN(22, "Import CSV into transfer_test_imported",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\",\"params\":{\"name\":\"import_data\",\"arguments\":{\"source_path\":\"transfer_test_export.csv\",\"destination_table\":\"transfer_test_imported\",\"format\":\"csv\",\"auto_create_table\":true,\"optimize_speed\":true}}}" ),
        verify_dual_channel(resp, 22) && strstr(resp, "success"));

    /* TC-23: Verify row count parity */
    RUN(23, "Verify row count parity (1000 rows in both tables)",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"tools/call\",\"params\":{\"name\":\"read_query\",\"arguments\":{\"sql\":\"SELECT (SELECT COUNT(*) FROM transfer_test) AS src, (SELECT COUNT(*) FROM transfer_test_imported) AS dst\"}}}" ),
        verify_dual_channel(resp, 23) && strstr(resp, "\"src\":\"1000\"") && strstr(resp, "\"dst\":\"1000\""));

    /* TC-24: Cleanup */
    RUN(24, "Cleanup transfer test tables",
        send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"tools/call\",\"params\":{\"name\":\"write_query\",\"arguments\":{\"sql\":\"DROP TABLE IF EXISTS transfer_test; DROP TABLE IF EXISTS transfer_test_imported\"}}}" ),
        verify_dual_channel(resp, 24));

    close_server(&proc);

    printf("\n===================================================\n");
    printf("Test Results: %d Passed, %d Failed\n", passed, failed);
    printf("===================================================\n");
    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
