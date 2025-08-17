#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include "http.h"
#include "log.h"
#include "utils.h"

// configurable serve directory (set by main via state)
static const char *g_serve_dir = "public";
void http_set_serve_dir(const char *d) { g_serve_dir = d ? d : "public"; }

static void url_decode_internal(char *dst, const char *src) {
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

void url_decode(char *dst, const char *src) { url_decode_internal(dst, src); }

const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
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

long handle_client(SOCKET client, const struct sockaddr_in *addr, int *out_status) {
    char buffer[8192];
    int received = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        closesocket(client);
        if (out_status) *out_status = 0;
        return -1;
    }
    buffer[received] = '\0';
    char method[16] = {0};
    char url[1024] = {0};
    sscanf(buffer, "%15s %1023s", method, url);
    char *q = strchr(url, '?'); if (q) *q = '\0';
    char decoded[1024]; url_decode(decoded, url);
    if (strcmp(decoded, "/") == 0) strcpy(decoded, "/index.html");
    if (strstr(decoded, "..") != NULL) {
        const char *forbidden = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        send(client, forbidden, (int)strlen(forbidden), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
        if (out_status) *out_status = 403;
        return 0;
    }
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", g_serve_dir, decoded);
    long total_sent = 0; int status = 200;
    char referer_buf[201] = "-"; char ua_buf[201] = "-";
    char *headers_end = strstr(buffer, "\r\n\r\n");
    if (headers_end) {
        char *line_start = strchr(buffer, '\n'); if (line_start) line_start += 1;
        while (line_start && line_start < headers_end) {
            char *line_end = strstr(line_start, "\r\n"); if (!line_end) break;
            size_t linelen = (size_t)(line_end - line_start);
            if (linelen > 0) {
                char line[1024]; size_t copylen = linelen < sizeof(line)-1 ? linelen : sizeof(line)-1;
                memcpy(line, line_start, copylen); line[copylen] = '\0';
                if (_strnicmp(line, "Referer:", 8) == 0) {
                    char *val = line + 8; while (*val == ' ') val++; sanitize_header(val, referer_buf, sizeof(referer_buf));
                } else if (_strnicmp(line, "User-Agent:", 11) == 0) {
                    char *val = line + 11; while (*val == ' ') val++; sanitize_header(val, ua_buf, sizeof(ua_buf));
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
                fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
                const char *mime = get_mime_type(filepath);
                char hdr[512]; int hdrlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
                    mime, fsize);
                send(client, hdr, hdrlen, 0); total_sent += hdrlen;
                char filebuf[8192]; size_t r;
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
    shutdown(client, SD_SEND); closesocket(client); if (out_status) *out_status = status;
    char ipstr[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &addr->sin_addr, ipstr, sizeof(ipstr));
    time_t now = time(NULL); struct tm local_tm, gm_tm;
#if defined(_MSC_VER) || defined(__MINGW32__)
    localtime_s(&local_tm, &now); gmtime_s(&gm_tm, &now);
#else
    localtime_r(&now, &local_tm); gmtime_r(&now, &gm_tm);
#endif
    char timestr[64]; strftime(timestr, sizeof(timestr), "%d/%b/%Y:%H:%M:%S", &local_tm);
    time_t lt = mktime(&local_tm); time_t gt = mktime(&gm_tm); long tzsec = (long)difftime(lt, gt);
    int tzh = (int)(tzsec / 3600); int tzm = (int)((abs(tzsec) % 3600) / 60);
    char tz[8]; snprintf(tz, sizeof(tz), "%+03d%02d", tzh, tzm);
    char request_line[512]; snprintf(request_line, sizeof(request_line), "%s %s", method, url);
    log_printf("ACCESS", "%s - - [%s %s] \"%s HTTP/1.1\" %d %ld \"%s\" \"%s\"", ipstr, timestr, tz, request_line, status, total_sent,
               referer_buf[0] ? referer_buf : "-", ua_buf[0] ? ua_buf : "-");
    return total_sent;
}
