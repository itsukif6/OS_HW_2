# 編譯器
CC = gcc

# 用-pthread：因為 Server 用到了多執行緒
CFLAGS = -Wall -pthread

# 目標檔案
all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

# 清除生成的檔案
clean:
	rm -f server client