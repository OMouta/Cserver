# Win32 C HTTP server (MSYS2 / MinGW)

Minimal, small HTTP server written in C using the Win32/Winsock2 API. It serves static files from a directory and provides a tiny thread pool, access logging (Combined Log Format), and size-based log rotation.

This repository is intended for learning, experimentation, and small internal use-cases. It is not a
drop-in production web server. See the "Limitations and security" section for recent hardening and
remaining gaps.

## Highlights

- Serve a directory of static files (default `public`).
- Small thread pool (configurable worker count).
- Access logs written in Combined Log Format (CLF) when `--log-file` is used.
- Size-based log rotation with `--log-rotate-size` and `--log-rotate-count`.
- Graceful shutdown on Ctrl+C.

## Recent hardening

- Header size limit: request headers are limited to the first 8 KiB of data; if the header terminator `\r\n\r\n` is
  not received before the buffer fills the server returns `431 Request Header Fields Too Large` and closes the connection.
- Request-URI decode limit: decoded request paths are bounded to 1024 bytes; if decoding would exceed that the server returns
  `414 Request-URI Too Long` and closes the connection.
- Path canonicalization: requested paths are canonicalized (using `_fullpath`) and must reside under the configured serve
  directory; requests that resolve outside the serve dir return `403 Forbidden`.

These are small, pragmatic protections useful for low-risk deployments. They do not replace a full HTTP/CORS/TLS
defense-in-depth model.

## Build

You can build this project either from MSYS2 (MinGW) or with a standard Windows toolchain that provides `gcc`.

MSYS2 (MinGW64) recommended steps:

1. Open "MSYS2 MinGW 64-bit".
2. Ensure toolchain is installed: pacman -Syu mingw-w64-x86_64-gcc make
3. Build with make:

```powershell
make
```

Or build directly with gcc (PowerShell / MSYS2 shell):

```powershell
gcc -O2 -Wall -o server.exe src/server.c src/http.c src/log.c src/threadpool.c src/state.c src/utils.c -lws2_32
```

If you use a different compiler or cross-compile, adjust flags accordingly.

## Run

Default: serve `./public` on port 8080:

```powershell
.\server.exe
```

Examples:

- Serve `public` on port 8080 with 4 workers and write access logs to `server.log`:

```powershell
.\server.exe --port 8080 --serve public --workers 4 --log-file .\server.log
```

- Log rotation: rotate logs at ~10 MB and keep 5 rotated files:

```powershell
.\server.exe --log-file .\server.log --log-rotate-size 10M --log-rotate-count 5
```

## Command-line flags

- `--port <port>` — TCP port to listen on (default 8080).
- `--serve <path>` — directory to serve (default `public`). `--public` is accepted as an alias for compatibility.
- `--workers <N>` — number of worker threads in the pool (default 8).
- `--log-file <path>` — append access and internal logs to the provided file.
- `--log-rotate-size <size>` — enable size-based rotation; examples: `1K`, `10M`, `1G`.
- `--log-rotate-count <n>` — how many rotated files to keep (default 5).

The server writes access logs in Combined Log Format when `--log-file` is set:

%h %l %u [%t] "%r" %>s %b "%{Referer}i" "%{User-agent}i"

Example access line:

```log
127.0.0.1 - - [17/Aug/2025:00:35:10 +0000] "GET /index.html HTTP/1.1" 200 1024 "-" "curl/7.68.0"
```

## Limitations and security notes

This server is intentionally small and educational. Recent hardening above (header, URI, and canonicalization limits)
improves resilience, but there are still notable limitations:

- No TLS. Use a reverse proxy (nginx, Caddy) or a TLS-terminating load balancer for production.
- Minimal HTTP parsing: only basic GET is implemented; many headers and edge-cases are not fully RFC-compliant.
- No rate limiting or per-IP connection limits.
- Log rotation is simple rename-based; no compression or time-based rotation.
- No authentication or authorization.

If you need production-grade behavior, run this behind a hardened proxy or gateway and add tests + CI.

## Files

- `src/server.c` - server entrypoint and argument parsing
- `src/http.c` / `src/http.h` - HTTP request parsing and static file serving (contains header/URI limits & canonicalization)
- `src/log.c` / `src/log.h` - centralized logging and rotation
- `src/threadpool.c` / `src/threadpool.h` - worker queue and thread pool
- `src/state.c` / `src/state.h` - shared runtime state (running flag, listen socket)
- `src/utils.c` / `src/utils.h` - small helpers (parse_size, header sanitization)
- `Makefile` - build helper
- `README.md` - this file

## Contributing

Small patches, tests, and documentation improvements welcome. If you change public behavior (status codes, limits),
please add tests that demonstrate the behavior.

## License

This project uses the MIT License.
