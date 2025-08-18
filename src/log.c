// logging implementation (thread-safe, rotation)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include "log.h"

static FILE *g_log_file = NULL;
static CRITICAL_SECTION g_log_lock;
static int g_log_inited = 0; /* guard whether g_log_lock was initialized */
static char g_log_path[MAX_PATH] = "";
static unsigned long long g_log_rotate_size = 0;
static int g_log_rotate_count = 0;
static unsigned long long g_log_bytes = 0ULL; /* cached current log file size */


static void rotate_logs_locked(void) {
    if (!g_log_path[0] || g_log_rotate_size == 0 || g_log_rotate_count <= 0) return;
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    char oldpath[MAX_PATH];
    char newpath[MAX_PATH];
    for (int i = g_log_rotate_count - 1; i >= 1; --i) {
        snprintf(oldpath, sizeof(oldpath), "%s.%d", g_log_path, i);
        snprintf(newpath, sizeof(newpath), "%s.%d", g_log_path, i+1);
        MoveFileExA(oldpath, newpath, MOVEFILE_REPLACE_EXISTING);
    }
    snprintf(newpath, sizeof(newpath), "%s.1", g_log_path);
    MoveFileExA(g_log_path, newpath, MOVEFILE_REPLACE_EXISTING);
    g_log_file = fopen(g_log_path, "a");
}

void log_init(const char *path, unsigned long long rotate_size, int rotate_count) {
    static int inited = 0;
    if (!inited) {
        InitializeCriticalSection(&g_log_lock);
        inited = 1;
        g_log_inited = 1;
    }
    if (path) {
        // if same path already open, keep it
        if (g_log_path[0] && strcmp(g_log_path, path) == 0 && g_log_file) {
            // nothing to do
        } else {
            strncpy(g_log_path, path, sizeof(g_log_path)-1);
            g_log_path[sizeof(g_log_path)-1] = '\0';
            if (g_log_file) { fclose(g_log_file); g_log_file = NULL; }
            g_log_file = fopen(g_log_path, "a");
            if (!g_log_file) {
                fprintf(stderr, "Failed to open log file '%s'\n", path);
            }
            /* Initialize cached size from existing file on disk to avoid stat on each write. */
            if (g_log_file) {
                struct _stat st;
                if (0 == _stat(g_log_path, &st)) g_log_bytes = (unsigned long long)st.st_size;
                else g_log_bytes = 0ULL;
            }
        }
    }
    g_log_rotate_size = rotate_size;
    g_log_rotate_count = rotate_count;
}

void log_printf(const char *level, const char *fmt, ...) {
    char timestr[64];
    time_t now = time(NULL);
    struct tm tm;
#if defined(_MSC_VER) || defined(__MINGW32__)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "[%s] %s: ", timestr, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    if (g_log_file) {
        EnterCriticalSection(&g_log_lock);
        /* Write pieces and measure bytes written to update cached size. */
        int wrote = 0;
        int a = fprintf(g_log_file, "[%s] %s: ", timestr, level);
        va_list ap2;
        va_start(ap2, fmt);
        int b = vfprintf(g_log_file, fmt, ap2);
        va_end(ap2);
        int c = fprintf(g_log_file, "\n");
        fflush(g_log_file);
        if (a > 0) wrote += a;
        if (b > 0) wrote += b;
        if (c > 0) wrote += c;
        if (wrote > 0) g_log_bytes += (unsigned long long)wrote;
        /* Rotate only when cached size exceeds threshold to avoid costly stats. */
        if (g_log_rotate_size > 0 && g_log_bytes >= g_log_rotate_size) {
            rotate_logs_locked();
            /* after rotation, reset cached size by stat of the reopened file */
            if (g_log_file) {
                struct _stat st;
                if (0 == _stat(g_log_path, &st)) g_log_bytes = (unsigned long long)st.st_size;
                else g_log_bytes = 0ULL;
            } else {
                g_log_bytes = 0ULL;
            }
        }
        LeaveCriticalSection(&g_log_lock);
    }
}

void log_shutdown(void) {
    if (g_log_file) {
        EnterCriticalSection(&g_log_lock);
        fclose(g_log_file);
        g_log_file = NULL;
        LeaveCriticalSection(&g_log_lock);
    }
    /* delete critical section only if it was initialized */
    if (g_log_inited) {
        DeleteCriticalSection(&g_log_lock);
        g_log_inited = 0;
    }
}
