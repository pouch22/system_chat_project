CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE

SERVER_BIN = server
CLIENT_BIN = client

HEADERS = chat.h
SERVER_OBJS = server.o old_chat.o
CLIENT_OBJS = client.o

SERVER_LIBS = -lpthread
CLIENT_LIBS = -lncursesw -lpthread

all: $(SERVER_BIN) $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SERVER_LIBS)

$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(CLIENT_LIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all clean clean_all

clean:
	rm -f *.o $(SERVER_BIN) $(CLIENT_BIN)

clean_all: clean
	rm -f /tmp/sysprog_chat.sock chat_history.log
