/*
 * server.c - 檔案伺服器 v3
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
#define MAX_FILES 20
#define BUFFER_SIZE 1024

// 檔案資訊結構
typedef struct {
    char name[50];
    char owner[50];
    char group[50];
    char perms[7];
    int is_used;
    pthread_rwlock_t lock;
} FileEntry;

FileEntry file_list[MAX_FILES];
pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int sockfd;
    char user[50];
    char group[50];
} ClientInfo;

void init_files() {
    for(int i=0; i<MAX_FILES; i++) {
        file_list[i].is_used = 0;
        pthread_rwlock_init(&file_list[i].lock, NULL);
    }
}

void print_capability_lists() {
    printf("\n=== Capability List (Server Status) ===\n");
    printf("%-20s %-10s %-10s %-10s\n", "檔名", "擁有者", "群組", "權限");
    for(int i=0; i<MAX_FILES; i++) {
        if(file_list[i].is_used) {
            printf("%-20s %-10s %-10s %-10s\n", 
                file_list[i].name, file_list[i].owner, file_list[i].group, file_list[i].perms);
        }
    }
    printf("=======================================\n\n");
}

int check_perm_format(const char *perms) {
    if (strlen(perms) != 6) return 0;
    for (int i = 0; i < 6; i++) {
        if (i % 2 == 0) {
            if (perms[i] != 'r' && perms[i] != 'n') return 0;
        } else {
            if (perms[i] != 'w' && perms[i] != 'n') return 0;
        }
    }
    return 1;
}

int check_permission(FileEntry *file, char *user, char *group, char mode) {
    int offset = 4;
    if (strcmp(file->owner, user) == 0) offset = 0;
    else if (strcmp(file->group, group) == 0) offset = 2;

    char p_target = (mode == 'r') ? file->perms[offset] : file->perms[offset + 1];
    
    if (mode == 'r' && p_target == 'r') return 1;
    if (mode == 'w' && p_target == 'w') return 1;
    return 0;
}

void *client_handler(void *arg) {
    ClientInfo *client = (ClientInfo *)arg;
    int sock = client->sockfd;
    char buffer[BUFFER_SIZE];
    int n;

    // 1. 接收登入資訊
    memset(buffer, 0, BUFFER_SIZE);
    if (recv(sock, buffer, BUFFER_SIZE, 0) <= 0) {
        close(sock); free(client); return NULL;
    }
    sscanf(buffer, "%s %s", client->user, client->group);

    // [新增] 檢查群組名稱是否有在允許清單中
    if (strcmp(client->group, "AOS-group") != 0 && strcmp(client->group, "CSE-group") != 0) {
        printf("登入失敗: %s 使用了無效群組 %s\n", client->user, client->group);
        char *error_msg = "Login Failed: Invalid Group. Only 'AOS-group' or 'CSE-group' allowed.";
        send(sock, error_msg, strlen(error_msg), 0);
        
        // 群組錯誤直接斷線
        close(sock);
        free(client);
        return NULL; 
    }

    // 登入成功
    printf("客戶端登入成功: 使用者=%s, 群組=%s\n", client->user, client->group);
    send(sock, "Login OK", 8, 0);

    // 2. 指令處理迴圈
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        n = recv(sock, buffer, BUFFER_SIZE, 0);
        if (n <= 0) break;

        char cmd[10], arg1[50], arg2[50];
        memset(cmd, 0, sizeof(cmd));
        memset(arg1, 0, sizeof(arg1));
        memset(arg2, 0, sizeof(arg2));
        
        sscanf(buffer, "%s %s %s", cmd, arg1, arg2);
        char response[BUFFER_SIZE];
        memset(response, 0, BUFFER_SIZE);

        if (strcmp(cmd, "new") == 0) {
            if (!check_perm_format(arg2)) {
                sprintf(response, "錯誤: 權限格式錯誤 (必須是6碼，例如 rwrnnn)");
            } else {
                pthread_mutex_lock(&list_mutex);
                int idx = -1;
                for(int i=0; i<MAX_FILES; i++) if(!file_list[i].is_used) { idx = i; break; }
                
                if (idx != -1) {
                    strcpy(file_list[idx].name, arg1);
                    strcpy(file_list[idx].perms, arg2);
                    strcpy(file_list[idx].owner, client->user);
                    strcpy(file_list[idx].group, client->group);
                    file_list[idx].is_used = 1;
                    
                    FILE *fp = fopen(arg1, "w");
                    if(fp) { fprintf(fp, "Init file: %s\n", arg1); fclose(fp); }
                    
                    sprintf(response, "檔案 %s 建立成功。", arg1);
                    print_capability_lists();
                } else {
                    sprintf(response, "錯誤: 空間已滿。");
                }
                pthread_mutex_unlock(&list_mutex);
            }
        
        } else if (strcmp(cmd, "change") == 0) {
            if (!check_perm_format(arg2)) {
                sprintf(response, "錯誤: 權限格式錯誤 (必須是6碼，例如 rwrnnn)");
            } else {
                int found = 0;
                for(int i=0; i<MAX_FILES; i++) {
                    if (file_list[i].is_used && strcmp(file_list[i].name, arg1) == 0) {
                        if (strcmp(file_list[i].owner, client->user) == 0) {
                            strcpy(file_list[i].perms, arg2);
                            sprintf(response, "檔案 %s 權限已變更。", arg1);
                            print_capability_lists();
                        } else {
                            sprintf(response, "錯誤: 你不是擁有者。");
                        }
                        found = 1; break;
                    }
                }
                if (!found) sprintf(response, "錯誤: 找不到檔案。");
            }

        } else if (strcmp(cmd, "read") == 0) {
            int idx = -1;
            for(int i=0; i<MAX_FILES; i++) {
                if (file_list[i].is_used && strcmp(file_list[i].name, arg1) == 0) { idx = i; break; }
            }

            if (idx != -1) {
                if (check_permission(&file_list[idx], client->user, client->group, 'r')) {
                    pthread_rwlock_rdlock(&file_list[idx].lock);
                    printf("[Read] %s 正在讀取... (模擬延遲)\n", client->user);
                    sleep(3); 

                    FILE *fp = fopen(arg1, "r");
                    if (fp) {
                        char content[500];
                        fgets(content, 500, fp);
                        sprintf(response, "讀取內容: %s", content);
                        fclose(fp);
                    } else {
                        sprintf(response, "錯誤: 讀取失敗。");
                    }
                    pthread_rwlock_unlock(&file_list[idx].lock);
                } else {
                    sprintf(response, "權限不足: 無法讀取。");
                }
            } else {
                sprintf(response, "錯誤: 找不到檔案。");
            }

        } else if (strcmp(cmd, "write") == 0) {
            int idx = -1;
            for(int i=0; i<MAX_FILES; i++) {
                if (file_list[i].is_used && strcmp(file_list[i].name, arg1) == 0) { idx = i; break; }
            }

            if (idx != -1) {
                if (check_permission(&file_list[idx], client->user, client->group, 'w')) {
                    pthread_rwlock_wrlock(&file_list[idx].lock);
                    printf("[Write] %s 正在寫入... (模擬延遲)\n", client->user);
                    sleep(8); 

                    FILE *fp;
                    if (strcmp(arg2, "o") == 0) fp = fopen(arg1, "w");
                    else fp = fopen(arg1, "a");

                    if (fp) {
                        time_t now = time(NULL);
                        struct tm *t = localtime(&now);
                        char time_str[64];
                        strftime(time_str, sizeof(time_str), "%Y/%m/%d-%H:%M:%S", t);
                        
                        fprintf(fp, "%s wrote here at %s\n", client->user, time_str);
                        fclose(fp);
                        sprintf(response, "寫入成功 (時間: %s)。", time_str);
                    } else {
                        sprintf(response, "錯誤: 寫入失敗。");
                    }
                    pthread_rwlock_unlock(&file_list[idx].lock);
                } else {
                    sprintf(response, "權限不足: 無法寫入。");
                }
            } else {
                sprintf(response, "錯誤: 找不到檔案。");
            }
        } else {
            sprintf(response, "無效指令。");
        }
        send(sock, response, strlen(response), 0);
    }
    close(sock); free(client); return NULL;
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    init_files();

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) { perror("Socket failed"); exit(1); }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) { perror("Bind failed"); exit(1); }
    if (listen(server_fd, 5) < 0) { perror("Listen failed"); exit(1); }

    printf("伺服器啟動 (Port %d)...\n", PORT);

    while (1) {
        ClientInfo *new_client = malloc(sizeof(ClientInfo));
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed"); free(new_client); continue;
        }
        new_client->sockfd = new_socket;
        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, (void*)new_client);
        pthread_detach(tid);
    }
    return 0;
}