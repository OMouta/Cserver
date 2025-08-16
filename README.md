# Win32 C HTTP server (MSYS2 / MinGW)

Minimal HTTP server written in C using the Win32/Winsock2 API. It serves static files from a directory
and supports a small thread pool, access logging (Combined Log Format), and size-based log rotation.

## Features

- Serve a directory of static files (default `public`).
- Thread pool (configurable worker count) to bound concurrent request processing.
- Access logs written in Combined Log Format when `--log-file` is used.
- Size-based log rotation with `--log-rotate-size` and `--log-rotate-count`.
- Graceful shutdown on Ctrl+C.

## Build (MSYS2 MinGW64 shell)

1. Open `MSYS2 MinGW 64-bit` (or `mingw32` if you target 32-bit).
2. Install toolchain if you haven't: `pacman -Syu mingw-w64-x86_64-gcc make`
3. Build with make:

  make

Or build directly with gcc:

  gcc -O2 -Wall -o server.exe src/server.c -lws2_32

## Run (PowerShell or MSYS2 shell)

### Default run (serves `./public` on port 8080)

```powershell
.\server.exe
```

### Recommended run examples

- Serve `public` on port 8080 with 4 workers and write access logs to `server.log`:

```powershell
.\server.exe --port 8080 --serve public --workers 4 --log-file d:\Dev\Cserver\server.log
```

- Enable size-based rotation (rotate files at ~10MB, keep 5 rotated files):

```powershell
.\server.exe --log-file d:\Dev\Cserver\server.log --log-rotate-size 10M --log-rotate-count 5
```

## Notes on flags

- `--port <port>` — TCP port to listen on (default 8080).
- `--serve <path>` — directory to serve (default `public`). `--public` is accepted as an alias for backwards compatibility.
- `--workers <N>` — number of worker threads in the pool (default 8).
- `--log-file <path>` — append access and internal logs to the provided file.
- `--log-rotate-size <size>` — enable size-based rotation; examples: `1K`, `10M`, `1G`.
- `--log-rotate-count <n>` — how many rotated files to keep (default 5).

## Access logs

When `--log-file` is provided the server writes access entries in Combined Log Format (CLF):

%h %l %u [%t] "%r" %>s %b "%{Referer}i" "%{User-agent}i"

### Example

```log
127.0.0.1 - - [17/Aug/2025:00:35:10 +0000] "GET /index.html HTTP/1.1" 200 1024 "-" "curl/7.68.0"
```

## Log rotation

Rotation is size-based (when `--log-rotate-size` is set) and uses the `--log-rotate-count` value to keep numbered backups
(`server.log.1`, `server.log.2`, ...). Rotation occurs atomically in the server process before writing the next entry.

## Limitations & notes

- The server is intentionally small and educational: not production hardened (limited HTTP parsing, no TLS, no advanced caching).
- Log rotation is simple: files are renamed and not compressed. If you need compression or time-based rotation, that can be added.

## Stopping the server

- Press Ctrl+C in the console or close the window. The server attempts a graceful shutdown and waits briefly for workers to finish.

## Files

- `src/server.c` - server implementation
- `Makefile` - build helper
- `README.md` - this file
