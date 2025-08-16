CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lws2_32
SRC = src/server.c
OUT = server.exe

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(LDFLAGS)

clean:
	del /Q $(OUT) || rm -f $(OUT)

.PHONY: all clean
