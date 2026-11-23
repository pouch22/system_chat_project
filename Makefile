CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE

.PHONY: all clean

all: server client

server: server.c old_chat.c chat.h
	$(CC) $(CFLAGS) -o $@ server.c old_chat.c

client: client.c chat.h
	$(CC) $(CFLAGS) -pthread -o $@ $< -lncursesw -ltinfo

clean:
	rm -f server client
