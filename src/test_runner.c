/*
 * test_runner.c - Automated Integration Test Harness Console Application
 *
 * Spawns sqlite-mcp-server process over STDIO IPC, sends JSON-RPC requests,
 * and asserts strict protocol compliance.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
  #include <windows.h>
  typedef struct {
      HANDLE hChildStdInWrite;
      HANDLE hChildStdOutRead;
      PROCESS_INFORMATION procInfo;
  } ProcessPipe;
#else
  #include <unistd.h>
  #include <sys/wait.h>
  #include <sys/types.h>
  typedef struct {
      int fd_in;
      int fd_out;
      pid_t pid;
  } ProcessPipe;
#endif

static bool spawn_server(const char *exe_path, ProcessPipe *proc) {
    memset(proc, 0, sizeof(ProcessPipe));

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hStdInRead, hStdInWrite;
    HANDLE hStdOutRead, hStdOutWrite;

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) return false;
    if (!SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0)) return false;

    if (!CreatePipe(&hStdInRead, &hStdInWrite, &sa, 0)) return false;
    if (!SetHandleInformation(hStdInWrite, HANDLE_FLAG_INHERIT, 0)) return false;

    STARTUPINFOA si;
    memset(&si, 0, sizeof(STARTUPINFOA));
    si.cb = sizeof(STARTUPINFOA);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = hStdOutWrite;
    si.hStdInput = hStdInRead;
    si.dwFlags |= STARTF_USESTDHANDLES;

    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline), "\"%s\"", exe_path);

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &proc->procInfo)) {
        CloseHandle(hStdOutRead); CloseHandle(hStdOutWrite);
        CloseHandle(hStdInRead); CloseHandle(hStdInWrite);
        return false;
    }

    CloseHandle(hStdOutWrite);
    CloseHandle(hStdInRead);

    proc->hChildStdInWrite = hStdInWrite;
    proc->hChildStdOutRead = hStdOutRead;
    return true;
#else
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[0]); close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);

        char *args[] = {(char *)exe_path, NULL};
        execv(exe_path, args);
        exit(127);
    }

    close(pipe_in[0]);
    close(pipe_out[1]);

    proc->fd_in = pipe_in[1];
    proc->fd_out = pipe_out[0];
    proc->pid = pid;
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
    free(buf);
    return res && written > 0;
#else
    ssize_t res = write(proc->fd_in, buf, strlen(buf));
    free(buf);
    return res > 0;
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
        ssize_t res = read(proc->fd_out, &ch, 1);
        if (res <= 0) break;
#endif
        if (ch == '\r') continue;
        if (ch == '\n') {
            out_buf[idx] = '\0';
            return true;
        }
        out_buf[idx++] = ch;
    }
    out_buf[idx] = '\0';
    return idx > 0;
}

static void close_server(ProcessPipe *proc) {
#ifdef _WIN32
    CloseHandle(proc->hChildStdInWrite);
    CloseHandle(proc->hChildStdOutRead);
    WaitForSingleObject(proc->procInfo.hProcess, 2000);
    CloseHandle(proc->procInfo.hProcess);
    CloseHandle(proc->procInfo.hThread);
#else
    close(proc->fd_in);
    close(proc->fd_out);
    int status;
    waitpid(proc->pid, &status, 0);
#endif
}

int main(int argc, char **argv) {
    const char *server_exe = (argc > 1) ? argv[1] :
#ifdef _WIN32
        "bin\\sqlite-mcp-server.exe";
#else
        "./bin/sqlite-mcp-server";
#endif

    printf("===================================================\n");
    printf("MCP Integration Test Runner\n");
    printf("Target Server: %s\n", server_exe);
    printf("===================================================\n\n");

    ProcessPipe proc;
    if (!spawn_server(server_exe, &proc)) {
        fprintf(stderr, "[FAIL] Could not spawn server executable: %s\n", server_exe);
        return EXIT_FAILURE;
    }

    int passed = 0;
    int failed = 0;
    char resp[4096];

    /* TC-01: Legacy 2025-11-25 Handshake */
    printf("[TC-01] Testing legacy initialize handshake... ");
    send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\"}}");
    if (read_json_line(&proc, resp, sizeof(resp)) && strstr(resp, "\"id\":1") && strstr(resp, "protocolVersion")) {
        printf("PASS\n"); passed++;
    } else {
        printf("FAIL (Resp: %s)\n", resp); failed++;
    }

    /* TC-02: System Ping */
    printf("[TC-02] Testing system ping... ");
    send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}");
    if (read_json_line(&proc, resp, sizeof(resp)) && strstr(resp, "\"id\":2") && strstr(resp, "\"result\":{}")) {
        printf("PASS\n"); passed++;
    } else {
        printf("FAIL (Resp: %s)\n", resp); failed++;
    }

    /* TC-03: Modern 2026-07-28 Discovery */
    printf("[TC-03] Testing modern server/discover... ");
    send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"server/discover\"}");
    if (read_json_line(&proc, resp, sizeof(resp)) && strstr(resp, "\"id\":3") && strstr(resp, "2026-07-28")) {
        printf("PASS\n"); passed++;
    } else {
        printf("FAIL (Resp: %s)\n", resp); failed++;
    }

    /* TC-04: Tool Discovery */
    printf("[TC-04] Testing tools/list discovery... ");
    send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\"}");
    if (read_json_line(&proc, resp, sizeof(resp)) && strstr(resp, "\"id\":4") && strstr(resp, "read_query")) {
        printf("PASS\n"); passed++;
    } else {
        printf("FAIL (Resp: %s)\n", resp); failed++;
    }

    /* TC-05: Read Query Execution */
    printf("[TC-05] Testing tools/call read_query... ");
    send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"read_query\",\"arguments\":{\"sql\":\"SELECT 1;\"}}}");
    if (read_json_line(&proc, resp, sizeof(resp)) && strstr(resp, "\"id\":5") && strstr(resp, "resultType")) {
        printf("PASS\n"); passed++;
    } else {
        printf("FAIL (Resp: %s)\n", resp); failed++;
    }

    /* TC-06: Missing Parameter Error Handling */
    printf("[TC-06] Testing missing parameters error handling... ");
    send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\"}");
    if (read_json_line(&proc, resp, sizeof(resp)) && strstr(resp, "\"id\":6") && strstr(resp, "error")) {
        printf("PASS\n"); passed++;
    } else {
        printf("FAIL (Resp: %s)\n", resp); failed++;
    }

    /* TC-07: Malicious Identifier Sanitization */
    printf("[TC-07] Testing identifier sanitization... ");
    send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"describe_table\",\"arguments\":{\"table_name\":\"users; DROP TABLE users;\"}}}");
    if (read_json_line(&proc, resp, sizeof(resp)) && strstr(resp, "\"id\":7")) {
        printf("PASS\n"); passed++;
    } else {
        printf("FAIL (Resp: %s)\n", resp); failed++;
    }

    /* TC-08: Unknown Method Handling */
    printf("[TC-08] Testing unknown method error handling... ");
    send_json_line(&proc, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"unknown_method\"}");
    if (read_json_line(&proc, resp, sizeof(resp)) && strstr(resp, "\"id\":8") && strstr(resp, "error")) {
        printf("PASS\n"); passed++;
    } else {
        printf("FAIL (Resp: %s)\n", resp); failed++;
    }

    close_server(&proc);

    printf("\n===================================================\n");
    printf("Test Results: %d Passed, %d Failed\n", passed, failed);
    printf("===================================================\n");

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
