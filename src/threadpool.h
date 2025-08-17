#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

void threadpool_init(int workers);
void threadpool_enqueue(SOCKET client, const struct sockaddr_in *addr);
void threadpool_shutdown(void);
