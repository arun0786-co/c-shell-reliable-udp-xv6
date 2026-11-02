CC = gcc
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Werror \
	-Wno-unused-parameter -fno-asm

# OpenSSL provides MD5 to the server. pkg-config knows where it is on Linux and
# with Homebrew; without pkg-config try Homebrew's openssl@3, then the defaults.
SSL_CFLAGS := $(shell pkg-config --cflags libcrypto 2>/dev/null)
SSL_LIBS := $(shell pkg-config --libs libcrypto 2>/dev/null)
ifeq ($(SSL_LIBS),)
SSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
ifneq ($(SSL_PREFIX),)
SSL_CFLAGS := -I$(SSL_PREFIX)/include
SSL_LIBS := -L$(SSL_PREFIX)/lib
endif
SSL_LIBS += -lcrypto
endif

COMMON = sham.c chat.c

all: server client

server: server.c $(COMMON) sham.h
	$(CC) $(CFLAGS) $(SSL_CFLAGS) -o $@ server.c $(COMMON) $(SSL_LIBS)

client: client.c $(COMMON) sham.h
	$(CC) $(CFLAGS) -o $@ client.c $(COMMON)

test: all
	./test.sh

clean:
	rm -f server client server_log.txt client_log.txt

.PHONY: all test clean
