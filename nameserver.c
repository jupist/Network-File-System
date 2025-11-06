#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>     
#include "common.h"

/*
 * --- Global Data Structures ---
 */

typedef struct {
    char username[256];
    char permission; // 'R' for Read, 'W' for Read/Write
} AccessControl;

#define MAX_PERMISSIONS 10 

typedef struct {
    char filename[256];
    char owner_username[256];        
    AccessControl acl[MAX_PERMISSIONS]; 
    int num_permissions;                

    char last_accessed_by[256]; 
    char last_accessed_ts[128]; 

    int ss_client_port;      
    char ss_ip_addr[INET_ADDRSTRLEN]; 
} FileLocation;


typedef struct {
    int ss_nm_port;          
    int ss_client_port;      
    char ss_ip_addr[INET_ADDRSTRLEN];
} StorageServer;

typedef struct {
    char username[256];
    char ip_addr[INET_ADDRSTRLEN];
    int client_socket_fd; 
} ClientInfo;

// --- **** NEW: STRUCTURE FOR FILE LOCKS **** ---
typedef struct {
    char filename[256];
    int sentence_num;
    char username[256]; // Who holds the lock
} FileLock;

#define MAX_FILES 100
#define MAX_SERVERS 10
#define MAX_CLIENTS 50 
#define MAX_LOCKS 50 // Max concurrent sentence locks
#define EXEC_OUTPUT_BUFFER_SIZE 8192 

FileLocation file_directory[MAX_FILES];
StorageServer server_list[MAX_SERVERS];
ClientInfo client_list[MAX_CLIENTS]; 
FileLock g_file_locks[MAX_LOCKS]; // NEW: Global lock list
int g_num_files = 0;
int g_num_servers = 0;
int g_num_clients = 0; 
int g_num_locks = 0; // NEW: Global lock counter

pthread_mutex_t g_system_mutex = PTHREAD_MUTEX_INITIALIZER;


// --- (All helper functions are unchanged from the previous step) ---
int forward_create_to_ss(const char* ss_ip, int ss_nm_port, const char* filename) {
    int ss_sock;
    struct sockaddr_in ss_addr;
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("NM (SS-Client): socket failed");
        return -1;
    }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_nm_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        perror("NM (SS-Client): invalid address");
        close(ss_sock);
        return -1;
    }
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("NM (SS-Client): connect to SS failed");
        close(ss_sock);
        return -1;
    }
    snprintf(command, sizeof(command), "CREATE_FILE %s\n", filename);
    if (write(ss_sock, command, strlen(command)) < 0) {
        perror("NM (SS-Client): write to SS failed");
        close(ss_sock);
        return -1;
    }
    ssize_t bytes_read = read(ss_sock, response, sizeof(response) - 1);
    close(ss_sock); 
    if (bytes_read <= 0) {
        printf("NM (SS-Client): No response from SS.\n");
        return -1;
    }
    response[bytes_read] = '\0';
    if (strncmp(response, "ACK_CREATE_SUCCESS", 18) == 0) {
        return 0; 
    } else {
        return -1; 
    }
}

int forward_delete_to_ss(const char* ss_ip, int ss_nm_port, const char* filename) {
    int ss_sock;
    struct sockaddr_in ss_addr;
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("NM (SS-Client): socket failed");
        return -1;
    }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_nm_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        perror("NM (SS-Client): invalid address");
        close(ss_sock);
        return -1;
    }
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("NM (SS-Client): connect to SS failed");
        close(ss_sock);
        return -1;
    }
    snprintf(command, sizeof(command), "DELETE_FILE %s\n", filename);
    if (write(ss_sock, command, strlen(command)) < 0) {
        perror("NM (SS-Client): write to SS failed");
        close(ss_sock);
        return -1;
    }
    ssize_t bytes_read = read(ss_sock, response, sizeof(response) - 1);
    close(ss_sock); 
    if (bytes_read <= 0) {
        printf("NM (SS-Client): No response from SS.\n");
        return -1;
    }
    response[bytes_read] = '\0';
    if (strncmp(response, "ACK_DELETE_SUCCESS", 18) == 0) {
        return 0; 
    } else {
        return -1; 
    }
}

int get_metadata_from_ss(const char* ss_ip, int ss_nm_port, const char* filename,
                         int* out_words, int* out_chars, 
                         char* out_created_ts, char* out_modified_ts, int ts_len) 
{
    int ss_sock;
    struct sockaddr_in ss_addr;
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) { /* ... */ return -1; }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_nm_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) { /* ... */ close(ss_sock); return -1; }
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) { /* ... */ close(ss_sock); return -1; }
    
    snprintf(command, sizeof(command), "GET_METADATA %s\n", filename);
    if (write(ss_sock, command, strlen(command)) < 0) { /* ... */ close(ss_sock); return -1; }
    
    ssize_t bytes_read = read(ss_sock, response, sizeof(response) - 1);
    close(ss_sock); 
    
    if (bytes_read <= 0) { /* ... */ return -1; }
    response[bytes_read] = '\0';
    
    if (strncmp(response, "METADATA_RESPONSE", 17) == 0) {
        char created_date[64], created_time[64];
        char modified_date[64], modified_time[64];
        
        int items = sscanf(response, "METADATA_RESPONSE %d %d %63s %63s %63s %63s", 
                 out_words, out_chars, 
                 created_date, created_time,
                 modified_date, modified_time);
        
        if (items != 6) { 
            printf("NM: Failed to parse metadata response: %s\n", response);
            return -1;
        }
        
        snprintf(out_created_ts, ts_len, "%s %s", created_date, created_time);
        snprintf(out_modified_ts, ts_len, "%s %s", modified_date, modified_time);
        
        return 0; // Success
    } else {
        return -1; // Failure
    }
}

int get_file_content_from_ss(const char* ss_ip, int ss_client_port, const char* filename, char* out_buffer, int out_len) {
    int ss_sock;
    struct sockaddr_in ss_addr;
    char command[BUFFER_SIZE];

    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) { 
        perror("NM (SS-Client): socket failed");
        return -1; 
    }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_client_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) { 
        perror("NM (SS-Client): invalid address");
        close(ss_sock); 
        return -1; 
    }
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) { 
        perror("NM (SS-Client): connect to SS failed");
        close(ss_sock); 
        return -1; 
    }
    
    snprintf(command, sizeof(command), "READ_FILE %s\n", filename);
    if (write(ss_sock, command, strlen(command)) < 0) { 
        perror("NM (SS-Client): write to SS failed");
        close(ss_sock); 
        return -1; 
    }

    ssize_t total_bytes_read = 0;
    ssize_t bytes_read;
    while (total_bytes_read < out_len - 1 && 
           (bytes_read = read(ss_sock, out_buffer + total_bytes_read, out_len - 1 - total_bytes_read)) > 0) {
        total_bytes_read += bytes_read;
    }
    
    out_buffer[total_bytes_read] = '\0'; // Null-terminate
    close(ss_sock);
    
    if (bytes_read < 0) {
        perror("NM (SS-Client): read file content failed");
        return -1;
    }
    
    return 0; // Success
}

int check_permission(FileLocation* file, const char* user_requesting, char permission_needed) {
    if (strcmp(file->owner_username, user_requesting) == 0) {
        return 1; 
    }
    for (int i = 0; i < file->num_permissions; i++) {
        if (strcmp(file->acl[i].username, user_requesting) == 0) {
            if (file->acl[i].permission == 'W') {
                return 1; 
            }
            if (file->acl[i].permission == 'R' && permission_needed == 'R') {
                return 1; 
            }
        }
    }
    return 0;
}


void get_current_timestamp(char* buffer, int len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, len, "%Y-%m-%d %H:%M", tm_info);
}

int forward_undo_to_ss(const char* ss_ip, int ss_nm_port, const char* filename) {
    int ss_sock;
    struct sockaddr_in ss_addr;
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("NM (SS-Client-Undo): socket failed");
        return -1;
    }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_nm_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        perror("NM (SS-Client-Undo): invalid address");
        close(ss_sock);
        return -1;
    }
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("NM (SS-Client-Undo): connect to SS failed");
        close(ss_sock);
        return -1;
    }
    
    snprintf(command, sizeof(command), "UNDO_FILE %s\n", filename);
    if (write(ss_sock, command, strlen(command)) < 0) {
        perror("NM (SS-Client-Undo): write to SS failed");
        close(ss_sock);
        return -1;
    }
    
    ssize_t bytes_read = read(ss_sock, response, sizeof(response) - 1);
    close(ss_sock); 
    
    if (bytes_read <= 0) {
        printf("NM (SS-Client-Undo): No response from SS.\n");
        return -1;
    }
    response[bytes_read] = '\0';
    
    if (strncmp(response, "ACK_UNDO_SUCCESS", 16) == 0) {
        return 0; // Success
    } else if (strncmp(response, "ACK_UNDO_FAIL_NO_BAK", 20) == 0) {
        return -2; // No backup file
    } else {
        return -1; // Other failure
    }
}

// ---------------------------------------

/*
 * This is the function that each thread will run.
 */
void* handle_connection(void* arg) {
    int client_socket = *((int*)arg);
    free(arg); 
    
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    getpeername(client_socket, (struct sockaddr*)&peer_addr, &peer_len);
    char peer_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip));

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        printf("Connection closed before identification.\n");
        close(client_socket);
        return NULL;
    }
    buffer[bytes_read] = '\0'; 

    // 3. Identify the connection type
    if (strncmp(buffer, "REGISTER_SS", 11) == 0) {
        // (This section is unchanged)
        printf("New connection from a Storage Server (%s).\n", peer_ip);
        
        int ss_nm_port, ss_client_port;
        char* token;
        char* rest = buffer;
        token = strtok_r(rest, " \n", &rest); 
        token = strtok_r(NULL, " \n", &rest); 
        ss_nm_port = token ? atoi(token) : -1;
        token = strtok_r(NULL, " \n", &rest); 
        ss_client_port = token ? atoi(token) : -1;

        if (ss_nm_port <= 0 || ss_client_port <= 0) {
            printf("Failed to parse SS registration message.\n");
            close(client_socket);
            return NULL;
        }

        pthread_mutex_lock(&g_system_mutex);
        
        if (g_num_servers < MAX_SERVERS) {
            StorageServer* new_ss = &server_list[g_num_servers++];
            new_ss->ss_nm_port = ss_nm_port;
            new_ss->ss_client_port = ss_client_port;
            strncpy(new_ss->ss_ip_addr, peer_ip, INET_ADDRSTRLEN);
            printf("  Registered SS (NM Port %d, Client Port %d)\n", ss_nm_port, ss_client_port);
        }

        while ((token = strtok_r(NULL, " \n", &rest)) != NULL) {
            if (g_num_files < MAX_FILES) {
                FileLocation* new_file = &file_directory[g_num_files++];
                strncpy(new_file->filename, token, 255);
                new_file->filename[255] = '\0';
                new_file->ss_client_port = ss_client_port;
                strncpy(new_file->ss_ip_addr, peer_ip, INET_ADDRSTRLEN);
                
                strncpy(new_file->owner_username, "system", 255); 
                new_file->num_permissions = 0; 
                strncpy(new_file->last_accessed_by, "N/A", 255);
                strncpy(new_file->last_accessed_ts, "N/A", 127);
                
                printf("  Registered file: %s\n", new_file->filename);
            }
        }
        
        pthread_mutex_unlock(&g_system_mutex);
        
        close(client_socket);
        printf("Storage Server registration complete. Connection closed.\n");
        return NULL; 
        
    } else if (strncmp(buffer, "REGISTER_CLIENT", 15) == 0) {
        // (This section is unchanged)
        printf("New connection from a Client (%s).\n", peer_ip);
        
        char username[256]; 
        if (sscanf(buffer, "REGISTER_CLIENT %s", username) != 1) {
            printf("  Failed to parse username. Closing connection.\n");
            close(client_socket);
            return NULL;
        }
        username[255] = '\0'; 

        pthread_mutex_lock(&g_system_mutex);
        if (g_num_clients < MAX_CLIENTS) {
            ClientInfo* new_client = &client_list[g_num_clients++];
            strncpy(new_client->username, username, 255);
            new_client->username[255] = '\0';
            strncpy(new_client->ip_addr, peer_ip, INET_ADDRSTRLEN);
            new_client->client_socket_fd = client_socket;
            printf("  User '%s' registered from %s.\n", username, peer_ip);
        } else {
            printf("  Max clients reached. Rejecting %s.\n", username);
        }
        pthread_mutex_unlock(&g_system_mutex);


        // 4. Enter a loop to handle commands from *this* client
        while ((bytes_read = read(client_socket, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            buffer[strcspn(buffer, "\n")] = 0; // Remove trailing newline

            if (strncmp(buffer, "VIEW", 4) == 0) {
                // (This section is unchanged)
                printf("Client requested VIEW\n");
                
                int all_flag = 0;
                int long_flag = 0;

                if (strstr(buffer, "-al") != NULL || strstr(buffer, "-la") != NULL) {
                    all_flag = 1;
                    long_flag = 1;
                } else if (strstr(buffer, "-a") != NULL) {
                    all_flag = 1;
                } else if (strstr(buffer, "-l") != NULL) {
                    long_flag = 1;
                }
                
                char response_buffer[BUFFER_SIZE] = {0};
                int offset = 0;
                int files_shown = 0;

                if (long_flag) {
                    offset += sprintf(response_buffer,
                        "| %-20s | %5s | %5s | %-16s | %-10s |\n",
                        "Filename", "Words", "Chars", "Last Access Time", "Owner");
                    offset += sprintf(response_buffer + offset,
                        "|----------------------|-------|-------|------------------|------------|\n");
                }

                pthread_mutex_lock(&g_system_mutex);

                for (int i = 0; i < g_num_files; i++) {
                    int has_permission = check_permission(&file_directory[i], username, 'R');
                    
                    if (all_flag || has_permission) {
                        if (long_flag) {
                            int words = 0, chars = 0;
                            char created_ts[128], modified_ts[128]; 
                            
                            StorageServer* ss = NULL;
                            for (int j = 0; j < g_num_servers; j++) {
                                if (strcmp(server_list[j].ss_ip_addr, file_directory[i].ss_ip_addr) == 0 &&
                                    server_list[j].ss_client_port == file_directory[i].ss_client_port) {
                                    ss = &server_list[j];
                                    break;
                                }
                            }
                            
                            if (ss != NULL) {
                                pthread_mutex_unlock(&g_system_mutex);
                                int res = get_metadata_from_ss(ss->ss_ip_addr, ss->ss_nm_port, file_directory[i].filename, 
                                                               &words, &chars, created_ts, modified_ts, 128);
                                pthread_mutex_lock(&g_system_mutex);
                                (void)res; 
                            }
                            
                            offset += sprintf(response_buffer + offset,
                                "| %-20s | %5d | %5d | %-16s | %-10s |\n",
                                file_directory[i].filename, words, chars, 
                                file_directory[i].last_accessed_ts, 
                                file_directory[i].owner_username);
                            
                        } else {
                            offset += sprintf(response_buffer + offset, "%s\n", file_directory[i].filename);
                        }
                        files_shown++;
                    }

                    if (offset > BUFFER_SIZE - 256) {
                        write(client_socket, response_buffer, offset);
                        offset = 0; 
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);
                
                if (files_shown == 0 && offset == 0) {
                    offset += sprintf(response_buffer + offset, "No files to display.\n");
                }
                
                if (offset > 0) {
                    write(client_socket, response_buffer, offset);
                }
            
            } else if (strncmp(buffer, "READ", 4) == 0) {
                // (This section is unchanged)
                char filename[256];
                if (sscanf(buffer, "READ %s", filename) != 1) {
                    const char* err_msg = "ERROR: Invalid READ format. Use: READ <filename>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                printf("Client requested READ for '%s'\n", filename);

                int found = 0;
                char ss_ip[INET_ADDRSTRLEN];
                int ss_port;
                int permitted = 0; 

                pthread_mutex_lock(&g_system_mutex);
                for (int i = 0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, filename) == 0) {
                        found = 1;
                        if (check_permission(&file_directory[i], username, 'R')) {
                            permitted = 1;
                            strncpy(ss_ip, file_directory[i].ss_ip_addr, INET_ADDRSTRLEN);
                            ss_port = file_directory[i].ss_client_port;
                            
                            get_current_timestamp(file_directory[i].last_accessed_ts, 128);
                            strncpy(file_directory[i].last_accessed_by, username, 255);
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                char response_buffer[BUFFER_SIZE];
                if (!found) {
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR: File not found.\n");
                    printf("  File not found.\n");
                } else if (!permitted) {
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR: Access Denied.\n");
                    printf("  Access Denied for user '%s'.\n", username);
                } else {
                    snprintf(response_buffer, sizeof(response_buffer), "SS_LOCATION %s %d\n", ss_ip, ss_port);
                    printf("  Access Granted. Sending location to client: %s %d\n", ss_ip, ss_port);
                }
                write(client_socket, response_buffer, strlen(response_buffer));
            
            } else if (strncmp(buffer, "STREAM", 6) == 0) {
                // (This section is unchanged)
                char filename[256];
                if (sscanf(buffer, "STREAM %s", filename) != 1) {
                    const char* err_msg = "ERROR: Invalid STREAM format. Use: STREAM <filename>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                printf("Client requested STREAM for '%s'\n", filename);

                int found = 0;
                char ss_ip[INET_ADDRSTRLEN];
                int ss_port;
                int permitted = 0; 

                pthread_mutex_lock(&g_system_mutex);
                for (int i = 0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, filename) == 0) {
                        found = 1;
                        if (check_permission(&file_directory[i], username, 'R')) {
                            permitted = 1;
                            strncpy(ss_ip, file_directory[i].ss_ip_addr, INET_ADDRSTRLEN);
                            ss_port = file_directory[i].ss_client_port;
                            
                            get_current_timestamp(file_directory[i].last_accessed_ts, 128);
                            strncpy(file_directory[i].last_accessed_by, username, 255);
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                char response_buffer[BUFFER_SIZE];
                if (!found) {
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR: File not found.\n");
                    printf("  File not found.\n");
                } else if (!permitted) {
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR: Access Denied.\n");
                    printf("  Access Denied for user '%s'.\n", username);
                } else {
                    snprintf(response_buffer, sizeof(response_buffer), "SS_LOCATION %s %d\n", ss_ip, ss_port);
                    printf("  Access Granted. Sending location to client: %s %d\n", ss_ip, ss_port);
                }
                write(client_socket, response_buffer, strlen(response_buffer));

            } else if (strncmp(buffer, "CREATE", 6) == 0) {
                // (This section is unchanged)
                char filename[256];
                if (sscanf(buffer, "CREATE %s", filename) != 1) {
                    const char* err_msg = "ERROR: Invalid CREATE format. Use: CREATE <filename>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested CREATE for '%s'\n", filename);

                int found = 0;
                pthread_mutex_lock(&g_system_mutex);
                for (int i = 0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, filename) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (found) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: File already exists.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (g_num_servers == 0) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: No Storage Servers available.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = server_list[0].ss_nm_port;
                int ss_client_port = server_list[0].ss_client_port;
                strncpy(ss_ip, server_list[0].ss_ip_addr, INET_ADDRSTRLEN);
                pthread_mutex_unlock(&g_system_mutex);

                int result = forward_create_to_ss(ss_ip, ss_nm_port, filename);

                if (result == 0) {
                    pthread_mutex_lock(&g_system_mutex);
                    if (g_num_files < MAX_FILES) {
                        FileLocation* new_file = &file_directory[g_num_files++];
                        strncpy(new_file->filename, filename, 255);
                        new_file->ss_client_port = ss_client_port;
                        strncpy(new_file->ss_ip_addr, ss_ip, INET_ADDRSTRLEN);

                        strncpy(new_file->owner_username, username, 255); 
                        new_file->num_permissions = 0; 
                        
                        strncpy(new_file->last_accessed_by, "N/A", 255);
                        strncpy(new_file->last_accessed_ts, "N/A", 127);
                        
                        printf("  Successfully registered new file '%s' for owner '%s'\n", filename, username);
                        write(client_socket, "File created successfully.\n", sizeof("File created successfully.\n") - 1);
                    } else {
                        write(client_socket, "ERROR: NM file directory is full.\n", sizeof("ERROR: NM file directory is full.\n") - 1);
                    }
                    pthread_mutex_unlock(&g_system_mutex);
                } else {
                    write(client_socket, "ERROR: Storage Server failed to create file.\n", sizeof("ERROR: Storage Server failed to create file.\n") - 1);
                }
            
            } else if (strncmp(buffer, "DELETE", 6) == 0) {
                // (This section is unchanged)
                char filename[256];
                if (sscanf(buffer, "DELETE %s", filename) != 1) {
                    const char* err_msg = "ERROR: Invalid DELETE format. Use: DELETE <filename>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested DELETE for '%s'\n", filename);

                int found_index = -1;
                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = -1;
                int permitted = 0; 

                pthread_mutex_lock(&g_system_mutex);
                for (int i = 0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, filename) == 0) {
                        found_index = i;
                        if (strcmp(file_directory[i].owner_username, username) == 0) {
                            permitted = 1;
                            for (int j = 0; j < g_num_servers; j++) {
                                if (strcmp(server_list[j].ss_ip_addr, file_directory[i].ss_ip_addr) == 0 &&
                                    server_list[j].ss_client_port == file_directory[i].ss_client_port) 
                                {
                                    strncpy(ss_ip, server_list[j].ss_ip_addr, INET_ADDRSTRLEN);
                                    ss_nm_port = server_list[j].ss_nm_port;
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                if (found_index == -1) {
                    const char* err_msg = "ERROR: File not found.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                if (!permitted) {
                    const char* err_msg = "ERROR: Access Denied. Only the owner can delete a file.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    printf("  Access Denied for user '%s'. Not owner.\n", username);
                    continue;
                }

                if (ss_nm_port == -1) {
                    const char* err_msg = "ERROR: Could not find SS for file. Directory out of sync.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                int result = forward_delete_to_ss(ss_ip, ss_nm_port, filename);

                if (result == 0) {
                    pthread_mutex_lock(&g_system_mutex);
                    printf("  Deleting file at index %d\n", found_index);
                    for (int i = found_index; i < g_num_files - 1; i++) {
                        file_directory[i] = file_directory[i + 1];
                    }
                    g_num_files--;
                    
                    printf("  Successfully deleted file '%s' from directory.\n", filename);
                    write(client_socket, "File deleted successfully.\n", sizeof("File deleted successfully.\n") - 1);
                    pthread_mutex_unlock(&g_system_mutex);
                } else {
                    write(client_socket, "ERROR: Storage Server failed to delete file.\n", sizeof("ERROR: Storage Server failed to delete file.\n") - 1);
                }

            } else if (strncmp(buffer, "LIST", 4) == 0) {
                // (This section is unchanged)
                printf("Client requested LIST\n");
                
                char response_buffer[BUFFER_SIZE] = {0};
                int offset = 0;

                pthread_mutex_lock(&g_system_mutex);
                
                if (g_num_clients == 0) {
                    offset += sprintf(response_buffer + offset, "No users connected.\n");
                } else {
                    offset += sprintf(response_buffer + offset, "Connected Users:\n");
                    for (int i = 0; i < g_num_clients; i++) {
                        offset += sprintf(response_buffer + offset, "-> %s\n", client_list[i].username);
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);
                
                write(client_socket, response_buffer, strlen(response_buffer));

            } else if (strncmp(buffer, "ADDACCESS", 9) == 0) {
                // (This section is unchanged)
                char perm_char_str[2], target_filename[256], target_username[256];
                if (sscanf(buffer, "ADDACCESS %1s %s %s", perm_char_str, target_filename, target_username) != 3) {
                    const char* err_msg = "ERROR: Invalid format. Use: ADDACCESS <R|W> <filename> <username>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                char perm = perm_char_str[0];
                if (perm != 'R' && perm != 'W') {
                    const char* err_msg = "ERROR: Permission must be 'R' or 'W'.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                printf("User '%s' requested ADDACCESS %c for '%s' to user '%s'\n", username, perm, target_filename, target_username);

                pthread_mutex_lock(&g_system_mutex);
                int found_index = -1;
                for(int i=0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, target_filename) == 0) {
                        found_index = i;
                        break;
                    }
                }

                if (found_index == -1) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: File not found.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (strcmp(file_directory[found_index].owner_username, username) != 0) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: You are not the owner of this file.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (file_directory[found_index].num_permissions < MAX_PERMISSIONS) {
                    AccessControl* new_perm = &file_directory[found_index].acl[file_directory[found_index].num_permissions++];
                    strncpy(new_perm->username, target_username, 255);
                    new_perm->permission = perm;
                    printf("  Access granted.\n");
                    write(client_socket, "Access granted successfully.\n", sizeof("Access granted successfully.\n") - 1);
                } else {
                    const char* err_msg = "ERROR: File has reached its maximum permission entries.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                }
                pthread_mutex_unlock(&g_system_mutex);

            } else if (strncmp(buffer, "REMACCESS", 9) == 0) {
                // (This section is unchanged)
                char target_filename[256], target_username[256];
                if (sscanf(buffer, "REMACCESS %s %s", target_filename, target_username) != 2) {
                    const char* err_msg = "ERROR: Invalid format. Use: REMACCESS <filename> <username>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                printf("User '%s' requested REMACCESS for '%s' from user '%s'\n", username, target_filename, target_username);

                pthread_mutex_lock(&g_system_mutex);
                int file_index = -1;
                for(int i=0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, target_filename) == 0) {
                        file_index = i;
                        break;
                    }
                }

                if (file_index == -1) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: File not found.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (strcmp(file_directory[file_index].owner_username, username) != 0) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: You are not the owner of this file.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                int perm_index = -1;
                for (int i = 0; i < file_directory[file_index].num_permissions; i++) {
                    if (strcmp(file_directory[file_index].acl[i].username, target_username) == 0) {
                        perm_index = i;
                        break;
                    }
                }

                if (perm_index == -1) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: That user does not have special permissions on this file.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                for (int i = perm_index; i < file_directory[file_index].num_permissions - 1; i++) {
                    file_directory[file_index].acl[i] = file_directory[file_index].acl[i + 1];
                }
                file_directory[file_index].num_permissions--;
                
                printf("  Access removed.\n");
                write(client_socket, "Access removed successfully.\n", sizeof("Access removed successfully.\n") - 1);
                pthread_mutex_unlock(&g_system_mutex);
            
            } else if (strncmp(buffer, "INFO", 4) == 0) {
                // (This section is unchanged)
                char filename[256];
                if (sscanf(buffer, "INFO %s", filename) != 1) {
                    const char* err_msg = "ERROR: Invalid INFO format. Use: INFO <filename>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested INFO for '%s'\n", filename);

                int found_index = -1;
                int permitted = 0;
                char response_buffer[BUFFER_SIZE] = {0};
                int offset = 0;

                pthread_mutex_lock(&g_system_mutex);
                for (int i = 0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, filename) == 0) {
                        found_index = i;
                        if (check_permission(&file_directory[i], username, 'R')) {
                            permitted = 1;
                        }
                        break;
                    }
                }

                if (found_index == -1) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: File not found.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (!permitted) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: Access Denied.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    printf("  Access Denied for user '%s'.\n", username);
                    continue;
                }

                FileLocation file_copy = file_directory[found_index];
                StorageServer* ss = NULL;
                for (int j = 0; j < g_num_servers; j++) {
                    if (strcmp(server_list[j].ss_ip_addr, file_copy.ss_ip_addr) == 0 &&
                        server_list[j].ss_client_port == file_copy.ss_client_port) {
                        ss = &server_list[j];
                        break;
                    }
                }
                
                if (ss == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    const char* err_msg = "ERROR: Could not find SS for file. Directory out of sync.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                char ss_ip[INET_ADDRSTRLEN];
                strncpy(ss_ip, ss->ss_ip_addr, INET_ADDRSTRLEN);
                int ss_nm_port = ss->ss_nm_port;
                
                pthread_mutex_unlock(&g_system_mutex);
                
                int words = 0, chars = 0;
                char created_ts[128], modified_ts[128]; 

                if (get_metadata_from_ss(ss_ip, ss_nm_port, file_copy.filename, &words, &chars, created_ts, modified_ts, 128) != 0) {
                    const char* err_msg = "ERROR: Failed to retrieve file metadata from Storage Server.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                offset += sprintf(response_buffer + offset, "--> File: %s\n", file_copy.filename);
                offset += sprintf(response_buffer + offset, "--> Owner: %s\n", file_copy.owner_username);
                offset += sprintf(response_buffer + offset, "--> Created: %s\n", created_ts);
                offset += sprintf(response_buffer + offset, "--> Last Modified: %s\n", modified_ts);
                offset += sprintf(response_buffer + offset, "--> Size: %d bytes\n", chars); 
                
                offset += sprintf(response_buffer + offset, "--> Access: %s (RW)\n", file_copy.owner_username);
                for (int i = 0; i < file_copy.num_permissions; i++) {
                    offset += sprintf(response_buffer + offset, "-->          %s (%s)\n", 
                                      file_copy.acl[i].username, 
                                      file_copy.acl[i].permission == 'W' ? "RW" : "R"); 
                }
                
                offset += sprintf(response_buffer + offset, "--> Last Accessed: %s by %s\n", file_copy.last_accessed_ts, file_copy.last_accessed_by); 

                write(client_socket, response_buffer, offset);
            
            } else if (strncmp(buffer, "EXEC", 4) == 0) {
                // (This section is unchanged)
                char filename[256];
                if (sscanf(buffer, "EXEC %s", filename) != 1) {
                    const char* err_msg = "ERROR: Invalid EXEC format. Use: EXEC <filename>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested EXEC for '%s'\n", filename);

                int found_index = -1;
                int permitted = 0;
                FileLocation file_copy;
                StorageServer* ss = NULL;

                pthread_mutex_lock(&g_system_mutex);
                for (int i = 0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, filename) == 0) {
                        found_index = i;
                        if (check_permission(&file_directory[i], username, 'R')) {
                            permitted = 1;
                            file_copy = file_directory[i]; 
                            for (int j = 0; j < g_num_servers; j++) {
                                if (strcmp(server_list[j].ss_ip_addr, file_copy.ss_ip_addr) == 0 &&
                                    server_list[j].ss_client_port == file_copy.ss_client_port) {
                                    ss = &server_list[j];
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                if (found_index == -1) {
                    const char* err_msg = "ERROR: File not found.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                if (!permitted) {
                    const char* err_msg = "ERROR: Access Denied.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                if (ss == NULL) {
                    const char* err_msg = "ERROR: Could not find SS for file. Directory out of sync.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                char* file_content = malloc(EXEC_OUTPUT_BUFFER_SIZE);
                if (file_content == NULL) {
                    const char* err_msg = "ERROR: NM failed to allocate memory for exec.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                if (get_file_content_from_ss(ss->ss_ip_addr, ss->ss_client_port, file_copy.filename, file_content, EXEC_OUTPUT_BUFFER_SIZE) != 0) {
                    const char* err_msg = "ERROR: NM failed to fetch file from SS.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    free(file_content);
                    continue;
                }
                
                printf("  Executing content:\n%s\n", file_content);
                FILE* pipe = popen(file_content, "r");
                free(file_content); 

                if (pipe == NULL) {
                    const char* err_msg = "ERROR: NM failed to execute command.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                char* output_buffer = malloc(EXEC_OUTPUT_BUFFER_SIZE);
                if (output_buffer == NULL) {
                    const char* err_msg = "ERROR: NM failed to allocate memory for output.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    pclose(pipe);
                    continue;
                }
                
                ssize_t output_size = fread(output_buffer, 1, EXEC_OUTPUT_BUFFER_SIZE - 1, pipe);
                output_buffer[output_size] = '\0';
                pclose(pipe);

                printf("  Sending output to client:\n%s\n", output_buffer);
                write(client_socket, output_buffer, output_size);
                free(output_buffer);

            // --- **** NEW: UNDO Logic **** ---
            } else if (strncmp(buffer, "UNDO", 4) == 0) {
                char filename[256];
                if (sscanf(buffer, "UNDO %s", filename) != 1) {
                    const char* err_msg = "ERROR: Invalid UNDO format. Use: UNDO <filename>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested UNDO for '%s'\n", filename);

                int found_index = -1;
                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = -1;
                int permitted = 0; 

                pthread_mutex_lock(&g_system_mutex);
                for (int i = 0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, filename) == 0) {
                        found_index = i;
                        // Per spec, "user B can also do it", but this is a
                        // file modification. We'll require Write permission.
                        if (check_permission(&file_directory[i], username, 'W')) {
                            permitted = 1;
                            // Find the SS
                            for (int j = 0; j < g_num_servers; j++) {
                                if (strcmp(server_list[j].ss_ip_addr, file_directory[i].ss_ip_addr) == 0 &&
                                    server_list[j].ss_client_port == file_directory[i].ss_client_port) 
                                {
                                    strncpy(ss_ip, server_list[j].ss_ip_addr, INET_ADDRSTRLEN);
                                    ss_nm_port = server_list[j].ss_nm_port;
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                if (found_index == -1) {
                    const char* err_msg = "ERROR: File not found.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                if (!permitted) {
                    const char* err_msg = "ERROR: Access Denied (Write permission required to undo).\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                if (ss_nm_port == -1) {
                    const char* err_msg = "ERROR: Could not find SS for file. Directory out of sync.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                // Forward the command to the SS
                int result = forward_undo_to_ss(ss_ip, ss_nm_port, filename);

                if (result == 0) {
                    write(client_socket, "Undo Successful!\n", 17);
                } else if (result == -2) { // Special code for "No backup"
                    write(client_socket, "ERROR: No undo history found for this file.\n", 44);
                } else {
                    write(client_socket, "ERROR: Storage Server failed to undo file.\n", 43);
                }
            // --- **** END OF NEW BLOCK **** ---
            
            } else if (strncmp(buffer, "WRITE", 5) == 0) {
                // (This section is unchanged)
                char filename[256];
                int sentence_num;
                if (sscanf(buffer, "WRITE %s %d", filename, &sentence_num) != 2) {
                    const char* err_msg = "ERROR: Invalid WRITE format. Use: WRITE <filename> <sentence_num>\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested WRITE for '%s', sentence %d\n", filename, sentence_num);

                int found = 0;
                char ss_ip[INET_ADDRSTRLEN];
                int ss_port;
                int permitted = 0;
                int already_locked = 0;
                char locking_user[256];

                pthread_mutex_lock(&g_system_mutex);
                for (int i = 0; i < g_num_files; i++) {
                    if (strcmp(file_directory[i].filename, filename) == 0) {
                        found = 1;
                        if (check_permission(&file_directory[i], username, 'W')) {
                            permitted = 1;
                            
                            // Check for existing lock
                            for (int j = 0; j < g_num_locks; j++) {
                                if (strcmp(g_file_locks[j].filename, filename) == 0 && g_file_locks[j].sentence_num == sentence_num) {
                                    already_locked = 1;
                                    strncpy(locking_user, g_file_locks[j].username, 255);
                                    break;
                                }
                            }

                            if (!already_locked && g_num_locks < MAX_LOCKS) {
                                // Grant lock
                                FileLock* new_lock = &g_file_locks[g_num_locks++];
                                strncpy(new_lock->filename, filename, 255);
                                strncpy(new_lock->username, username, 255);
                                new_lock->sentence_num = sentence_num;
                                
                                // Get SS location
                                strncpy(ss_ip, file_directory[i].ss_ip_addr, INET_ADDRSTRLEN);
                                ss_port = file_directory[i].ss_client_port;
                                printf("  Lock granted to '%s'\n", username);
                            } else if (already_locked) {
                                // Lock is held
                            } else {
                                // Too many locks
                                already_locked = -1; // Use -1 to signal max locks
                            }
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                char response_buffer[BUFFER_SIZE];
                if (!found) {
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR: File not found.\n");
                } else if (!permitted) {
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR: Access Denied (Write permission required).\n");
                } else if (already_locked == 1) {
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR: Sentence is currently locked by '%s'.\n", locking_user);
                } else if (already_locked == -1) {
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR: System is at maximum lock capacity. Try again later.\n");
                } else {
                    // Success!
                    snprintf(response_buffer, sizeof(response_buffer), "SS_LOCATION %s %d\n", ss_ip, ss_port);
                }
                write(client_socket, response_buffer, strlen(response_buffer));
            
            } else if (strncmp(buffer, "RELEASE_LOCK", 12) == 0) {
                // (This section is unchanged)
                char filename[256];
                int sentence_num;
                if (sscanf(buffer, "RELEASE_LOCK %s %d", filename, &sentence_num) != 2) {
                    const char* err_msg = "ERROR: Invalid RELEASE_LOCK format.\n";
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                printf("Client requested RELEASE_LOCK for '%s', sentence %d\n", filename, sentence_num);
                pthread_mutex_lock(&g_system_mutex);
                int lock_index = -1;
                for (int i = 0; i < g_num_locks; i++) {
                    if (strcmp(g_file_locks[i].filename, filename) == 0 &&
                        g_file_locks[i].sentence_num == sentence_num &&
                        strcmp(g_file_locks[i].username, username) == 0) // Only owner can release
                    {
                        lock_index = i;
                        break;
                    }
                }

                if (lock_index != -1) {
                    // Remove lock by shifting
                    for (int i = lock_index; i < g_num_locks - 1; i++) {
                        g_file_locks[i] = g_file_locks[i + 1];
                    }
                    g_num_locks--;
                    printf("  Lock released.\n");
                    write(client_socket, "ACK_LOCK_RELEASED\n", 18);
                } else {
                    printf("  Invalid lock release request.\n");
                    write(client_socket, "ERROR: You do not hold that lock.\n", 34);
                }
                pthread_mutex_unlock(&g_system_mutex);

            } else {
                const char* ack = "ERROR: Unknown command.\n";
                write(client_socket, ack, strlen(ack));
            }
        }
    } else {
        printf("Unknown connection type. Closing.\n");
    }

    // --- (Unchanged: Remove client on disconnect) ---
    pthread_mutex_lock(&g_system_mutex);
    int client_index = -1;
    for (int i = 0; i < g_num_clients; i++) {
        if (client_list[i].client_socket_fd == client_socket) {
            client_index = i;
            break;
        }
    }

    if (client_index != -1) {
        printf("User '%s' disconnected. Removing from list.\n", client_list[client_index].username);
        // --- **** FIX: Also release any locks held by this user **** ---
        for (int i = g_num_locks - 1; i >= 0; i--) { // Iterate backwards
            if (strcmp(g_file_locks[i].username, client_list[client_index].username) == 0) {
                printf("  User disconnected, releasing lock on %s (sent %d)\n", g_file_locks[i].filename, g_file_locks[i].sentence_num);
                // Remove lock by shifting
                for (int j = i; j < g_num_locks - 1; j++) {
                    g_file_locks[j] = g_file_locks[j + 1];
                }
                g_num_locks--;
            }
        }
        // --- **** END OF FIX **** ---
        
        for (int i = client_index; i < g_num_clients - 1; i++) {
            client_list[i] = client_list[i + 1];
        }
        g_num_clients--;
    }
    pthread_mutex_unlock(&g_system_mutex);

    // 5. Clean up
    close(client_socket);
    printf("Connection closed.\n");
    return NULL;
}

/*
 * --- Main Server Function ---
 */
int main() {
    // (Unchanged)
    int server_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; 
    server_addr.sin_port = htons(NAME_SERVER_PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) { 
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Name Server listening on port %d\n", NAME_SERVER_PORT);

    while (1) {
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            perror("accept failed");
            continue; 
        }

        printf("Accepted new connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        pthread_t thread_id;
        int* p_client_socket = malloc(sizeof(int));
        *p_client_socket = client_socket;

        if (pthread_create(&thread_id, NULL, handle_connection, (void*)p_client_socket) != 0) {
            perror("pthread_create failed");
            free(p_client_socket);
            close(client_socket);
        }
    }

    close(server_fd);
    return 0;
}