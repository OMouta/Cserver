CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lws2_32
SRCS = src/server.c src/http.c src/log.c src/threadpool.c src/state.c src/utils.c
OUT = Cserver.exe

all: $(OUT)

$(OUT): $(SRCS)
	$(CC) $(CFLAGS) -o $(OUT) $(SRCS) $(LDFLAGS)

clean:
	del /Q $(OUT) || rm -f $(OUT)

.PHONY: all clean
