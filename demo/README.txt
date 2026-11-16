컴파일
$ make

서버 키기
터미널 1 
$ ./server

클라이언트 키기
터미널 2
$ ./client 종민

터미널 3 
$ ./client 철수


**MakeFile EOF에러가 뜬다면 정상적으로 파일을 재생성해야함

예시)
printf 'CC = gcc\nCFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE\n\nSERVER_BIN = server\nCLIENT_BIN = client\n\nHEADERS = chat.h\nSERVER_OBJS = server.o old_chat.o\nCLIENT_OBJS = client.o\n\nSERVER_LIBS = -lpthread\nCLIENT_LIBS = -lncursesw -lpthread\n\nall: $(SERVER_BIN) $(CLIENT_BIN)\n\n$(SERVER_BIN): $(SERVER_OBJS)\n\t$(CC) $(CFLAGS) -o $@ $^ $(SERVER_LIBS)\n\n$(CLIENT_BIN): $(CLIENT_OBJS)\n\t$(CC) $(CFLAGS) -o $@ $^ $(CLIENT_LIBS)\n\n%%.o: %%.c $(HEADERS)\n\t$(CC) $(CFLAGS) -c $< -o $@\n\n.PHONY: all clean clean_all\n\nclean:\n\trm -f *.o $(SERVER_BIN) $(CLIENT_BIN)\n\nclean_all: clean\n\trm -f /tmp/sysprog_chat.sock chat_history.log\n' > Makefile

위 예시보다 각자 AI에게 방법을 물어보는 것이 더 좋습니다.
현 데모빌드에서는 backspace(지움)키가 적용되지 않습니다.