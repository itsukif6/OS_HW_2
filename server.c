/*
 * server.c - 多執行緒檔案伺服器
 * 功能：提供檔案建立、讀寫與權限管理，並支援多個客戶端同時連線。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#define PORT 8888
#define MAX_FILES 20     // 最大檔案數量限制
#define BUFFER_SIZE 1024 // 防止溢位

// 檔案資訊結構：用來記錄伺服器上管理的檔案狀態
typedef struct
{
    char name[50];         // 檔案名稱
    char owner[50];        // 擁有者名稱
    char group[50];        // 所屬群組
    char perms[10];        // 權限字串 (6碼，格式如 "rwrnnn"，代表 擁有者/群組/其他人 的 讀/寫 權限)
    int is_used;           // 標記此結構是否已被使用 (1:有檔案, 0:空閒)
    pthread_rwlock_t lock; // PTHREAD 內建的讀寫鎖：允許多個讀取者，寫入時獨佔
} FileEntry;

// 全域檔案清單與互斥鎖
FileEntry file_list[MAX_FILES];
pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER; // 用於保護檔案清單的變更 (如建立新檔案時)，用 PTHREAD 內建的 mutex lock

// 客戶端連線資訊結構
typedef struct
{
    int sockfd;     // socket file descriptor
    char user[50];  // 使用者名稱
    char group[50]; // 使用者群組
} ClientInfo;

// 初始化檔案清單：重置使用狀態並初始化讀寫鎖
void init_files()
{
    for (int i = 0; i < MAX_FILES; i++)
    {
        file_list[i].is_used = 0;
        pthread_rwlock_init(&file_list[i].lock, NULL);
    }
}

// 印出目前的 Capability List (伺服器端除錯用)
void print_capability_lists()
{
    printf("\n=== Capability List (伺服器當前狀態) ===\n");
    printf("%-20s %-10s %-10s %-10s\n", "檔名", "擁有者", "群組", "權限");
    for (int i = 0; i < MAX_FILES; i++)
    {
        if (file_list[i].is_used)
        {
            printf("%-20s %-10s %-10s %-10s\n",
                   file_list[i].name, file_list[i].owner, file_list[i].group, file_list[i].perms);
        }
    }
    printf("=======================================\n\n");
}

// 檢查權限字串格式是否合法 (必須是 6 碼，且奇數位為 r/n，偶數位為 w/n)
int check_perm_format(const char *perms)
{
    if (strlen(perms) != 6)
        return 0;
    for (int i = 0; i < 6; i++)
    {
        if (i % 2 == 0) // 讀取位元 (Read bit)
        {
            if (perms[i] != 'r' && perms[i] != 'n')
                return 0;
        }
        else // 寫入位元 (Write bit)
        {
            if (perms[i] != 'w' && perms[i] != 'n')
                return 0;
        }
    }
    return 1;
}

// 核心權限檢查邏輯
// 根據使用者身分決定檢查權限字串的哪個部分
int check_permission(FileEntry *file, char *user, char *group, char mode)
{
    int offset = 4; // 預設檢查 "其他人" (Others) 的權限 (索引 4, 5)

    // strcmp 相等 return 0
    if (strcmp(file->owner, user) == 0)
        offset = 0; // 若是擁有者，檢查索引 0, 1
    else if (strcmp(file->group, group) == 0)
        offset = 2; // 若同群組，檢查索引 2, 3

    // 根據讀取 (r) 或寫入 (w) 模式，決定檢查該區段的第 1 個或第 2 個字元
    char p_target = (mode == 'r') ? file->perms[offset] : file->perms[offset + 1];

    if (mode == 'r' && p_target == 'r')
        return 1;
    if (mode == 'w' && p_target == 'w')
        return 1;
    return 0;
}

// 執行緒函式：處理單一客戶端的連線
void *client_handler(void *arg)
{
    ClientInfo *client = (ClientInfo *)arg;
    int sock = client->sockfd;
    char buffer[BUFFER_SIZE];
    int n;

    // --- Stage 1: 接收並驗證登入資訊 ---
    memset(buffer, 0, BUFFER_SIZE);              // 將 buffer 設為 0
    if (recv(sock, buffer, BUFFER_SIZE, 0) <= 0) // 從 buffer 讀資料到 sock，回傳值小於或等於 0 = 連線無效
    {
        close(sock);
        free(client);
        return NULL;
    }
    sscanf(buffer, "%s %s", client->user, client->group);

    // 檢查群組名稱是否在允許清單中，只能用 AOS-group、CSE-group
    if (strcmp(client->group, "AOS-group") != 0 && strcmp(client->group, "CSE-group") != 0)
    {
        printf("登入失敗: %s 使用了無效群組 %s\n", client->user, client->group);
        char *error_msg = "Login Failed: Invalid Group. Only 'AOS-group' or 'CSE-group' allowed.";
        send(sock, error_msg, strlen(error_msg), 0);

        close(sock);
        free(client);
        return NULL;
    }

    // 登入成功，回傳確認訊息
    printf("客戶端登入成功: 使用者=%s, 群組=%s\n", client->user, client->group);
    send(sock, "Login OK", 8, 0);

    // --- Stage 2: 指令處理迴圈 ---
    while (1)
    {
        memset(buffer, 0, BUFFER_SIZE); // 將 buffer 設為 0
        n = recv(sock, buffer, BUFFER_SIZE, 0);
        if (n <= 0) // 連線無效 or 客戶端斷線
            break;

        char cmd[10], arg1[50], arg2[50];
        memset(cmd, 0, sizeof(cmd));
        memset(arg1, 0, sizeof(arg1)); // new, read, write, change
        memset(arg2, 0, sizeof(arg2)); // 權限

        // 解析指令：Cmd [Arg1] [Arg2]
        sscanf(buffer, "%s %s %s", cmd, arg1, arg2);
        char response[BUFFER_SIZE];
        memset(response, 0, BUFFER_SIZE); // 將 response 設為 0

        // 指令：建立新檔案 (new)
        if (strcmp(cmd, "new") == 0)
        {
            // 先檢查權限格式
            if (!check_perm_format(arg2))
            {
                sprintf(response, "錯誤: 權限格式錯誤 (必須是6碼，例如 rwrnnn)");
            }
            else
            {
                // 修改全域檔案清單，必須上 Mutex 鎖
                pthread_mutex_lock(&list_mutex);
                int idx = -1;
                // 在 file_list 中尋找空閒的檔案位置
                for (int i = 0; i < MAX_FILES; i++)
                    if (!file_list[i].is_used)
                    {
                        idx = i;
                        break;
                    }

                if (idx != -1)
                {
                    // 複製變數到 file_list 之 key 所對應的 value
                    strcpy(file_list[idx].name, arg1);
                    strcpy(file_list[idx].perms, arg2);
                    strcpy(file_list[idx].owner, client->user);
                    strcpy(file_list[idx].group, client->group);
                    file_list[idx].is_used = 1;

                    // 實際在磁碟建立檔案
                    FILE *fp = fopen(arg1, "w");
                    if (fp)
                    {
                        fprintf(fp, "Init file: %s\n", arg1);
                        fclose(fp);
                    }

                    sprintf(response, "檔案 %s 建立成功。", arg1);
                    print_capability_lists();
                }
                else
                {
                    sprintf(response, "錯誤: 伺服器空間已滿。");
                }
                pthread_mutex_unlock(&list_mutex); // 釋放 Mutex 鎖
            }
        }
        // 指令：變更權限 (change)
        else if (strcmp(cmd, "change") == 0)
        {
            // 檢查權限
            if (!check_perm_format(arg2))
            {
                sprintf(response, "錯誤: 權限格式錯誤 (必須是6碼，例如 rwrnnn)");
            }
            else
            {
                int found = 0;
                for (int i = 0; i < MAX_FILES; i++)
                {
                    if (file_list[i].is_used && strcmp(file_list[i].name, arg1) == 0)
                    {
                        // 只有擁有者可以變更權限
                        if (strcmp(file_list[i].owner, client->user) == 0)
                        {
                            // 變更權限
                            strcpy(file_list[i].perms, arg2);
                            sprintf(response, "檔案 %s 權限已變更。", arg1);
                            print_capability_lists();
                        }
                        else
                        {
                            sprintf(response, "錯誤: 你不是擁有者，無法變更權限。");
                        }
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    sprintf(response, "錯誤: 找不到檔案。");
            }
        }
        // 指令：讀取檔案 (read)
        else if (strcmp(cmd, "read") == 0)
        {
            int idx = -1; // 檔案在 file_list 中的 index 位置
            for (int i = 0; i < MAX_FILES; i++)
            {
                if (file_list[i].is_used && strcmp(file_list[i].name, arg1) == 0)
                {
                    idx = i;
                    break;
                }
            }

            if (idx != -1)
            {
                // 檢查是否有讀取權限
                if (check_permission(&file_list[idx], client->user, client->group, 'r'))
                {
                    // 嘗試取得讀鎖，如果有人正在寫入則會等待
                    int lock_result = pthread_rwlock_tryrdlock(&file_list[idx].lock);
                    if (lock_result != 0)
                    {
                        // 無法立即取得讀鎖，表示有人正在寫入
                        sprintf(response, "該檔案正在被寫入");
                        send(sock, response, strlen(response), 0);
                        
                        // 等待寫入完成
                        pthread_rwlock_rdlock(&file_list[idx].lock);
                        
                        // 寫入完成後通知客戶端
                        memset(response, 0, BUFFER_SIZE);
                        sprintf(response, "寫入完成");
                        send(sock, response, strlen(response), 0);
                    }

                    printf("[Read] %s 正在讀取... (模擬延遲耗時 5 秒)\n", client->user);
                    sleep(5); // 模擬讀取耗時 5 秒，可以用來測試併發讀取

                    FILE *fp = fopen(arg1, "r");
                    if (fp)
                    {
                        char content[500];
                        fgets(content, 500, fp);
                        sprintf(response, "讀取內容: %s", content);
                        fclose(fp);
                    }
                    else
                    {
                        sprintf(response, "錯誤: 讀取失敗 (I/O Error)。");
                    }
                    // 釋放鎖
                    pthread_rwlock_unlock(&file_list[idx].lock);
                }
                else
                {
                    sprintf(response, "權限不足: 無法讀取。");
                }
            }
            else
            {
                sprintf(response, "錯誤: 找不到檔案。");
            }
        }
        // 指令：寫入檔案 (write)
        else if (strcmp(cmd, "write") == 0)
        {
            int idx = -1; // 檔案在 file_list 中的 index 位置
            for (int i = 0; i < MAX_FILES; i++)
            {
                if (file_list[i].is_used && strcmp(file_list[i].name, arg1) == 0)
                {
                    idx = i;
                    break;
                }
            }

            if (idx != -1)
            {
                // 檢查是否有寫入權限
                if (check_permission(&file_list[idx], client->user, client->group, 'w'))
                {
                    // 嘗試取得寫鎖，如果有人正在讀取或寫入則會等待
                    int lock_result = pthread_rwlock_trywrlock(&file_list[idx].lock);
                    if (lock_result != 0)
                    {
                        // 無法立即取得寫鎖，表示有人正在讀取
                        sprintf(response, "該檔案正在被讀取");
                        send(sock, response, strlen(response), 0);
                        
                        // 等待讀取完成
                        pthread_rwlock_wrlock(&file_list[idx].lock);
                        
                        // 讀取完成後通知客戶端
                        memset(response, 0, BUFFER_SIZE);
                        sprintf(response, "讀取完成");
                        send(sock, response, strlen(response), 0);
                    }

                    printf("[Write] %s 正在寫入... (模擬延遲耗時 10 秒)\n", client->user);
                    sleep(10); // 模擬寫入耗時 10 秒，可以用來測試鎖定機制

                    FILE *fp;
                    // 判斷是覆蓋 (o) 還是附加 (a) 模式
                    if (strcmp(arg2, "o") == 0)
                        fp = fopen(arg1, "w");
                    else
                        fp = fopen(arg1, "a");

                    if (fp)
                    {
                        // 寫入的資訊格式: xxx wrote here at xx/xx/xx-xx:xx:xx.
                        time_t now = time(NULL);
                        struct tm *t = localtime(&now);
                        char time_str[64];
                        strftime(time_str, sizeof(time_str), "%Y/%m/%d-%H:%M:%S", t);

                        fprintf(fp, "%s wrote here at %s.\n", client->user, time_str);
                        fclose(fp);
                        sprintf(response, "寫入成功 (時間: %s)。", time_str);
                    }
                    else
                    {
                        sprintf(response, "錯誤: 寫入失敗 (I/O Error)。");
                    }
                    // 釋放鎖
                    pthread_rwlock_unlock(&file_list[idx].lock);
                }
                else
                {
                    sprintf(response, "權限不足: 無法寫入。");
                }
            }
            else
            {
                sprintf(response, "錯誤: 找不到檔案。");
            }
        }
        // exception
        else
        {
            sprintf(response, "無效指令。");
        }
        // 將執行結果回傳給客戶端
        send(sock, response, strlen(response), 0);
    }
    // 清理資源
    close(sock);
    free(client);
    return NULL;
}

int main()
{
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    // 初始化檔案清單：重置使用狀態並初始化讀寫鎖
    init_files();

    // 建立 Socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("Socket failed");
        exit(1);
    }

    // 設定 Socket 選項，允許重用位址 (避免伺服器重啟時 Port 被佔用)
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // 監聽所有網卡介面
    address.sin_port = htons(PORT);

    // 綁定 Port
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("Bind failed");
        exit(1);
    }
    // 開始監聽，最大等待佇列為 5
    if (listen(server_fd, 5) < 0)
    {
        perror("Listen failed");
        exit(1);
    }

    printf("伺服器啟動 (Port %d)...\n", PORT);

    // 主迴圈：等待連線並產生 Thread
    while (1)
    {
        ClientInfo *new_client = malloc(sizeof(ClientInfo));
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0)
        {
            perror("Accept failed");
            free(new_client);
            continue;
        }
        new_client->sockfd = new_socket;

        // 建立執行緒處理新的客戶端
        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, (void *)new_client);
        pthread_detach(tid); // 分離執行緒，結束時自動回收資源
    }
    return 0;
}