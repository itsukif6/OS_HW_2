/*
 * client.c - 客戶端程式
 * 功能：連線至伺服器，發送使用者身分，並提供互動式指令介面。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define PORT 8888
#define BUFFER_SIZE 1024

int main()
{
    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE] = {0};
    char user[50], group[50];

    // 建立 Socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        printf("\n Socket creation error \n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // 將 IP 字串轉換為網路二進位格式
    // 若要在不同電腦執行，請將 "127.0.0.1" 改為伺服器的實際 IP
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0)
    {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
    }

    // 發起連線
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        printf("\nConnection Failed \n");
        return -1;
    }

    // 輸入使用者身分
    printf("請輸入你的名字: ");
    scanf("%s", user);
    printf("請輸入你的群組 (例如 AOS-group, CSE-group): ");
    scanf("%s", group);

    // 1. 傳送登入資訊給伺服器
    sprintf(buffer, "%s %s", user, group);
    send(sock, buffer, strlen(buffer), 0);

    // 2. 接收登入結果 (成功或失敗)
    memset(buffer, 0, BUFFER_SIZE);
    read(sock, buffer, BUFFER_SIZE);
    printf("伺服器回應: %s\n", buffer);

    // 顯示操作說明
    printf("\n=== 指令說明 ===\n");
    printf("1. 建立檔案: new [檔名] [權限: rwrnnn]\n");
    printf("2. 讀取檔案: read [檔名]\n");
    printf("3. 寫入檔案: write [檔名] [模式: o(覆蓋)/a(附加)]\n");
    printf("4. 變更權限: change [檔名] [權限]\n");
    printf("範例: new test.c rwrnnn\n");
    printf("輸入 'exit' 離開程式。\n\n");

    // 清除輸入緩衝區殘留的換行符號，避免影響下一次 fgets
    getchar();

    // 互動式指令迴圈
    while (1)
    {
        printf("> ");
        memset(buffer, 0, BUFFER_SIZE);
        // 使用 fgets 讀取整行，允許參數間有空格
        fgets(buffer, BUFFER_SIZE, stdin);

        // 移除字串末端的換行符號
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "exit") == 0)
        {
            break;
        }

        // 傳送指令至伺服器
        send(sock, buffer, strlen(buffer), 0);
        printf("等待伺服器回應...\n");

        // 接收並顯示執行結果
        memset(buffer, 0, BUFFER_SIZE);
        int valread = read(sock, buffer, BUFFER_SIZE);
        if (valread > 0)
        {
            printf("伺服器: %s\n", buffer);
        }
        else
        {
            printf("伺服器已斷線。\n");
            break;
        }
    }

    close(sock);
    return 0;
}