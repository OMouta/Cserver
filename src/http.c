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
#include "cgi.h"

// configurable serve directory (set by main via state)
static const char *g_serve_dir = "public";
void http_set_serve_dir(const char *d) { g_serve_dir = d ? d : "public"; }

/* Configurable php-cgi command; default to "php-cgi" which is the typical CGI wrapper for PHP.
    The earlier code used "php -f" which invokes the CLI and doesn't behave exactly like php-cgi.
    Allow overriding via server CLI. */
static const char *g_php_cgi_cmd = "php-cgi";
void http_set_php_cgi(const char *path) { g_php_cgi_cmd = path ? path : "php-cgi"; }

static void url_decode_internal(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && isxdigit((unsigned char)a) && isxdigit((unsigned char)b)) {
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

/* Bounded URL decode: returns 0 on success, 1 if output would overflow. */
static int url_decode_n(char *dst, size_t dstlen, const char *src) {
    size_t wi = 0;
    while (*src) {
        if ((*src == '%') && src[1] && src[2] && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            if (wi + 1 >= dstlen) return 1;
            char hex[3] = { (char)src[1], (char)src[2], '\0' };
            dst[wi++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            if (wi + 1 >= dstlen) return 1;
            dst[wi++] = ' ';
            src++;
        } else {
            if (wi + 1 >= dstlen) return 1;
            dst[wi++] = *src++;
        }
    }
    if (wi >= dstlen) return 1;
    dst[wi] = '\0';
    return 0;
}

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
    int received = 0;
    int total = 0;
    /* Set a receive timeout so we don't block indefinitely waiting for a client. */
    {
        DWORD timeout_ms = 5000; /* 5 seconds */
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    }

    /* Read until end of headers (\r\n\r\n) or buffer full. */
    while (total < (int)sizeof(buffer) - 1) {
        received = recv(client, buffer + total, (int)sizeof(buffer) - 1 - total, 0);
        if (received <= 0) {
            closesocket(client);
            if (out_status) *out_status = 0;
            return -1;
        }
        total += received;
        buffer[total] = '\0';
        if (strstr(buffer, "\r\n\r\n") != NULL) break;
        /* continue reading until headers end or buffer limit */
    }
    /* If we exited the loop without finding headers terminator, treat as too large */
    if (strstr(buffer, "\r\n\r\n") == NULL) {
        const char *too_large = "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n";
        send(client, too_large, (int)strlen(too_large), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
        if (out_status) *out_status = 431;
        return 0;
    }
    /* Optionally clear timeout (socket will be closed later) */
    char method[16] = {0};
    char url[1024] = {0};
    char original_url[1024] = {0};
    sscanf(buffer, "%15s %1023s", method, url);
    snprintf(original_url, sizeof(original_url), "%s", url);
    /* separate query string from path so we can decode the path but still pass QUERY_STRING to CGI */
    char query_string[1024] = "";
    char path_only[1024]; snprintf(path_only, sizeof(path_only), "%s", url);
    char *q = strchr(path_only, '?');
    if (q) {
        size_t qlen = strlen(q+1);
        if (qlen >= sizeof(query_string)) qlen = sizeof(query_string)-1;
        memcpy(query_string, q+1, qlen); query_string[qlen] = '\0';
        *q = '\0';
    }
    /* Safe URL decode into bounded buffer */
    char decoded[1024];
    if (url_decode_n(decoded, sizeof(decoded), path_only) != 0) {
        const char *too_long = "HTTP/1.1 414 Request-URI Too Long\r\nConnection: close\r\n\r\n";
        send(client, too_long, (int)strlen(too_long), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
        if (out_status) *out_status = 414;
        return 0;
    }
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
    /* build the raw path then canonicalize it */
    char pathbuf[MAX_PATH];
    snprintf(pathbuf, sizeof(pathbuf), "%s%s", g_serve_dir, decoded);
    char full_serve[MAX_PATH]; char fullpath[MAX_PATH];
    if (!_fullpath(full_serve, g_serve_dir, sizeof(full_serve))) {
        /* fallback: use g_serve_dir as-is */
        strncpy(full_serve, g_serve_dir, sizeof(full_serve)-1); full_serve[sizeof(full_serve)-1] = '\0';
    }
    /* ensure trailing backslash for prefix check */
    {
        size_t sl = strlen(full_serve);
        if (sl > 0 && full_serve[sl-1] != '\\' && full_serve[sl-1] != '/') {
            if (sl + 1 < sizeof(full_serve)) { full_serve[sl] = '\\'; full_serve[sl+1] = '\0'; }
        }
    }
    if (!_fullpath(fullpath, pathbuf, sizeof(fullpath))) {
        const char *err500 = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
        send(client, err500, (int)strlen(err500), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
        if (out_status) *out_status = 500;
        return 0;
    }
    /* enforce that fullpath is under full_serve directory */
    if (strncmp(fullpath, full_serve, strlen(full_serve)) != 0) {
        const char *forbidden = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        send(client, forbidden, (int)strlen(forbidden), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
        if (out_status) *out_status = 403;
        return 0;
    }

    /* use the canonicalized path for file operations */
    strncpy(filepath, fullpath, sizeof(filepath)-1); filepath[sizeof(filepath)-1] = '\0';
    /* If the path is a directory, try to resolve an index file (index.php then index.html). */
    {
        DWORD attrs = GetFileAttributesA(filepath);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            char candidate[MAX_PATH];
            /* try index.php */
            snprintf(candidate, sizeof(candidate), "%s\\index.php", filepath);
            if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
                /* use index.php */
                strncpy(filepath, candidate, sizeof(filepath)-1); filepath[sizeof(filepath)-1] = '\0';
                /* adjust decoded (SCRIPT_NAME/PHP_SELF) to include index.php */
                size_t dl = strlen(decoded);
                if (dl + 10 < sizeof(decoded)) {
                    if (dl > 0 && decoded[dl-1] == '/') strncat(decoded, "index.php", sizeof(decoded)-dl-1);
                    else strncat(decoded, "/index.php", sizeof(decoded)-dl-1);
                }
            } else {
                /* try index.html */
                snprintf(candidate, sizeof(candidate), "%s\\index.html", filepath);
                if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
                    strncpy(filepath, candidate, sizeof(filepath)-1); filepath[sizeof(filepath)-1] = '\0';
                    size_t dl = strlen(decoded);
                    if (dl + 11 < sizeof(decoded)) {
                        if (dl > 0 && decoded[dl-1] == '/') strncat(decoded, "index.html", sizeof(decoded)-dl-1);
                        else strncat(decoded, "/index.html", sizeof(decoded)-dl-1);
                    }
                }
            }
        }
    }
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
    /* accept GET, POST and HEAD for existing handling (POST forwarded to CGI for dynamic files) */
    if (_stricmp(method, "GET") == 0 || _stricmp(method, "POST") == 0 || _stricmp(method, "HEAD") == 0) {
        if (strcmp(decoded, "/hello") == 0) {
            const char *body = "<html><body><h1>/hello says hi!</h1></body></html>";
            char hdr[512];
            int hdrlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                strlen(body));
            send(client, hdr, hdrlen, 0); total_sent += hdrlen;
            /* don't send body for HEAD requests */
            if (_stricmp(method, "HEAD") != 0) {
                send(client, body, (int)strlen(body), 0); total_sent += (int)strlen(body);
            }
            status = 200;
        } else {
            /* If the requested file ends with a dynamic extension, run the associated interpreter via CGI. */
            const char *ext = strrchr(filepath, '.');
            if (ext) {
                ext++;
                /* map a few common script extensions to interpreter commands. In production make this configurable. */
                const char *interp = NULL;
                if (_stricmp(ext, "php") == 0) interp = g_php_cgi_cmd; /* default php-cgi or overridden via --php-cgi */
                else if (_stricmp(ext, "py") == 0) interp = "python"; /* python script */
                else if (_stricmp(ext, "pl") == 0) interp = "perl";
                else if (_stricmp(ext, "rb") == 0) interp = "ruby";
                if (interp) {
                    /* Build CGI environment: REQUEST_METHOD, SCRIPT_FILENAME, QUERY_STRING, SERVER_PROTOCOL,
                       CONTENT_LENGTH (if present), CONTENT_TYPE (if present), REMOTE_ADDR and HTTP_* headers. */
                    char env_request_method[64];
                    char env_script[MAX_PATH + 32];
                    char env_query[1024] = "QUERY_STRING=";
                    char env_proto[64];
                    char env_cl[64] = "";
                    char env_ct[256] = "";
                    char env_remote[128] = "";
                    snprintf(env_request_method, sizeof(env_request_method), "REQUEST_METHOD=%s", method);
                    snprintf(env_script, sizeof(env_script), "SCRIPT_FILENAME=%s", filepath);
                    /* use extracted query string from earlier */
                    if (query_string[0]) {
                        snprintf(env_query + strlen("QUERY_STRING="), sizeof(env_query) - strlen("QUERY_STRING="), "%s", query_string);
                    }
                    snprintf(env_proto, sizeof(env_proto), "SERVER_PROTOCOL=HTTP/1.1");
                    /* CONTENT_LENGTH and CONTENT_TYPE could be parsed from headers; simple scan below */
                    char content_length_val[64] = "";
                    char content_type_val[192] = "";
                    if (headers_end) {
                        char *ls = strchr(buffer, '\n'); if (ls) ls += 1;
                        while (ls && ls < headers_end) {
                            char *le = strstr(ls, "\r\n"); if (!le) break;
                            size_t llen = (size_t)(le - ls);
                            if (llen > 0 && llen < 1024) {
                                char line[1024]; size_t copylen = llen < sizeof(line)-1 ? llen : sizeof(line)-1;
                                memcpy(line, ls, copylen); line[copylen] = '\0';
                                if (_strnicmp(line, "Content-Length:", 15) == 0) {
                                    char *v = line + 15; while (*v == ' ') v++; snprintf(content_length_val, sizeof(content_length_val), "%s", v);
                                } else if (_strnicmp(line, "Content-Type:", 13) == 0) {
                                    char *v = line + 13; while (*v == ' ') v++; snprintf(content_type_val, sizeof(content_type_val), "%s", v);
                                }
                            }
                            ls = le + 2;
                        }
                    }
                    if (content_length_val[0]) snprintf(env_cl, sizeof(env_cl), "CONTENT_LENGTH=%s", content_length_val);
                    if (content_type_val[0]) snprintf(env_ct, sizeof(env_ct), "CONTENT_TYPE=%s", content_type_val);
                    /* REMOTE_ADDR */
                    char ipstr[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &addr->sin_addr, ipstr, sizeof(ipstr));
                    snprintf(env_remote, sizeof(env_remote), "REMOTE_ADDR=%s", ipstr);

                    /* Additional CGI vars */
                    char env_gateway[] = "GATEWAY_INTERFACE=CGI/1.1";
                    char env_software[] = "SERVER_SOFTWARE=Cserver/0.1";
                    char env_script_name[MAX_PATH + 32];
                    char env_path_translated[MAX_PATH + 32];
                    char env_php_self[1024];
                    char env_redirect[] = "REDIRECT_STATUS=200"; /* helpful for php-cgi */
                    snprintf(env_script_name, sizeof(env_script_name), "SCRIPT_NAME=%s", decoded);
                    snprintf(env_path_translated, sizeof(env_path_translated), "PATH_TRANSLATED=%s", filepath);
                    snprintf(env_php_self, sizeof(env_php_self), "PHP_SELF=%s", decoded);
                    /* SERVER_NAME/PORT: assume localhost:8080 unless the server provides these elsewhere */
                    char env_server_name[] = "SERVER_NAME=localhost";
                    char env_server_port[] = "SERVER_PORT=8080";
                    char env_remote_port[64]; snprintf(env_remote_port, sizeof(env_remote_port), "REMOTE_PORT=%u", ntohs(addr->sin_port));

                    /* Build an env vector, including HTTP_* headers. Limit to a fixed number to keep stack usage bounded. */
                    const int MAX_ENV = 64;
                    const int MAX_ENV_STORAGE = 64;
                    char env_storage[MAX_ENV_STORAGE][512];
                    const char *envp[MAX_ENV];
                    int ei = 0;
                    envp[ei++] = env_request_method;
                    envp[ei++] = env_script;
                    envp[ei++] = env_query;
                    envp[ei++] = env_proto;
                    if (env_cl[0]) envp[ei++] = env_cl;
                    if (env_ct[0]) envp[ei++] = env_ct;
                    envp[ei++] = env_remote;
                    envp[ei++] = env_gateway;
                    envp[ei++] = env_software;
                    envp[ei++] = env_script_name;
                    envp[ei++] = env_path_translated;
                    envp[ei++] = env_php_self;
                    envp[ei++] = env_redirect;
                    envp[ei++] = env_server_name;
                    envp[ei++] = env_server_port;
                    envp[ei++] = env_remote_port;

                    /* Scan headers and append as HTTP_<NAME>=value entries. */
                    if (headers_end) {
                        char *ls = strchr(buffer, '\n'); if (ls) ls += 1;
                        while (ls && ls < headers_end && ei < MAX_ENV-1) {
                            char *le = strstr(ls, "\r\n"); if (!le) break;
                            size_t llen = (size_t)(le - ls);
                            if (llen > 0 && llen < 480) {
                                char line[512]; size_t copylen = llen < sizeof(line)-1 ? llen : sizeof(line)-1;
                                memcpy(line, ls, copylen); line[copylen] = '\0';
                                /* split name:value */
                                char *colon = strchr(line, ':');
                                if (colon) {
                                    *colon = '\0';
                                    char *name = line; char *val = colon + 1;
                                    while (*val == ' ') val++;
                                    /* normalize name: uppercase and dashes -> underscores */
                                    char nm[256]; size_t ni = 0;
                                    for (char *p = name; *p && ni + 1 < sizeof(nm); ++p) {
                                        char c = *p;
                                        if (c == '-') c = '_';
                                        nm[ni++] = (char)toupper((unsigned char)c);
                                    }
                                    nm[ni] = '\0';
                                    /* Skip Content-Type and Content-Length since they're added separately */
                                    if (_stricmp(nm, "CONTENT_TYPE") == 0 || _stricmp(nm, "CONTENT_LENGTH") == 0) {
                                        /* skip */
                                    } else {
                                        /* prefix HTTP_ */
                                        snprintf(env_storage[ei % MAX_ENV_STORAGE], sizeof(env_storage[0]), "HTTP_%s=%s", nm, val);
                                        envp[ei] = env_storage[ei % MAX_ENV_STORAGE];
                                        ei++;
                                    }
                                }
                            }
                            ls = le + 2;
                        }
                    }
                    envp[ei] = NULL;

                    /* Read request body (if any) and pass to CGI. Enforce a cap to avoid OOMs. */
                    size_t content_len = 0;
                    if (content_length_val[0]) content_len = (size_t)strtoul(content_length_val, NULL, 10);
                    const size_t MAX_BODY = 4 * 1024 * 1024; /* 4 MiB */
                    if (content_len > MAX_BODY) {
                        const char *too_big = "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\n\r\n";
                        send(client, too_big, (int)strlen(too_big), 0);
                        shutdown(client, SD_SEND); closesocket(client);
                        if (out_status) *out_status = 413;
                        return 0;
                    }

                    char *body_buf = NULL; size_t body_read = 0;
                    if (content_len > 0) {
                        body_buf = (char*)malloc(content_len);
                        if (!body_buf) {
                            const char *err500 = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
                            send(client, err500, (int)strlen(err500), 0);
                            shutdown(client, SD_SEND); closesocket(client);
                            if (out_status) *out_status = 500;
                            return 0;
                        }
                        /* compute how many body bytes are already in buffer after headers_end */
                        char *body_start = headers_end + 4; /* skip \r\n\r\n */
                        int already = (int)(total - (body_start - buffer));
                        if (already > 0) {
                            int copy = (int)min((size_t)already, content_len);
                            memcpy(body_buf, body_start, copy);
                            body_read = copy;
                        }
                        /* read remaining bytes */
                        while (body_read < content_len) {
                            int toread = (int)min(content_len - body_read, (size_t)4096);
                            int r = recv(client, body_buf + body_read, toread, 0);
                            if (r <= 0) break; /* timeout or error */
                            body_read += (size_t)r;
                        }
                        if (body_read != content_len) {
                            free(body_buf);
                            const char *badreq = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
                            send(client, badreq, (int)strlen(badreq), 0);
                            shutdown(client, SD_SEND); closesocket(client);
                            if (out_status) *out_status = 400;
                            return 0;
                        }
                    }

                    if (run_cgi_command(client, interp, filepath, envp, body_buf, content_len, &status) == 0) {
                        if (body_buf) free(body_buf);
                        /* run_cgi_command streams the response directly. We consider request handled. */
                        total_sent = 0; /* response already sent */
                        shutdown(client, SD_SEND); closesocket(client);
                        if (out_status) *out_status = status;
                        /* Log entry will be written below by returning early; to avoid double close we return here. */
                        char ipstr[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &addr->sin_addr, ipstr, sizeof(ipstr));
                        time_t now = time(NULL); struct tm local_tm;
#if defined(_MSC_VER) || defined(__MINGW32__)
                        localtime_s(&local_tm, &now);
#else
                        localtime_r(&now, &local_tm);
#endif
                        char timestr[64]; strftime(timestr, sizeof(timestr), "%d/%b/%Y:%H:%M:%S", &local_tm);
                        char request_line[512]; snprintf(request_line, sizeof(request_line), "%s %s", method, url);
                        log_printf("ACCESS", "%s - - [%s +0000] \"%s HTTP/1.1\" %d %ld \"%s\" \"%s\"", ipstr, timestr, request_line, status, total_sent,
                                   referer_buf[0] ? referer_buf : "-", ua_buf[0] ? ua_buf : "-");
                        return 0;
                    } else {
                        const char *err500 = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
                        send(client, err500, (int)strlen(err500), 0);
                        total_sent += (int)strlen(err500);
                        status = 500;
                    }
                }
            }
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
