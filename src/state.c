#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "state.h"

static volatile int g_running = 1;
static SOCKET g_listen_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_state_lock;

void state_init(void) {
    InitializeCriticalSection(&g_state_lock);
    g_running = 1;
    g_listen_sock = INVALID_SOCKET;
}

void state_shutdown(void) {
    DeleteCriticalSection(&g_state_lock);
}

int state_is_running(void) {
    int v;
    EnterCriticalSection(&g_state_lock);
    v = g_running;
    LeaveCriticalSection(&g_state_lock);
    return v;
}

void state_set_running(int v) {
    EnterCriticalSection(&g_state_lock);
    g_running = v;
    LeaveCriticalSection(&g_state_lock);
}

SOCKET state_get_listen_sock(void) {
    SOCKET s;
    EnterCriticalSection(&g_state_lock);
    s = g_listen_sock;
    LeaveCriticalSection(&g_state_lock);
    return s;
}

void state_set_listen_sock(SOCKET s) {
    EnterCriticalSection(&g_state_lock);
    g_listen_sock = s;
    LeaveCriticalSection(&g_state_lock);
}
