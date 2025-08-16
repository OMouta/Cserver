// Minimal Win32 HTTP server (Winsock2 + Win32 threads)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

#define DEFAULT_PORT "8080"
#define BACKLOG 10

typedef struct {
    SOCKET sock;
} ClientParam;

// Globals for graceful shutdown and thread tracking
static volatile BOOL g_running = TRUE;
static SOCKET g_listen_sock = INVALID_SOCKET;

static HANDLE *g_thread_handles = NULL;
static size_t g_thread_handles_count = 0;
static size_t g_thread_handles_capacity = 0;
static CRITICAL_SECTION g_handles_lock;

// Logging to file
static FILE *g_log_file = NULL;
static CRITICAL_SECTION g_log_lock;
static char g_log_path[MAX_PATH] = "";
static unsigned long long g_log_rotate_size = 0; // bytes, 0 = disabled
static int g_log_rotate_count = 5;

// parse size strings like 10K, 5M
static unsigned long long parse_size(const char *s) {
    if (!s) return 0;
    char *endptr = NULL;
    unsigned long long v = strtoull(s, &endptr, 10);
    if (endptr && *endptr) {
        if (*endptr == 'K' || *endptr == 'k') v *= 1024ULL;
        else if (*endptr == 'M' || *endptr == 'm') v *= 1024ULL*1024ULL;
        else if (*endptr == 'G' || *endptr == 'g') v *= 1024ULL*1024ULL*1024ULL;
    }
    return v;
}

// rotate logs: called while holding g_log_lock
static void rotate_logs_locked(void) {
    if (!g_log_path[0] || g_log_rotate_size == 0 || g_log_rotate_count <= 0) return;
    // close current file
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    // shift files: logfile.(n-1) -> logfile.n
    char oldpath[MAX_PATH];
    char newpath[MAX_PATH];
    for (int i = g_log_rotate_count - 1; i >= 1; --i) {
        snprintf(oldpath, sizeof(oldpath), "%s.%d", g_log_path, i);
        snprintf(newpath, sizeof(newpath), "%s.%d", g_log_path, i+1);
        // ignore errors
        MoveFileExA(oldpath, newpath, MOVEFILE_REPLACE_EXISTING);
    }
    // move current log to .1
    snprintf(newpath, sizeof(newpath), "%s.1", g_log_path);
    MoveFileExA(g_log_path, newpath, MOVEFILE_REPLACE_EXISTING);

    // reopen log
    g_log_file = fopen(g_log_path, "a");
}

// Thread pool queue
typedef struct QueueNode {
    SOCKET client;
    struct sockaddr_in addr;
    struct QueueNode *next;
} QueueNode;

static QueueNode *g_queue_head = NULL;
static QueueNode *g_queue_tail = NULL;
static CRITICAL_SECTION g_queue_lock;
static CONDITION_VARIABLE g_queue_cond;

// forward declarations
static void url_decode(char *dst, const char *src);
static const char *get_mime_type(const char *path);
static void log_printf(const char *level, const char *fmt, ...);

// configurable public dir (set in main)
static const char *g_serve_dir = "public";

static void enqueue_client(SOCKET client, const struct sockaddr_in *addr) {
    QueueNode *n = (QueueNode*)malloc(sizeof(QueueNode));
    if (!n) { closesocket(client); return; }
    n->client = client; n->addr = *addr; n->next = NULL;
    EnterCriticalSection(&g_queue_lock);
    if (g_queue_tail) g_queue_tail->next = n; else g_queue_head = n;
    g_queue_tail = n;
    LeaveCriticalSection(&g_queue_lock);
    WakeConditionVariable(&g_queue_cond);
}

static QueueNode *dequeue_node(void) {
    QueueNode *n = NULL;
    EnterCriticalSection(&g_queue_lock);
    if (g_queue_head) {
        n = g_queue_head;
        g_queue_head = n->next;
        if (!g_queue_head) g_queue_tail = NULL;
    }
    LeaveCriticalSection(&g_queue_lock);
    return n;
}

// Core client handling (extracted from previous client_thread)
// handle_client returns bytes_sent (>=0) or negative on error; status is set in out_status
static long handle_client(SOCKET client, const struct sockaddr_in *addr, int *out_status) {
    char buffer[8192];
    int received = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
    closesocket(client);
    if (out_status) *out_status = 0;
    return -1;
    }
    buffer[received] = '\0';

    // Simple request line parse
    char method[16] = {0};
    char url[1024] = {0};
    sscanf(buffer, "%15s %1023s", method, url);

    // Strip query string
    char *q = strchr(url, '?');
    if (q) *q = '\0';

    // Decode URL and map to local file under ./public
    char decoded[1024];
    url_decode(decoded, url);

    // Default to / -> /index.html
    if (strcmp(decoded, "/") == 0) {
        strcpy(decoded, "/index.html");
    }

    // Basic security: reject paths containing ".."
    if (strstr(decoded, "..") != NULL) {
        const char *forbidden = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        send(client, forbidden, (int)strlen(forbidden), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
        if (out_status) *out_status = 403;
        return 0;
    }

    // Build file path: <public_dir> + decoded
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", g_serve_dir, decoded);

    long total_sent = 0;
    int status = 200;
    // Parse headers for Referer and User-Agent safely
    char referer_buf[201] = "-";
    char ua_buf[201] = "-";
    char *headers_end = strstr(buffer, "\r\n\r\n");
    if (headers_end) {
        char *line_start = strchr(buffer, '\n'); // first CRLF after request line
        if (line_start) line_start += 1; // move to start of headers (after LF)
        while (line_start && line_start < headers_end) {
            char *line_end = strstr(line_start, "\r\n");
            if (!line_end) break;
            size_t linelen = (size_t)(line_end - line_start);
            if (linelen > 0) {
                // copy into temp buffer
                char line[1024];
                size_t copylen = linelen < sizeof(line)-1 ? linelen : sizeof(line)-1;
                memcpy(line, line_start, copylen);
                line[copylen] = '\0';
                // header name search
                if (_strnicmp(line, "Referer:", 8) == 0) {
                    char *val = line + 8; while (*val == ' ') val++;
                    strncpy(referer_buf, val, 200); referer_buf[200] = '\0';
                } else if (_strnicmp(line, "User-Agent:", 11) == 0) {
                    char *val = line + 11; while (*val == ' ') val++;
                    strncpy(ua_buf, val, 200); ua_buf[200] = '\0';
                }
            }
            line_start = line_end + 2;
        }
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(decoded, "/hello") == 0) {
            const char *body = "<html><body><h1>/hello says hi!</h1></body></html>";
            char hdr[512];
            int hdrlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                strlen(body));
            send(client, hdr, hdrlen, 0); total_sent += hdrlen;
            send(client, body, (int)strlen(body), 0); total_sent += (int)strlen(body);
            status = 200;
        } else {
            FILE *f = fopen(filepath, "rb");
            if (!f) {
                const char *notfound = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                send(client, notfound, (int)strlen(notfound), 0);
                total_sent += (int)strlen(notfound);
                status = 404;
            } else {
                fseek(f, 0, SEEK_END);
                long fsize = ftell(f);
                fseek(f, 0, SEEK_SET);

                const char *mime = get_mime_type(filepath);
                char hdr[512];
                int hdrlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
                    mime, fsize);
                send(client, hdr, hdrlen, 0); total_sent += hdrlen;

                char filebuf[8192];
                size_t r;
                while ((r = fread(filebuf, 1, sizeof(filebuf), f)) > 0) {
                    int sent = send(client, filebuf, (int)r, 0);
                    if (sent == SOCKET_ERROR) break;
                    total_sent += sent;
                }
                fclose(f);
                status = 200;
            }
        }
    } else {
        const char *notimpl = "HTTP/1.1 501 Not Implemented\r\nConnection: close\r\n\r\n";
        send(client, notimpl, (int)strlen(notimpl), 0);
        total_sent += (int)strlen(notimpl);
        status = 501;
    }

    shutdown(client, SD_SEND);
    closesocket(client);
    if (out_status) *out_status = status;

    // Access log in Combined Log Format: %h %l %u [%t] "%r" %>s %b "%{Referer}i" "%{User-agent}i"
    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ipstr, sizeof(ipstr));
    // time in format: 10/Oct/2000:13:55:36 -0700
    time_t now = time(NULL);
    struct tm local_tm, gm_tm;
#if defined(_MSC_VER) || defined(__MINGW32__)
    localtime_s(&local_tm, &now);
    gmtime_s(&gm_tm, &now);
#else
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &gm_tm);
#endif
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%d/%b/%Y:%H:%M:%S", &local_tm);
    // timezone offset
    time_t lt = mktime(&local_tm);
    time_t gt = mktime(&gm_tm);
    long tzsec = (long)difftime(lt, gt);
    int tzh = (int)(tzsec / 3600);
    int tzm = (int)((abs(tzsec) % 3600) / 60);
    char tz[8];
    snprintf(tz, sizeof(tz), "%+03d%02d", tzh, tzm);
    char request_line[512];
    snprintf(request_line, sizeof(request_line), "%s %s", method, url);
    log_printf("ACCESS", "%s - - [%s %s] \"%s HTTP/1.1\" %d %ld \"%s\" \"%s\"", ipstr, timestr, tz, request_line, status, total_sent,
               referer_buf[0] ? referer_buf : "-", ua_buf[0] ? ua_buf : "-");
    return total_sent;
}

// Worker thread for pool
static DWORD WINAPI worker_thread(LPVOID lpParam) {
    (void)lpParam;
    while (g_running) {
        QueueNode *n = dequeue_node();
        if (!n) {
            SleepConditionVariableCS(&g_queue_cond, &g_queue_lock, 1000);
            continue;
        }
    int status = 0;
    (void)handle_client(n->client, &n->addr, &status);
    free(n);
    }
    return 0;
}

static void add_thread_handle(HANDLE h) {
    EnterCriticalSection(&g_handles_lock);
    if (g_thread_handles_count == g_thread_handles_capacity) {
        size_t newcap = g_thread_handles_capacity ? g_thread_handles_capacity * 2 : 64;
        HANDLE *b = (HANDLE*)realloc(g_thread_handles, newcap * sizeof(HANDLE));
        if (b) {
            g_thread_handles = b;
            g_thread_handles_capacity = newcap;
        }
    }
    if (g_thread_handles_count < g_thread_handles_capacity) {
        g_thread_handles[g_thread_handles_count++] = h;
    } else {
        // fallback: close handle if we can't store it
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_handles_lock);
}

// Simple logging
static void log_printf(const char *level, const char *fmt, ...) {
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
    // also write to file if configured
    if (g_log_file) {
        EnterCriticalSection(&g_log_lock);
        // check size
        if (g_log_rotate_size > 0) {
            // get current file size
            struct _stat st;
            if (0 == _stat(g_log_path, &st)) {
                if ((unsigned long long)st.st_size >= g_log_rotate_size) {
                    rotate_logs_locked();
                }
            }
        }
        fprintf(g_log_file, "[%s] %s: ", timestr, level);
        va_list ap2;
        va_start(ap2, fmt);
        vfprintf(g_log_file, fmt, ap2);
        va_end(ap2);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
        LeaveCriticalSection(&g_log_lock);
    }
}

// Console Ctrl handler for graceful shutdown
static BOOL WINAPI console_ctrl_handler(DWORD sig) {
    if (sig == CTRL_C_EVENT || sig == CTRL_CLOSE_EVENT || sig == CTRL_BREAK_EVENT) {
        log_printf("INFO", "Shutdown requested (signal=%u)", (unsigned)sig);
        g_running = FALSE;
        if (g_listen_sock != INVALID_SOCKET) {
            closesocket(g_listen_sock);
            g_listen_sock = INVALID_SOCKET;
        }
        return TRUE;
    }
    return FALSE;
}

// Simple percent-decoding for URL paths. Decodes in place.
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && isxdigit(a) && isxdigit(b)) {
            char hex[3] = {a, b, '\0'};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Very small MIME map (extend as needed)
static const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++; // skip dot
    if (_stricmp(ext, "html") == 0 || _stricmp(ext, "htm") == 0) return "text/html; charset=UTF-8";
    if (_stricmp(ext, "css") == 0) return "text/css";
    if (_stricmp(ext, "js") == 0) return "application/javascript";
    if (_stricmp(ext, "json") == 0) return "application/json";
    if (_stricmp(ext, "png") == 0) return "image/png";
    if (_stricmp(ext, "jpg") == 0 || _stricmp(ext, "jpeg") == 0) return "image/jpeg";
    if (_stricmp(ext, "gif") == 0) return "image/gif";
    if (_stricmp(ext, "svg") == 0) return "image/svg+xml";
    if (_stricmp(ext, "txt") == 0) return "text/plain; charset=UTF-8";
    if (_stricmp(ext, "wasm") == 0) return "application/wasm";
    return "application/octet-stream";
}

DWORD WINAPI client_thread(LPVOID lpParam) {
    ClientParam *p = (ClientParam*)lpParam;
    SOCKET client = p->sock;
    free(p);

    char buffer[8192];
    int received = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        closesocket(client);
        return 0;
    }
    buffer[received] = '\0';

    // Simple request line parse
    char method[16] = {0};
    char url[1024] = {0};
    sscanf(buffer, "%15s %1023s", method, url);

    // Strip query string
    char *q = strchr(url, '?');
    if (q) *q = '\0';

    // Decode URL and map to local file under ./public
    char decoded[1024];
    url_decode(decoded, url);

    // Default to / -> /index.html
    if (strcmp(decoded, "/") == 0) {
        strcpy(decoded, "/index.html");
    }

    // Basic security: reject paths containing ".."
    if (strstr(decoded, "..") != NULL) {
        const char *forbidden = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        send(client, forbidden, (int)strlen(forbidden), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
        return 0;
    }

    // Build file path: ./public + decoded
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "public%s", decoded);

    if (strcmp(method, "GET") == 0) {
        // Special-case small route
        if (strcmp(decoded, "/hello") == 0) {
            const char *body = "<html><body><h1>/hello says hi!</h1></body></html>";
            char hdr[512];
            int hdrlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                strlen(body));
            send(client, hdr, hdrlen, 0);
            send(client, body, (int)strlen(body), 0);
        } else {
            // Try to open the file
            FILE *f = fopen(filepath, "rb");
            if (!f) {
                const char *notfound = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                send(client, notfound, (int)strlen(notfound), 0);
            } else {
                // determine file size
                fseek(f, 0, SEEK_END);
                long fsize = ftell(f);
                fseek(f, 0, SEEK_SET);

                const char *mime = get_mime_type(filepath);
                char hdr[512];
                int hdrlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
                    mime, fsize);
                send(client, hdr, hdrlen, 0);

                // stream file in chunks
                char filebuf[8192];
                size_t r;
                while ((r = fread(filebuf, 1, sizeof(filebuf), f)) > 0) {
                    int sent = send(client, filebuf, (int)r, 0);
                    if (sent == SOCKET_ERROR) break;
                }
                fclose(f);
            }
        }
    } else {
        const char *notimpl = "HTTP/1.1 501 Not Implemented\r\nConnection: close\r\n\r\n";
        send(client, notimpl, (int)strlen(notimpl), 0);
    }

    // graceful close
    shutdown(client, SD_SEND);
    closesocket(client);
    return 0;
}

int main(int argc, char **argv) {
    WSADATA wsaData;
    int rv = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (rv != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", rv);
        return 1;
    }

    // default options
    const char *port = DEFAULT_PORT;
    const char *serve_dir = "public";
    const char *log_file = NULL;
    int workers = 8; // default worker count
    const char *log_rotate_size_str = NULL;
    int log_rotate_count = 5;

    // parse simple args
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--public") == 0 && i + 1 < argc) {
            // Keep --public as an alias
            serve_dir = argv[++i];
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            log_file = argv[++i];
        } else if (strcmp(argv[i], "--log-rotate-size") == 0 && i + 1 < argc) {
            log_rotate_size_str = argv[++i];
        } else if (strcmp(argv[i], "--log-rotate-count") == 0 && i + 1 < argc) {
            log_rotate_count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
            if (workers <= 0) workers = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--port PORT] [--public PATH] [--workers N] [--log-file path]\n", argv[0]);
            return 0;
        }
    }

    InitializeCriticalSection(&g_handles_lock);
    InitializeCriticalSection(&g_queue_lock);
    InitializeConditionVariable(&g_queue_cond);
    InitializeCriticalSection(&g_log_lock);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    if (log_file) {
        strncpy(g_log_path, log_file, sizeof(g_log_path)-1);
        g_log_path[sizeof(g_log_path)-1] = '\0';
        g_log_file = fopen(log_file, "a");
        if (!g_log_file) {
            fprintf(stderr, "Failed to open log file '%s'\n", log_file);
        }
    }
    if (log_rotate_size_str) {
        g_log_rotate_size = parse_size(log_rotate_size_str);
        g_log_rotate_count = log_rotate_count > 0 ? log_rotate_count : 5;
    }

    struct addrinfo hints;
    struct addrinfo *result = NULL;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    rv = getaddrinfo(NULL, port, &hints, &result);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo failed: %d\n", rv);
        WSACleanup();
        return 1;
    }

    SOCKET listen_sock = INVALID_SOCKET;
    listen_sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        freeaddrinfo(result);
        WSACleanup();
        return 1;
    }

    // Allow quick restart
    BOOL opt = TRUE;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    rv = bind(listen_sock, result->ai_addr, (int)result->ai_addrlen);
    if (rv == SOCKET_ERROR) {
        fprintf(stderr, "bind failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        freeaddrinfo(result);
        WSACleanup();
        return 1;
    }

    freeaddrinfo(result);

    rv = listen(listen_sock, BACKLOG);
    if (rv == SOCKET_ERROR) {
        fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    g_listen_sock = listen_sock;
    // store global serve dir
    g_serve_dir = serve_dir;
    log_printf("INFO", "Listening on port %s, serving directory '%s', workers=%d", port, serve_dir, workers);

    // spawn worker threads
    for (int i = 0; i < workers; ++i) {
        HANDLE wh = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) &worker_thread, NULL, 0, NULL);
        if (wh) add_thread_handle(wh);
    }

    while (g_running) {
        struct sockaddr_in client_addr;
        int addrlen = sizeof(client_addr);
        SOCKET client = accept(listen_sock, (struct sockaddr*)&client_addr, &addrlen);
        if (client == INVALID_SOCKET) {
            int err = WSAGetLastError();
            if (!g_running && err == WSAENOTSOCK) break; // expected during shutdown
            log_printf("WARN", "accept failed: %d", err);
            break;
        }

    // enqueue client for worker threads
    enqueue_client(client, &client_addr);
    }

    // Shutdown: wake workers and wait for worker threads to finish
    WakeAllConditionVariable(&g_queue_cond);
    EnterCriticalSection(&g_handles_lock);
    size_t count = g_thread_handles_count;
    HANDLE *wait_handles = NULL;
    if (count) {
        wait_handles = (HANDLE*)malloc(count * sizeof(HANDLE));
        if (wait_handles) memcpy(wait_handles, g_thread_handles, count * sizeof(HANDLE));
    }
    LeaveCriticalSection(&g_handles_lock);

    if (wait_handles) {
        // wait with timeout in case threads never exit (10s)
        DWORD waitres = WaitForMultipleObjects((DWORD)count, wait_handles, TRUE, 10000);
        (void)waitres;
        for (size_t i = 0; i < count; ++i) CloseHandle(wait_handles[i]);
        free(wait_handles);
    }

    if (listen_sock != INVALID_SOCKET) {
        closesocket(listen_sock);
    }
    WSACleanup();
    DeleteCriticalSection(&g_handles_lock);
    return 0;
}
