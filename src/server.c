// Main entry: uses modular http, threadpool, and logging modules
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "threadpool.h"
#include "http.h"
#include "state.h"
#include "utils.h"

#define DEFAULT_PORT "8080"
#define BACKLOG 10

// Console Ctrl handler for graceful shutdown
static BOOL WINAPI console_ctrl_handler(DWORD sig) {
    if (sig == CTRL_C_EVENT || sig == CTRL_CLOSE_EVENT || sig == CTRL_BREAK_EVENT) {
        log_printf("INFO", "Shutdown requested (signal=%u)", (unsigned)sig);
        state_set_running(0);
        {
            SOCKET s = state_get_listen_sock();
            if (s != INVALID_SOCKET) {
                closesocket(s);
                state_set_listen_sock(INVALID_SOCKET);
            }
        }
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv) {
    WSADATA wsaData;
    int rv = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (rv != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", rv);
        return 1;
    }

    // initialize shared state (CRITICAL_SECTION inside state.c)
    state_init();
    state_set_running(1);

    const char *port = DEFAULT_PORT;
    const char *serve_dir = "public";
    const char *log_file = NULL;
    const char *php_cgi = NULL;
    int workers = 8;
    const char *log_rotate_size_str = NULL;
    int log_rotate_count = 5;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--public") == 0 && i + 1 < argc) {
            serve_dir = argv[++i];
        } else if (strcmp(argv[i], "--serve") == 0 && i + 1 < argc) {
            serve_dir = argv[++i];
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            log_file = argv[++i];
        } else if (strcmp(argv[i], "--log-rotate-size") == 0 && i + 1 < argc) {
            log_rotate_size_str = argv[++i];
        } else if (strcmp(argv[i], "--log-rotate-count") == 0 && i + 1 < argc) {
            log_rotate_count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--php-cgi") == 0 && i + 1 < argc) {
            php_cgi = argv[++i];
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]); if (workers <= 0) workers = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--port PORT] [--serve PATH] [--workers N] [--log-file path] [--php-cgi \"path\"]\n", argv[0]);
            return 0;
        }
    }

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    unsigned long long rotate_size = 0;
    if (log_rotate_size_str) rotate_size = parse_size(log_rotate_size_str);
    log_init(log_file, rotate_size, log_rotate_count);
    http_set_serve_dir(serve_dir);
    if (php_cgi) http_set_php_cgi(php_cgi);
    threadpool_init(workers);

    struct addrinfo hints; struct addrinfo *result = NULL;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP; hints.ai_flags = AI_PASSIVE;
    rv = getaddrinfo(NULL, port, &hints, &result);
    if (rv != 0) {
        log_printf("ERROR", "getaddrinfo failed: %d", rv);
        WSACleanup();
        return 1;
    }

    SOCKET listen_sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listen_sock == INVALID_SOCKET) {
        log_printf("ERROR", "socket failed: %d", WSAGetLastError());
        freeaddrinfo(result); WSACleanup(); return 1;
    }
    BOOL opt = TRUE; setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    rv = bind(listen_sock, result->ai_addr, (int)result->ai_addrlen);
    if (rv == SOCKET_ERROR) { log_printf("ERROR", "bind failed: %d", WSAGetLastError()); closesocket(listen_sock); freeaddrinfo(result); WSACleanup(); return 1; }
    freeaddrinfo(result);
    rv = listen(listen_sock, BACKLOG);
    if (rv == SOCKET_ERROR) { log_printf("ERROR", "listen failed: %d", WSAGetLastError()); closesocket(listen_sock); WSACleanup(); return 1; }

    state_set_listen_sock(listen_sock);
    log_printf("INFO", "Listening on port %s, serving directory '%s', workers=%d", port, serve_dir, workers);

    while (state_is_running()) {
        struct sockaddr_in client_addr; int addrlen = sizeof(client_addr);
        SOCKET client = accept(listen_sock, (struct sockaddr*)&client_addr, &addrlen);
        if (client == INVALID_SOCKET) {
            int err = WSAGetLastError();
            /* When shutting down, closing the listen socket can cause accept to fail with
               WSAEINTR (10004) or WSAENOTSOCK; treat these as expected and stop looping. */
            if (!state_is_running() && (err == WSAENOTSOCK || err == WSAEINTR)) break;
            log_printf("WARN", "accept failed: %d", err);
            break;
        }
        threadpool_enqueue(client, &client_addr);
    }

    threadpool_shutdown();
    if (listen_sock != INVALID_SOCKET) closesocket(listen_sock);
    log_shutdown();
    state_shutdown();
    WSACleanup();
    return 0;
}
