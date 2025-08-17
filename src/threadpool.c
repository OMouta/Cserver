#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "threadpool.h"
#include "http.h"
#include "log.h"
#include "state.h"

typedef struct QueueNode {
    SOCKET client;
    struct sockaddr_in addr;
    struct QueueNode *next;
} QueueNode;

static QueueNode *g_queue_head = NULL;
static QueueNode *g_queue_tail = NULL;
static CRITICAL_SECTION g_queue_lock;
static CONDITION_VARIABLE g_queue_cond;

static HANDLE *g_thread_handles = NULL;
static size_t g_thread_handles_count = 0;
static size_t g_thread_handles_capacity = 0;
static CRITICAL_SECTION g_handles_lock;

// running state taken from state module

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
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_handles_lock);
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


static long handle_client_wrapper(SOCKET client, const struct sockaddr_in *addr) {
    int status = 0;
    long sent = handle_client(client, addr, &status);
    return sent;
}

static DWORD WINAPI worker_thread(LPVOID lpParam) {
    (void)lpParam;
    while (state_is_running()) {
        QueueNode *n = dequeue_node();
        if (!n) {
            SleepConditionVariableCS(&g_queue_cond, &g_queue_lock, 1000);
            continue;
        }
        (void)handle_client_wrapper(n->client, &n->addr);
        free(n);
    }
    return 0;
}

void threadpool_init(int workers) {
    InitializeCriticalSection(&g_handles_lock);
    InitializeCriticalSection(&g_queue_lock);
    InitializeConditionVariable(&g_queue_cond);
    for (int i = 0; i < workers; ++i) {
        HANDLE wh = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&worker_thread, NULL, 0, NULL);
        if (wh) add_thread_handle(wh);
    }
}

void threadpool_enqueue(SOCKET client, const struct sockaddr_in *addr) {
    QueueNode *n = (QueueNode*)malloc(sizeof(QueueNode));
    if (!n) { closesocket(client); return; }
    n->client = client; n->addr = *addr; n->next = NULL;
    EnterCriticalSection(&g_queue_lock);
    if (g_queue_tail) g_queue_tail->next = n; else g_queue_head = n;
    g_queue_tail = n;
    LeaveCriticalSection(&g_queue_lock);
    WakeConditionVariable(&g_queue_cond);
}

void threadpool_shutdown(void) {
    state_set_running(0);
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
        DWORD waitres = WaitForMultipleObjects((DWORD)count, wait_handles, TRUE, 10000);
        (void)waitres;
        for (size_t i = 0; i < count; ++i) CloseHandle(wait_handles[i]);
        free(wait_handles);
    }
    if (g_thread_handles) free(g_thread_handles);
    DeleteCriticalSection(&g_handles_lock);
    DeleteCriticalSection(&g_queue_lock);
}
