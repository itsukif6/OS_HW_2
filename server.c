/*
 * server.c - 檔案伺服器 (支援權限管理與並行控制)
 * 使用 pthread 處理多客戶端
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

#define PORT 8888
#define MAX_CLIENTS 10
#define MAX_FILES 20
#define BUFFER_SIZE 1024

// 檔案資訊結構 (模擬 Capability List / 檔案屬性)
typedef struct {
    char name[50];       // 檔名
    char owner[50];      // 擁有者
    char group[50];      // 群組
    char perms[7];       // 權限字串 (例如: "rwrnnn")
    int is_used;         // 此欄位是否被使用
    
    // 第二部分重點：讀寫鎖
    // 允許多個讀取者同時進入，但寫入者必須獨佔
    pthread_rwlock_t lock; 
} FileEntry;

// 全域檔案列表 (伺服器的共享資源)
FileEntry file_list[MAX_FILES];
pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER; // 保護檔案列表本身的修改

// 客戶端資訊結構
typedef struct {
    int sockfd;
    char user[50];
    char group[50];
} ClientInfo;

// 初始化檔案列表
void init_files() {
    for(int i=0; i<MAX_FILES; i++) {
        file_list[i].is_used = 0;
        pthread_rwlock_init(&file_list[i].lock, NULL);
    }
}

// 顯示目前的 Capability List (檔案權限表) 給助教看
void print_capability_lists() {
    printf("\n=== 目前伺服器上的檔案能力列表 (Capability List) ===\n");
    printf("%-20s %-10s %-10s %-10s\n", "檔名", "擁有者", "群組", "權限");
    for(int i=0; i<MAX_FILES; i++) {
        if(file_list[i].is_used) {
            printf("%-20s %-10s %-10s %-10s\n", 
                file_list[i].name, file_list[i].owner, file_list[i].group, file_list[i].perms);
        }
    }
    printf("==================================================\n\n");
}

// 檢查權限的函式
// mode: 'r' for read, 'w' for write
int check_permission(FileEntry *file, char *user, char *group, char mode) {
    int offset = 0; // 預設檢查 "其他人" (index 4,5)

    if (strcmp(file->owner, user) == 0) {
        offset = 0; // 擁有者 (index 0,1)
    } else if (strcmp(file->group, group) == 0) {
        offset = 2; // 群組 (index 2,3)
    } else {
        offset = 4; // 其他人
    }

    // perms 格式範例: "rwrnnn" -> index: 012345
    // 偶數 index 是讀取，奇數 index 是寫入
    char p_read = file->perms[offset];
    char p_write = file->perms[offset + 1];

    if (mode == 'r' && p_read == 'r') return 1;
    if (mode == 'w' && p_write == 'w') return 1;

    return 0;
}

// 處理單一客戶端的執行緒
void *client_handler(void *arg) {
    ClientInfo *client = (ClientInfo *)arg;
    int sock = client->sockfd;
    char buffer[BUFFER_SIZE];
    int n;

    // 1. 接收登入資訊
    // 格式預期: "User Group"
    memset(buffer, 0, BUFFER_SIZE);
    if (recv(sock, buffer, BUFFER_SIZE, 0) <= 0) {
        close(sock);
        free(client);
        return NULL;
    }
    sscanf(buffer, "%s %s", client->user, client->group);
    printf("客戶端登入: 使用者=%s, 群組=%s\n", client->user, client->group);
    send(sock, "Login OK", 8, 0);

    // 2. 處理指令迴圈
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        n = recv(sock, buffer, BUFFER_SIZE, 0);
        if (n <= 0) break; // 斷線

        // 解析指令
        char cmd[10], arg1[50], arg2[50];
        int params = sscanf(buffer, "%s %s %s", cmd, arg1, arg2);

        char response[BUFFER_SIZE];
        memset(response, 0, BUFFER_SIZE);

        // === 指令: NEW (建立新檔) ===
        if (strcmp(cmd, "new") == 0) {
            pthread_mutex_lock(&list_mutex); // 鎖定列表以修改
            int idx = -1;
            // 找空位
            for(int i=0; i<MAX_FILES; i++) {
                if(!file_list[i].is_used) { idx = i; break; }
            }
            
            if (idx != -1) {
                strcpy(file_list[idx].name, arg1);
                strcpy(file_list[idx].perms, arg2);
                strcpy(file_list[idx].owner, client->user);
                strcpy(file_list[idx].group, client->group);
                file_list[idx].is_used = 1;
                
                // 建立真實檔案
                FILE *fp = fopen(arg1, "w");
                if(fp) { fprintf(fp, "這是檔案 %s 的初始內容。\n", arg1); fclose(fp); }
                
                sprintf(response, "檔案 %s 建立成功。", arg1);
                print_capability_lists(); // 顯示給助教看
            } else {
                sprintf(response, "錯誤: 伺服器檔案空間已滿。");
            }
            pthread_mutex_unlock(&list_mutex);
        
        // === 指令: CHANGE (修改權限) ===
        } else if (strcmp(cmd, "change") == 0) {
             int found = 0;
             for(int i=0; i<MAX_FILES; i++) {
                if (file_list[i].is_used && strcmp(file_list[i].name, arg1) == 0) {
                    // 只有擁有者可以改權限
                    if (strcmp(file_list[i].owner, client->user) == 0) {
                        strcpy(file_list[i].perms, arg2);
                        sprintf(response, "檔案 %s 權限已變更。", arg1);
                        print_capability_lists(); // 顯示給助教看
                    } else {
                        sprintf(response, "錯誤: 你不是擁有者，無法變更權限。");
                    }
                    found = 1;
                    break;
                }
             }
             if (!found) sprintf(response, "錯誤: 找不到檔案。");

        // === 指令: READ (讀取檔案 - 支援並行) ===
        } else if (strcmp(cmd, "read") == 0) {
            int idx = -1;
            for(int i=0; i<MAX_FILES; i++) {
                if (file_list[i].is_used && strcmp(file_list[i].name, arg1) == 0) {
                    idx = i; break;
                }
            }

            if (idx != -1) {
                if (check_permission(&file_list[idx], client->user, client->group, 'r')) {
                    printf("[並行展示] %s 請求讀取鎖... (若有人在寫入則會等待)\n", client->user);
                    
                    // 取得讀取鎖 (Read Lock)
                    pthread_rwlock_rdlock(&file_list[idx].lock);
                    
                    printf("[並行展示] %s 已取得讀取鎖，正在讀取... (模擬延遲 3 秒)\n", client->user);
                    sleep(3); // 故意延遲，讓助教看到多人可以同時讀，但不能寫

                    // 讀取真實檔案內容
                    FILE *fp = fopen(arg1, "r");
                    if (fp) {
                        char file_content[500];
                        fgets(file_content, 500, fp); // 讀一行當作範例
                        sprintf(response, "讀取成功: %s", file_content);
                        fclose(fp);
                    } else {
                        sprintf(response, "錯誤: 檔案讀取失敗。");
                    }

                    pthread_rwlock_unlock(&file_list[idx].lock);
                    printf("[並行展示] %s 釋放讀取鎖。\n", client->user);

                } else {
                    sprintf(response, "權限不足: 無法讀取 (Capability List 檢查失敗)。");
                }
            } else {
                sprintf(response, "錯誤: 找不到檔案。");
            }

        // === 指令: WRITE (寫入檔案 - 支援並行) ===
        } else if (strcmp(cmd, "write") == 0) {
            // arg2 是模式: o (覆蓋), a (附加)
            int idx = -1;
            for(int i=0; i<MAX_FILES; i++) {
                if (file_list[i].is_used && strcmp(file_list[i].name, arg1) == 0) {
                    idx = i; break;
                }
            }

            if (idx != -1) {
                if (check_permission(&file_list[idx], client->user, client->group, 'w')) {
                    printf("[並行展示] %s 請求寫入鎖... (其他人完全無法存取)\n", client->user);
                    
                    // 取得寫入鎖 (Write Lock) - 這是互斥的
                    pthread_rwlock_wrlock(&file_list[idx].lock);
                    
                    printf("[並行展示] %s 已取得寫入鎖，正在寫入... (模擬延遲 10 秒)\n", client->user);
                    sleep(10); // 故意長延遲，展示互斥效果

                    FILE *fp;
                    if (strcmp(arg2, "o") == 0) fp = fopen(arg1, "w"); // 覆蓋
                    else fp = fopen(arg1, "a"); // 附加

                    if (fp) {
                        fprintf(fp, "%s wrote here.\n", client->user);
                        fclose(fp);
                        sprintf(response, "寫入成功 (%s 模式)。", arg2);
                    } else {
                        sprintf(response, "錯誤: 寫入失敗。");
                    }

                    pthread_rwlock_unlock(&file_list[idx].lock);
                    printf("[並行展示] %s 釋放寫入鎖。\n", client->user);

                } else {
                    sprintf(response, "權限不足: 無法寫入 (Capability List 檢查失敗)。");
                }
            } else {
                sprintf(response, "錯誤: 找不到檔案。");
            }

        } else {
            sprintf(response, "無效指令。使用方式: new, read, write, change");
        }

        // 傳送回應給客戶端
        send(sock, response, strlen(response), 0);
    }

    close(sock);
    free(client);
    return NULL;
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    init_files();

    // 建立 Socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    // 設定 IP 和 Port
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Bind
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen
    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("伺服器啟動，等待連線中 (Port %d)...\n", PORT);

    while (1) {
        ClientInfo *new_client = malloc(sizeof(ClientInfo));
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed");
            free(new_client);
            continue;
        }

        new_client->sockfd = new_socket;
        
        // 建立新執行緒處理此客戶端
        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, (void*)new_client);
        pthread_detach(tid); // 分離執行緒，結束後自動回收資源
    }

    return 0;
}