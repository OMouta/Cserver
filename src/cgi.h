#pragma once
#include <winsock2.h>
#include <stddef.h>

// Run a CGI interpreter for a script file. This spawns the interpreter process, passes
// the provided environment pairs (NULL-terminated array of "KEY=VALUE" strings), writes
// the request body to the child's stdin, and forwards the child's stdout directly to the
// client socket. Returns 0 on success, -1 on error. On success out_status may be set
// to the HTTP status code the CGI produced (best-effort; if unknown, set to 200).
int run_cgi_command(SOCKET client, const char *interpreter_cmd, const char *script_path,
                    const char **env_pairs, const char *body, size_t body_len, int *out_status);
