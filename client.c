/*
 * client.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define PORT 8888
#define BUFFER_SIZE 1024

int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE] = {0};
    char user[50], group[50];

    // 建立 Socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // 將 IP 字串轉換為二進位格式 (本機測試用 127.0.0.1)
    if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
    }

    // 連線
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
        return -1;
    }

    // 輸入使用者身分
    printf("請輸入你的名字: ");
    scanf("%s", user);
    printf("請輸入你的群組 (例如 AOS-group, CSE-group): ");
    scanf("%s", group);

    // 傳送登入資訊
    sprintf(buffer, "%s %s", user, group);
    send(sock, buffer, strlen(buffer), 0);

    // 接收登入確認
    memset(buffer, 0, BUFFER_SIZE);
    read(sock, buffer, BUFFER_SIZE);
    printf("伺服器回應: %s\n", buffer);

    printf("\n=== 指令說明 ===\n");
    printf("1. 建立檔案: new [檔名] [權限: rwrnnn]\n");
    printf("2. 讀取檔案: read [檔名]\n");
    printf("3. 寫入檔案: write [檔名] [模式: o(覆蓋)/a(附加)]\n");
    printf("4. 變更權限: change [檔名] [權限]\n");
    printf("範例: new hw2.c rwrnnn\n");
    printf("輸入 'exit' 離開程式。\n\n");

    // 清除輸入緩衝區的換行符號
    getchar(); 

    while(1) {
        printf("> ");
        memset(buffer, 0, BUFFER_SIZE);
        fgets(buffer, BUFFER_SIZE, stdin); // 讀取整行指令

        // 移除換行符號
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "exit") == 0) {
            break;
        }

        // 傳送指令
        send(sock, buffer, strlen(buffer), 0);
        printf("等待伺服器回應...\n");

        // 接收結果
        memset(buffer, 0, BUFFER_SIZE);
        int valread = read(sock, buffer, BUFFER_SIZE);
        if (valread > 0) {
            printf("伺服器: %s\n", buffer);
        } else {
            printf("伺服器已斷線。\n");
            break;
        }
    }

    close(sock);
    return 0;
}