#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

// handle a single client; returns bytes sent or negative on error; out_status is filled with HTTP status code
long handle_client(SOCKET client, const struct sockaddr_in *addr, int *out_status);

// utils
void url_decode(char *dst, const char *src);
const char *get_mime_type(const char *path);

// set serve directory
void http_set_serve_dir(const char *d);
