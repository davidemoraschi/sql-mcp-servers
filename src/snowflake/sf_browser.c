/*
 * sf_browser.c - Open an https URL in the default browser
 *
 * Line budget constraint: < 450 lines
 */
#ifndef _WIN32
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
  #endif
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#include <string.h>
#include <ctype.h>
#include "sf_loopback.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <shellapi.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "shell32.lib")
  #endif
#else
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

/* Only https:// URLs without whitespace or control characters are opened. */
static bool sf_url_is_safe(const char *url) {
    const unsigned char *p;
    if (!url || strlen(url) < 9) return false;
    if (strncmp(url, "https://", 8) != 0) return false;
    for (p = (const unsigned char *)url; *p; p++) {
        if (*p <= 0x20 || *p == 0x7F) return false;
    }
    return true;
}

#ifdef _WIN32

bool sf_browser_open(const char *url) {
    HINSTANCE r;
    if (!sf_url_is_safe(url)) return false;
    r = ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

#else

/* fork/exec without a shell; the child's stdout/stderr go to /dev/null so
 * the opener can never write into the MCP JSON-RPC stream. */
static bool sf_try_opener(const char *program, const char *url) {
    int status = 0;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp(program, program, url, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool sf_browser_open(const char *url) {
    if (!sf_url_is_safe(url)) return false;
    return sf_try_opener("xdg-open", url) ||
           sf_try_opener("gnome-open", url) ||
           sf_try_opener("x-www-browser", url);
}

#endif
