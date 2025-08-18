#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cgi.h"

#define CGI_READ_BUF 8192

// Helper to send data to socket; returns 1 on success, 0 on failure
static int socket_send_all(SOCKET s, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t rem = len;
    while (rem > 0) {
        int sent = send(s, p, (int)rem, 0);
        if (sent == SOCKET_ERROR || sent == 0) return 0;
        p += sent; rem -= sent;
    }
    return 1;
}

int run_cgi_command(SOCKET client, const char *interpreter_cmd, const char *script_path,
                    const char **env_pairs, const char *body, size_t body_len, int *out_status) {
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hChildStdinRd = NULL, hChildStdinWr = NULL;
    HANDLE hChildStdoutRd = NULL, hChildStdoutWr = NULL;
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOA si = {0};
    char cmdline[4096];

    // Create pipes
    if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &sa, 0)) goto err;
    if (!CreatePipe(&hChildStdinRd, &hChildStdinWr, &sa, 0)) goto err;

    // Ensure the read handle is not inherited by the child
    SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStdinWr, HANDLE_FLAG_INHERIT, 0);

    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hChildStdinRd;
    si.hStdOutput = hChildStdoutWr;
    si.hStdError = hChildStdoutWr;

    /* Quote the interpreter path to handle spaces/apostrophes in the path (e.g. D:\IDE's\...) */
    _snprintf_s(cmdline, sizeof(cmdline), _TRUNCATE, "\"%s\" \"%s\"", interpreter_cmd, script_path);

    // Build environment block
    LPCH curenv = GetEnvironmentStringsA();
    char *envblock = NULL; size_t envblock_len = 0;
    if (curenv) {
        LPCH p = curenv;
        while (!(p[0] == '\0' && p[1] == '\0')) p++;
        envblock_len = (size_t)(p - curenv) + 2;
        envblock = (char*)malloc(envblock_len + 1);
        if (envblock) memcpy(envblock, curenv, envblock_len);
        FreeEnvironmentStringsA(curenv);
    }
    if (env_pairs) {
        size_t extra = 0;
        for (const char **e = env_pairs; *e; ++e) extra += strlen(*e) + 1;
        if (extra > 0) {
            size_t base = envblock_len > 0 ? envblock_len - 1 : 0;
            char *newblock = (char*)malloc(base + extra + 2);
            if (!newblock) goto err;
            if (envblock) { memcpy(newblock, envblock, base); free(envblock); envblock = NULL; }
            char *dst = newblock + base;
            for (const char **e = env_pairs; *e; ++e) {
                size_t l = strlen(*e);
                memcpy(dst, *e, l); dst += l; *dst++ = '\0';
            }
            *dst++ = '\0'; envblock = newblock; envblock_len = (size_t)(dst - newblock);
        }
    }

    /* Use the script's directory as the child working directory so interpreters (php-cgi) can resolve relative paths. */
    char child_cwd[MAX_PATH] = {0};
    const char *slash = strrchr(script_path, '\\');
    if (!slash) slash = strrchr(script_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - script_path);
        if (dlen < sizeof(child_cwd)) {
            memcpy(child_cwd, script_path, dlen);
            child_cwd[dlen] = '\0';
        }
    }
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_SUSPENDED, envblock, child_cwd[0] ? child_cwd : NULL, &si, &pi)) goto err2;

    CloseHandle(hChildStdoutWr);
    CloseHandle(hChildStdinRd);

    /* Create a Job object to ensure we can terminate the process tree if it misbehaves. */
    HANDLE hJob = CreateJobObjectA(NULL, NULL);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        ZeroMemory(&jeli, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        if (!AssignProcessToJobObject(hJob, pi.hProcess)) {
            log_printf("WARN", "AssignProcessToJobObject failed for %s (err=%lu)", script_path, GetLastError());
            CloseHandle(hJob); hJob = NULL;
        } else {
            log_printf("DEBUG", "Created job object and assigned process for %s", script_path);
        }
    } else {
        log_printf("DEBUG", "CreateJobObjectA returned NULL for %s (err=%lu)", script_path, GetLastError());
    }

    /* Resume the suspended child now that it's assigned to the job. */
    ResumeThread(pi.hThread);

    // Write body then close stdin
    if (body_len > 0 && body != NULL) {
        DWORD written = 0;
        WriteFile(hChildStdinWr, body, (DWORD)body_len, &written, NULL);
    }
    CloseHandle(hChildStdinWr);

    // Read stdout, parse headers, send response head, then stream body
    {
        char buf[CGI_READ_BUF];
        char hdrbuf[8192]; size_t hdrfill = 0; int headers_sent = 0;
        DWORD n = 0; BOOL ok;
        while ((ok = ReadFile(hChildStdoutRd, buf, sizeof(buf), &n, NULL)) && n > 0) {
            if (!headers_sent) {
                size_t tocopy = (size_t)n;
                if (hdrfill + tocopy > sizeof(hdrbuf)) tocopy = sizeof(hdrbuf) - hdrfill;
                memcpy(hdrbuf + hdrfill, buf, tocopy); hdrfill += tocopy;
                char *end = NULL;
                for (size_t i = 0; i + 3 < hdrfill; ++i) {
                    if (hdrbuf[i] == '\r' && hdrbuf[i+1] == '\n' && hdrbuf[i+2] == '\r' && hdrbuf[i+3] == '\n') { end = hdrbuf + i + 4; break; }
                }
                if (!end) {
                    for (size_t i = 0; i + 1 < hdrfill; ++i) {
                        if (hdrbuf[i] == '\n' && hdrbuf[i+1] == '\n') { end = hdrbuf + i + 2; break; }
                    }
                }
                if (end) {
                    size_t hdrlen = (size_t)(end - hdrbuf);
                    if (hdrlen >= sizeof(hdrbuf)) hdrlen = sizeof(hdrbuf) - 1;
                    hdrbuf[hdrlen] = '\0';
                    char *p = hdrbuf;
                    int status_code = 200; char status_reason[128] = "OK";
                    char resphead[8192]; size_t resphead_len = 0;
                    while ((size_t)(p - hdrbuf) < hdrlen) {
                        char *ln = memchr(p, '\n', (size_t)(&hdrbuf[hdrlen] - p));
                        size_t linelen = ln ? (size_t)(ln - p) : (size_t)(&hdrbuf[hdrlen] - p);
                        if (linelen > 0 && p[linelen-1] == '\r') linelen--;
                        if (linelen == 0) break;
                        char line[1024]; size_t cl = linelen < sizeof(line)-1 ? linelen : sizeof(line)-1;
                        memcpy(line, p, cl); line[cl] = '\0';
                        if (_strnicmp(line, "Status:", 7) == 0) {
                            char *v = line + 7; while (*v == ' ') v++;
                            status_code = atoi(v);
                            char *sp = strchr(v, ' ');
                            if (sp) { while (*sp == ' ') sp++; strncpy(status_reason, sp, sizeof(status_reason)-1); status_reason[sizeof(status_reason)-1] = '\0'; }
                        } else {
                            resphead_len += snprintf(resphead + resphead_len, sizeof(resphead) - resphead_len, "%s\r\n", line);
                        }
                        if (!ln) break;
                        p = ln + 1;
                    }
                    char status_line[256]; int slen = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d%s%s\r\n", status_code,
                        status_reason[0] ? " " : "", status_reason);
                    socket_send_all(client, status_line, (size_t)slen);
                    if (resphead_len > 0) socket_send_all(client, resphead, resphead_len);
                    socket_send_all(client, "\r\n", 2);
                    headers_sent = 1;
                    size_t header_consumed = hdrlen;
                    size_t body_in_hdr = hdrfill - header_consumed;
                    if (body_in_hdr > 0) socket_send_all(client, hdrbuf + header_consumed, body_in_hdr);
                    if (tocopy < (size_t)n) socket_send_all(client, buf + tocopy, (size_t)n - tocopy);
                } else {
                    if (hdrfill == sizeof(hdrbuf)) { socket_send_all(client, hdrbuf, hdrfill); hdrfill = 0; headers_sent = 1; }
                }
            } else {
                if (!socket_send_all(client, buf, (size_t)n)) break;
            }
        }
    }

    /* Wait for process to finish, but enforce a timeout to avoid blocking workers indefinitely. */
    const DWORD CGI_TIMEOUT_MS = 5000; /* 5s default */
    DWORD waitRes = WaitForSingleObject(pi.hProcess, CGI_TIMEOUT_MS);
    DWORD exitCode = 0;
    if (waitRes == WAIT_TIMEOUT) {
        /* Timeout: terminate job (kills child and children) and mark as error. */
        log_printf("WARN", "CGI timeout after %u ms for interpreter='%s' script='%s' pid=%lu", CGI_TIMEOUT_MS, interpreter_cmd, script_path, (unsigned long)GetProcessId(pi.hProcess));
        if (hJob) {
            if (!TerminateJobObject(hJob, 1)) log_printf("ERROR", "TerminateJobObject failed (err=%lu)", GetLastError());
            else log_printf("INFO", "Terminated job object for script='%s'", script_path);
        } else {
            if (!TerminateProcess(pi.hProcess, 1)) log_printf("ERROR", "TerminateProcess failed (err=%lu)", GetLastError());
            else log_printf("INFO", "Terminated process for script='%s'", script_path);
        }
        if (out_status) *out_status = 504; /* gateway timeout-ish */
    } else {
        if (GetExitCodeProcess(pi.hProcess, &exitCode)) if (out_status) *out_status = (int)exitCode;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hJob) CloseHandle(hJob);
    CloseHandle(hChildStdoutRd);
    if (envblock) free(envblock);
    return 0;

err:
    if (hChildStdoutRd) CloseHandle(hChildStdoutRd);
    if (hChildStdoutWr) CloseHandle(hChildStdoutWr);
    if (hChildStdinRd) CloseHandle(hChildStdinRd);
    if (hChildStdinWr) CloseHandle(hChildStdinWr);
    return -1;

err2:
    if (envblock) free(envblock);
    goto err;
}
