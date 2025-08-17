#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

// runtime state shared across modules
void state_init(void);
void state_shutdown(void);
int state_is_running(void);
void state_set_running(int v);
SOCKET state_get_listen_sock(void);
void state_set_listen_sock(SOCKET s);
