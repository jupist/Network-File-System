#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>     
#include <stdarg.h>  // For va_list
#include "common.h"

// Include modular headers
#include "nm_modules/nm_types.h"
#include "nm_modules/nm_hashmap.h"
#include "nm_modules/nm_cache.h"
#include "nm_modules/nm_logging.h"
#include "nm_modules/nm_persistence.h"

/*
 * --- Global Variables ---
 * (Type definitions are now in nm_modules/nm_types.h)
 */

// --- **** Global Variables **** ---
HashNode* g_file_hash_map[HASH_MAP_SIZE]; // The main file directory
StorageServer server_list[MAX_SERVERS];
ClientInfo client_list[MAX_CLIENTS]; 
FileLock g_file_locks[MAX_LOCKS];
FolderNode* g_folder_list = NULL; // Linked list of folders
AccessRequest g_access_requests[MAX_ACCESS_REQUESTS];
int g_num_servers = 0;
int g_num_clients = 0; 
int g_num_locks = 0;
int g_num_folders = 0;
int g_num_access_requests = 0;
int g_next_ss_for_file = 0; // Round-robin counter for file distribution

// --- **** NEW: LRU CACHE GLOBALS **** ---
CacheMapEntry* g_cache_map[CACHE_MAP_SIZE];
CacheNode* g_cache_head = NULL;
CacheNode* g_cache_tail = NULL;
int g_cache_size = 0;

// --- **** Global Mutexes **** ---
pthread_mutex_t g_system_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER; 


// --- (Hash map, cache, logging, and persistence functions are now in nm_modules/) ---

// --- (SS Forwarding Helper Functions - UNCHANGED) ---

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

/*
 * Asynchronously replicate a command to the replica SS (if it exists and is online).
 * This function sends the command but does not wait for a response.
 */
void replicate_to_backup_ss(int primary_ss_index, const char* command) {
    pthread_mutex_lock(&g_system_mutex);
    
    int replica_index = server_list[primary_ss_index].replicated_by;
    if (replica_index < 0 || replica_index >= g_num_servers) {
        pthread_mutex_unlock(&g_system_mutex);
        return; // No replica configured
    }
    
    StorageServer* replica_ss = &server_list[replica_index];
    if (!replica_ss->is_online) {
        pthread_mutex_unlock(&g_system_mutex);
        printf("  Replica SS%d is offline, skipping replication\n", replica_index);
        return; // Replica is offline
    }
    
    char replica_ip[INET_ADDRSTRLEN];
    int replica_nm_port = replica_ss->ss_nm_port;
    strncpy(replica_ip, replica_ss->ss_ip_addr, INET_ADDRSTRLEN);
    
    pthread_mutex_unlock(&g_system_mutex);
    
    // Send command asynchronously (don't wait for response)
    int ss_sock;
    struct sockaddr_in ss_addr;
    
    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        return;
    }
    
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(replica_nm_port);
    if (inet_pton(AF_INET, replica_ip, &ss_addr.sin_addr) <= 0) {
        close(ss_sock);
        return;
    }
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        close(ss_sock);
        return;
    }
    
    write(ss_sock, command, strlen(command));
    close(ss_sock); // Don't wait for response
    
    printf("  Replicated command to SS%d: %s", replica_index, command);
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

// --- (Permission & Time Helpers - UNCHANGED) ---

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

// --- Folder Helper Functions ---

// Forward folder creation to storage server
int forward_create_folder_to_ss(const char* ss_ip, int ss_nm_port, const char* folder_path) {
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
    snprintf(command, sizeof(command), "CREATE_FOLDER %s\n", folder_path);
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
    if (strncmp(response, "ACK_FOLDER_SUCCESS", 18) == 0) {
        return 0; 
    } else {
        return -1; 
    }
}

// Forward file move to storage server
int forward_move_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, const char* folder_path) {
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
    snprintf(command, sizeof(command), "MOVE_FILE %s %s\n", filename, folder_path);
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
    if (strncmp(response, "ACK_MOVE_SUCCESS", 16) == 0) {
        return 0; 
    } else {
        return -1; 
    }
}

// Get folder listing from storage server
int get_folder_listing_from_ss(const char* ss_ip, int ss_nm_port, const char* folder_path, char* out_buffer, int out_len) {
    int ss_sock;
    struct sockaddr_in ss_addr;
    char command[BUFFER_SIZE];

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
    
    snprintf(command, sizeof(command), "VIEW_FOLDER %s\n", folder_path);
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
    
    out_buffer[total_bytes_read] = '\0';
    close(ss_sock);
    
    if (bytes_read < 0) {
        perror("NM (SS-Client): read folder listing failed");
        return -1;
    }
    
    return 0;
}

// Forward CHECKPOINT command to storage server
int forward_checkpoint_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, const char* checkpoint_tag, char* out_response, int out_len) {
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
    
    snprintf(command, sizeof(command), "CHECKPOINT %s %s\n", filename, checkpoint_tag);
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
    
    if (out_response) {
        snprintf(out_response, out_len, "%s", response);
    }
    
    if (strncmp(response, "ACK_CHECKPOINT_SUCCESS", 22) == 0) {
        return 0;
    } else {
        return -1;
    }
}

// Forward LISTCHECKPOINTS command to storage server
int forward_listcheckpoints_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, char* out_buffer, int out_len) {
    int ss_sock;
    struct sockaddr_in ss_addr;
    char command[BUFFER_SIZE];

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
    
    snprintf(command, sizeof(command), "LISTCHECKPOINTS %s\n", filename);
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
    
    out_buffer[total_bytes_read] = '\0';
    close(ss_sock);
    
    if (bytes_read < 0) {
        perror("NM (SS-Client): read checkpoint list failed");
        return -1;
    }
    
    return 0;
}

// Forward VIEWCHECKPOINT command to storage server
int forward_viewcheckpoint_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, const char* checkpoint_tag, char* out_buffer, int out_len) {
    int ss_sock;
    struct sockaddr_in ss_addr;
    char command[BUFFER_SIZE];

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
    
    snprintf(command, sizeof(command), "VIEWCHECKPOINT %s %s\n", filename, checkpoint_tag);
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
    
    out_buffer[total_bytes_read] = '\0';
    close(ss_sock);
    
    if (bytes_read < 0) {
        perror("NM (SS-Client): read checkpoint content failed");
        return -1;
    }
    
    return 0;
}

// Forward REVERT command to storage server
int forward_revert_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, const char* checkpoint_tag, char* out_response, int out_len) {
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
    
    snprintf(command, sizeof(command), "REVERT %s %s\n", filename, checkpoint_tag);
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
    
    if (out_response) {
        snprintf(out_response, out_len, "%s", response);
    }
    
    if (strncmp(response, "ACK_REVERT_SUCCESS", 18) == 0) {
        return 0;
    } else {
        return -1;
    }
}

// Find folder in folder list
FolderNode* find_folder(const char* folder_path) {
    FolderNode* current = g_folder_list;
    while (current != NULL) {
        if (strcmp(current->folder_path, folder_path) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Add folder to folder list (unsafe - must hold g_system_mutex)
void add_folder_unsafe(const char* foldername, const char* folder_path, const char* owner) {
    FolderNode* new_folder = (FolderNode*)malloc(sizeof(FolderNode));
    strncpy(new_folder->foldername, foldername, 255);
    new_folder->foldername[255] = '\0';
    strncpy(new_folder->folder_path, folder_path, 511);
    new_folder->folder_path[511] = '\0';
    strncpy(new_folder->owner_username, owner, 255);
    new_folder->owner_username[255] = '\0';
    
    new_folder->next = g_folder_list;
    g_folder_list = new_folder;
    g_num_folders++;
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
    int peer_port = ntohs(peer_addr.sin_port);

    char client_addr_str[INET_ADDRSTRLEN + 7]; // IP + ':' + 5-digit port + null
    snprintf(client_addr_str, sizeof(client_addr_str), "%s:%d", peer_ip, peer_port);
    
    char err_msg[256]; // Buffer for sending error messages
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        printf("Connection from %s closed before identification.\n", client_addr_str);
        close(client_socket);
        return NULL;
    }
    buffer[bytes_read] = '\0'; 

    // 3. Identify the connection type
    if (strncmp(buffer, "REGISTER_SS", 11) == 0) {
        printf("New connection from a Storage Server (%s).\n", client_addr_str);
        log_to_file(client_addr_str, "StorageServer", "REQ: REGISTER_SS");
        
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
            log_to_file(client_addr_str, "StorageServer", "RES: Failed to parse SS registration message.");
            close(client_socket);
            return NULL;
        }

        pthread_mutex_lock(&g_system_mutex);
        
        // Check if this SS was previously registered (reconnection scenario)
        int existing_ss_index = -1;
        for (int i = 0; i < g_num_servers; i++) {
            if (server_list[i].ss_nm_port == ss_nm_port && 
                strcmp(server_list[i].ss_ip_addr, peer_ip) == 0) {
                existing_ss_index = i;
                break;
            }
        }
        
        if (existing_ss_index >= 0) {
            // SS is reconnecting - mark as online and update heartbeat
            StorageServer* ss = &server_list[existing_ss_index];
            ss->is_online = 1;
            ss->last_heartbeat = time(NULL);
            ss->ss_client_port = ss_client_port; // Update client port in case it changed
            
            printf("  SS%d reconnected (NM Port %d, Client Port %d)\n", existing_ss_index, ss_nm_port, ss_client_port);
            log_to_file(client_addr_str, "StorageServer", "INFO: SS%d reconnected. Triggering resync...", existing_ss_index);
            
            // TODO: Trigger resync from replica SS
            // For now, we'll just mark it as online and let replication continue
            // In a full implementation, we would copy all files from replica_of SS
            
        } else if (g_num_servers < MAX_SERVERS) {
            int ss_index = g_num_servers;
            StorageServer* new_ss = &server_list[g_num_servers++];
            new_ss->ss_nm_port = ss_nm_port;
            new_ss->ss_client_port = ss_client_port;
            strncpy(new_ss->ss_ip_addr, peer_ip, INET_ADDRSTRLEN);
            
            // Initialize fault tolerance fields
            new_ss->is_online = 1;
            new_ss->last_heartbeat = time(NULL);
            new_ss->replica_of = -1;
            new_ss->replicated_by = -1;
            
            // Set up replication pairs: odd-numbered SS replicates even-numbered SS
            if (ss_index % 2 == 1 && ss_index > 0) {
                // This is odd-numbered, replicate the previous even-numbered
                new_ss->replica_of = ss_index - 1;
                server_list[ss_index - 1].replicated_by = ss_index;
                printf("  SS%d will replicate SS%d\n", ss_index, ss_index - 1);
                log_to_file(client_addr_str, "StorageServer", "INFO: SS%d will replicate SS%d", ss_index, ss_index - 1);
            }
            
            printf("  Registered SS%d (NM Port %d, Client Port %d)\n", ss_index, ss_nm_port, ss_client_port);
            log_to_file(client_addr_str, "StorageServer", "INFO: Registered SS%d (NM Port %d, Client Port %d)", ss_index, ss_nm_port, ss_client_port);
        }

        while ((token = strtok_r(NULL, " \n", &rest)) != NULL) {
            FileLocation* file = hash_map_find_unsafe(token);
            if (file != NULL) {
                file->ss_client_port = ss_client_port;
                strncpy(file->ss_ip_addr, peer_ip, INET_ADDRSTRLEN);
                printf("  Re-linking existing file: %s\n", token);
                log_to_file(client_addr_str, "StorageServer", "INFO: Re-linking existing file '%s'.", token);
            } else {
                 printf("  SS registered new non-persistent file: %s\n", token);
                 log_to_file(client_addr_str, "StorageServer", "INFO: Registered new non-persistent file: %s", token);
            }
        }
        
        pthread_mutex_unlock(&g_system_mutex);
        
        close(client_socket);
        printf("Storage Server registration complete. Connection closed.\n");
        log_to_file(client_addr_str, "StorageServer", "INFO: Registration complete. Connection closed.");
        return NULL; 
        
    } else if (strncmp(buffer, "HEARTBEAT", 9) == 0) {
        // Heartbeat from Storage Server
        int ss_nm_port;
        if (sscanf(buffer, "HEARTBEAT %d", &ss_nm_port) != 1) {
            printf("  Failed to parse HEARTBEAT message.\n");
            log_to_file(client_addr_str, "StorageServer", "ERROR: Failed to parse HEARTBEAT.");
            close(client_socket);
            return NULL;
        }
        
        pthread_mutex_lock(&g_system_mutex);
        
        // Find the SS by port and update heartbeat
        int found = 0;
        for (int i = 0; i < g_num_servers; i++) {
            if (server_list[i].ss_nm_port == ss_nm_port) {
                server_list[i].last_heartbeat = time(NULL);
                server_list[i].is_online = 1;
                found = 1;
                break;
            }
        }
        
        pthread_mutex_unlock(&g_system_mutex);
        
        if (found) {
            send(client_socket, "ACK\n", 4, 0);
        } else {
            printf("  HEARTBEAT from unknown SS (port %d)\n", ss_nm_port);
            log_to_file(client_addr_str, "StorageServer", "WARNING: HEARTBEAT from unknown SS (port %d)", ss_nm_port);
            send(client_socket, "ERR Unknown SS\n", 15, 0);
        }
        
        close(client_socket);
        return NULL;
        
    } else if (strncmp(buffer, "REGISTER_CLIENT", 15) == 0) {
        char username[256]; 
        if (sscanf(buffer, "REGISTER_CLIENT %s", username) != 1) {
            printf("  Failed to parse username. Closing connection.\n");
            log_to_file(client_addr_str, "Unknown", "ERROR: Failed to parse username from REGISTER_CLIENT.");
            close(client_socket);
            return NULL;
        }
        username[255] = '\0'; 
        printf("New connection from a Client (%s), user '%s'.\n", client_addr_str, username);
        log_to_file(client_addr_str, username, "REQ: REGISTER_CLIENT");

        pthread_mutex_lock(&g_system_mutex);
        if (g_num_clients < MAX_CLIENTS) {
            ClientInfo* new_client = &client_list[g_num_clients++];
            strncpy(new_client->username, username, 255);
            new_client->username[255] = '\0';
            strncpy(new_client->ip_addr, peer_ip, INET_ADDRSTRLEN);
            new_client->client_socket_fd = client_socket;
            printf("  User '%s' registered from %s.\n", username, client_addr_str);
            log_to_file(client_addr_str, username, "RES: Client registration successful.");
        } else {
            printf("  Max clients reached. Rejecting %s.\n", username);
            log_to_file(client_addr_str, username, "RES: Client registration failed: Max clients reached.");
        }
        pthread_mutex_unlock(&g_system_mutex);

        // 4. Enter a loop to handle commands from *this* client
        while ((bytes_read = read(client_socket, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            buffer[strcspn(buffer, "\n")] = 0; // Remove trailing newline

            // Check more specific commands BEFORE generic ones to avoid prefix matching
            if (strncmp(buffer, "VIEWCHECKPOINT", 14) == 0) {
                char filename[256], checkpoint_tag[128];
                if (sscanf(buffer, "VIEWCHECKPOINT %s %s", filename, checkpoint_tag) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid VIEWCHECKPOINT format. Use: VIEWCHECKPOINT <filename> <tag>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested VIEWCHECKPOINT for '%s' tag '%s'\n", filename, checkpoint_tag);
                log_to_file(client_addr_str, username, "REQ: VIEWCHECKPOINT for '%s' tag '%s'", filename, checkpoint_tag);

                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = -1;
                int permitted = 0;
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);
                
                file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }
                
                if (file != NULL) {
                    if (check_permission(file, username, 'R')) {
                        permitted = 1;
                        for (int j = 0; j < g_num_servers; j++) {
                            if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                server_list[j].ss_client_port == file->ss_client_port) 
                            {
                                strncpy(ss_ip, server_list[j].ss_ip_addr, INET_ADDRSTRLEN);
                                ss_nm_port = server_list[j].ss_nm_port;
                                break;
                            }
                        }
                    }
                }
                
                pthread_mutex_unlock(&g_system_mutex);

                if (!permitted) {
                    printf("  Access Denied or file not found.\n");
                    log_to_file(client_addr_str, username, "RES: VIEWCHECKPOINT denied for '%s'.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found or access denied.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                } else {
                    char checkpoint_content[BUFFER_SIZE];
                    if (forward_viewcheckpoint_to_ss(ss_ip, ss_nm_port, filename, checkpoint_tag, checkpoint_content, sizeof(checkpoint_content)) == 0) {
                        printf("  Sending checkpoint content to client.\n");
                        log_to_file(client_addr_str, username, "RES: VIEWCHECKPOINT for '%s' tag '%s' success.", filename, checkpoint_tag);
                        write(client_socket, checkpoint_content, strlen(checkpoint_content));
                    } else {
                        printf("  Failed to view checkpoint.\n");
                        log_to_file(client_addr_str, username, "RES: VIEWCHECKPOINT for '%s' failed.", filename);
                        snprintf(err_msg, sizeof(err_msg), "ERROR %d: Failed to view checkpoint.\n", ERROR_SERVER_ERROR);
                        write(client_socket, err_msg, strlen(err_msg));
                    }
                }

            } else if (strncmp(buffer, "LISTCHECKPOINTS", 15) == 0) {
                char filename[256];
                if (sscanf(buffer, "LISTCHECKPOINTS %s", filename) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid LISTCHECKPOINTS format. Use: LISTCHECKPOINTS <filename>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested LISTCHECKPOINTS for '%s'\n", filename);
                log_to_file(client_addr_str, username, "REQ: LISTCHECKPOINTS for '%s'", filename);

                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = -1;
                int permitted = 0;
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);
                
                file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }
                
                if (file != NULL) {
                    if (check_permission(file, username, 'R')) {
                        permitted = 1;
                        for (int j = 0; j < g_num_servers; j++) {
                            if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                server_list[j].ss_client_port == file->ss_client_port) 
                            {
                                strncpy(ss_ip, server_list[j].ss_ip_addr, INET_ADDRSTRLEN);
                                ss_nm_port = server_list[j].ss_nm_port;
                                break;
                            }
                        }
                    }
                }
                
                pthread_mutex_unlock(&g_system_mutex);

                if (!permitted) {
                    printf("  Access Denied or file not found.\n");
                    log_to_file(client_addr_str, username, "RES: LISTCHECKPOINTS denied for '%s'.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found or access denied.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                } else {
                    char checkpoint_list[BUFFER_SIZE];
                    if (forward_listcheckpoints_to_ss(ss_ip, ss_nm_port, filename, checkpoint_list, sizeof(checkpoint_list)) == 0) {
                        printf("  Sending checkpoint list to client.\n");
                        log_to_file(client_addr_str, username, "RES: LISTCHECKPOINTS for '%s' success.", filename);
                        write(client_socket, checkpoint_list, strlen(checkpoint_list));
                    } else {
                        printf("  Failed to list checkpoints.\n");
                        log_to_file(client_addr_str, username, "RES: LISTCHECKPOINTS for '%s' failed.", filename);
                        snprintf(err_msg, sizeof(err_msg), "ERROR %d: Failed to list checkpoints.\n", ERROR_SERVER_ERROR);
                        write(client_socket, err_msg, strlen(err_msg));
                    }
                }

            } else if (strncmp(buffer, "VIEW_REQUESTS", 13) == 0) {
                printf("Client '%s' requested to view access requests\n", username);
                log_to_file(client_addr_str, username, "REQ: VIEW_REQUESTS");

                pthread_mutex_lock(&g_system_mutex);
                
                char response_buffer[BUFFER_SIZE] = {0};
                int offset = snprintf(response_buffer, BUFFER_SIZE, 
                    "Pending Access Requests for your files:\n");
                offset += snprintf(response_buffer + offset, BUFFER_SIZE - offset,
                    "%-20s | %-20s | %-10s | %-20s\n", 
                    "File", "Requester", "Permission", "Timestamp");
                offset += snprintf(response_buffer + offset, BUFFER_SIZE - offset,
                    "--------------------------------------------------------------------------------\n");
                
                int requests_found = 0;
                for (int i = 0; i < g_num_access_requests; i++) {
                    if (strcmp(g_access_requests[i].owner_username, username) == 0 &&
                        g_access_requests[i].status == 0) { // Pending
                        requests_found++;
                        offset += snprintf(response_buffer + offset, BUFFER_SIZE - offset,
                            "%-20s | %-20s | %-10s | %-20s\n",
                            g_access_requests[i].filename,
                            g_access_requests[i].requester_username,
                            g_access_requests[i].requested_permission == 'R' ? "READ" : "WRITE",
                            g_access_requests[i].timestamp);
                        
                        if (offset >= BUFFER_SIZE - 200) break;
                    }
                }
                
                if (requests_found == 0) {
                    offset += snprintf(response_buffer + offset, BUFFER_SIZE - offset,
                        "(No pending requests)\n");
                }
                
                pthread_mutex_unlock(&g_system_mutex);
                
                printf("  Sending %d pending requests to client.\n", requests_found);
                log_to_file(client_addr_str, username, "RES: VIEW_REQUESTS success (%d requests).", requests_found);
                write(client_socket, response_buffer, strlen(response_buffer));

            } else if (strncmp(buffer, "VIEW", 4) == 0) {
                printf("Client requested VIEW\n");
                log_to_file(client_addr_str, username, "REQ: VIEW");
                
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

                // Iterate over hash map
                for (int i = 0; i < HASH_MAP_SIZE; i++) {
                    HashNode* node = g_file_hash_map[i];
                    while (node != NULL) {
                        FileLocation* file = &node->file;
                        int has_permission = check_permission(file, username, 'R');
                        
                        if (all_flag || has_permission) {
                            if (long_flag) {
                                int words = 0, chars = 0;
                                char created_ts[128], modified_ts[128]; 
                                StorageServer* ss = NULL;
                                for (int j = 0; j < g_num_servers; j++) {
                                    if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                        server_list[j].ss_client_port == file->ss_client_port) {
                                        ss = &server_list[j];
                                        break;
                                    }
                                }
                                
                                if (ss != NULL) {
                                    pthread_mutex_unlock(&g_system_mutex);
                                    get_metadata_from_ss(ss->ss_ip_addr, ss->ss_nm_port, file->filename, 
                                                                   &words, &chars, created_ts, modified_ts, 128);
                                    pthread_mutex_lock(&g_system_mutex);
                                }
                                
                                offset += sprintf(response_buffer + offset,
                                    "| %-20s | %5d | %5d | %-16s | %-10s |\n",
                                    file->filename, words, chars, 
                                    file->last_accessed_ts, 
                                    file->owner_username);
                                
                            } else {
                                offset += sprintf(response_buffer + offset, "%s\n", file->filename);
                            }
                            files_shown++;
                        }

                        if (offset > BUFFER_SIZE - 256) {
                            write(client_socket, response_buffer, offset);
                            offset = 0; 
                        }
                        node = node->next;
                    }
                }
                
                pthread_mutex_unlock(&g_system_mutex);
                
                if (files_shown == 0 && offset == 0) {
                    offset += sprintf(response_buffer + offset, "No files to display.\n");
                }
                
                if (offset > 0) {
                    write(client_socket, response_buffer, offset);
                }
                log_to_file(client_addr_str, username, "RES: VIEW success. Sent %d files.", files_shown);
            
            } else if (strncmp(buffer, "READ", 4) == 0) {
                char filename[1024];
                if (sscanf(buffer, "READ %s", filename) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid READ format. Use: READ <filename>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested READ for '%s'\n", filename);
                log_to_file(client_addr_str, username, "REQ: READ for '%s'", filename);

                char ss_ip[INET_ADDRSTRLEN];
                int ss_port;
                int permitted = 0; 
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);
                
                file = cache_get_unsafe(filename); // Check cache first
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename); // Check main map
                    if (file != NULL) {
                        cache_put_unsafe(filename, file); // Add to cache
                    }
                }
                
                if (file != NULL) {
                    if (check_permission(file, username, 'R')) {
                        permitted = 1;
                        strncpy(ss_ip, file->ss_ip_addr, INET_ADDRSTRLEN);
                        ss_port = file->ss_client_port;
                        
                        // Check if primary SS is online, if not use replica
                        int primary_ss_index = -1;
                        for (int j = 0; j < g_num_servers; j++) {
                            if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                server_list[j].ss_client_port == file->ss_client_port) {
                                primary_ss_index = j;
                                break;
                            }
                        }
                        
                        if (primary_ss_index >= 0 && !server_list[primary_ss_index].is_online) {
                            // Primary is offline, try to use replica
                            int replica_index = server_list[primary_ss_index].replicated_by;
                            if (replica_index >= 0 && replica_index < g_num_servers && 
                                server_list[replica_index].is_online) {
                                strncpy(ss_ip, server_list[replica_index].ss_ip_addr, INET_ADDRSTRLEN);
                                ss_port = server_list[replica_index].ss_client_port;
                                printf("  Primary SS offline, using replica SS%d\n", replica_index);
                                log_to_file(client_addr_str, username, "INFO: Primary SS offline, using replica SS%d for READ", replica_index);
                            }
                        }
                        
                        get_current_timestamp(file->last_accessed_ts, 128);
                        strncpy(file->last_accessed_by, username, 255);
                        
                        // Build full file path including folder for SS
                        if (strlen(file->folder_path) > 0 && strcmp(file->folder_path, "/") != 0) {
                            // File is in a folder - send full path
                            snprintf(filename, 1024, "%s/%s", 
                                    file->folder_path + 1, file->filename); // +1 to skip leading '/'
                        } else {
                            // File is in root - use base filename  
                            strncpy(filename, file->filename, 1023);
                            filename[1023] = '\0';
                        }
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                char response_buffer[BUFFER_SIZE];
                if (file == NULL) {
                    printf("  File not found.\n");
                    log_to_file(client_addr_str, username, "RES: READ for '%s' failed: File not found.", filename);
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                } else if (!permitted) {
                    printf("  Access Denied for user '%s'.\n", username);
                    log_to_file(client_addr_str, username, "RES: READ for '%s' failed: Access Denied.", filename);
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR %d: Access Denied.\n", ERROR_ACCESS_DENIED);
                } else {
                    printf("  Access Granted. Sending location to client: %s %d (file: %s)\n", ss_ip, ss_port, filename);
                    log_to_file(client_addr_str, username, "RES: READ for '%s' success. Sending SS_LOCATION %s %d %s", file->filename, ss_ip, ss_port, filename);
                    snprintf(response_buffer, sizeof(response_buffer), "SS_LOCATION %s %d %s\n", ss_ip, ss_port, filename);
                }
                write(client_socket, response_buffer, strlen(response_buffer));
            
            } else if (strncmp(buffer, "STREAM", 6) == 0) {
                char filename[1024];
                if (sscanf(buffer, "STREAM %s", filename) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid STREAM format. Use: STREAM <filename>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested STREAM for '%s'\n", filename);
                log_to_file(client_addr_str, username, "REQ: STREAM for '%s'", filename);

                char ss_ip[INET_ADDRSTRLEN];
                int ss_port;
                int permitted = 0; 
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);
                
                file = cache_get_unsafe(filename); // Check cache first
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename); // Check main map
                    if (file != NULL) {
                        cache_put_unsafe(filename, file); // Add to cache
                    }
                }
                
                if (file != NULL) {
                    if (check_permission(file, username, 'R')) {
                        permitted = 1;
                        strncpy(ss_ip, file->ss_ip_addr, INET_ADDRSTRLEN);
                        ss_port = file->ss_client_port;
                        
                        // Check if primary SS is online, if not use replica
                        int primary_ss_index = -1;
                        for (int j = 0; j < g_num_servers; j++) {
                            if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                server_list[j].ss_client_port == file->ss_client_port) {
                                primary_ss_index = j;
                                break;
                            }
                        }
                        
                        if (primary_ss_index >= 0 && !server_list[primary_ss_index].is_online) {
                            // Primary is offline, try to use replica
                            int replica_index = server_list[primary_ss_index].replicated_by;
                            if (replica_index >= 0 && replica_index < g_num_servers && 
                                server_list[replica_index].is_online) {
                                strncpy(ss_ip, server_list[replica_index].ss_ip_addr, INET_ADDRSTRLEN);
                                ss_port = server_list[replica_index].ss_client_port;
                                printf("  Primary SS offline, using replica SS%d\n", replica_index);
                                log_to_file(client_addr_str, username, "INFO: Primary SS offline, using replica SS%d for STREAM", replica_index);
                            }
                        }
                        
                        get_current_timestamp(file->last_accessed_ts, 128);
                        strncpy(file->last_accessed_by, username, 255);
                        
                        // Build full file path including folder for SS
                        if (strlen(file->folder_path) > 0 && strcmp(file->folder_path, "/") != 0) {
                            // File is in a folder - send full path
                            snprintf(filename, 1024, "%s/%s", 
                                    file->folder_path + 1, file->filename); // +1 to skip leading '/'
                        } else {
                            // File is in root - use base filename  
                            strncpy(filename, file->filename, 1023);
                            filename[1023] = '\0';
                        }
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                char response_buffer[BUFFER_SIZE];
                if (file == NULL) {
                    printf("  File not found.\n");
                    log_to_file(client_addr_str, username, "RES: STREAM for '%s' failed: File not found.", filename);
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                } else if (!permitted) {
                    printf("  Access Denied for user '%s'.\n", username);
                    log_to_file(client_addr_str, username, "RES: STREAM for '%s' failed: Access Denied.", filename);
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR %d: Access Denied.\n", ERROR_ACCESS_DENIED);
                } else {
                    printf("  Access Granted. Sending location to client: %s %d (file: %s)\n", ss_ip, ss_port, filename);
                    log_to_file(client_addr_str, username, "RES: STREAM for '%s' success. Sending SS_LOCATION %s %d %s", file->filename, ss_ip, ss_port, filename);
                    snprintf(response_buffer, sizeof(response_buffer), "SS_LOCATION %s %d %s\n", ss_ip, ss_port, filename);
                }
                write(client_socket, response_buffer, strlen(response_buffer));

            } else if (strncmp(buffer, "CREATEFOLDER", 12) == 0) {
                char foldername[256];
                if (sscanf(buffer, "CREATEFOLDER %s", foldername) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid CREATEFOLDER format. Use: CREATEFOLDER <foldername>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested CREATEFOLDER '%s'\n", foldername);
                log_to_file(client_addr_str, username, "REQ: CREATEFOLDER '%s'", foldername);

                // Build folder path (for now, simple flat structure: /foldername)
                char folder_path[512];
                snprintf(folder_path, sizeof(folder_path), "/%s", foldername);

                pthread_mutex_lock(&g_system_mutex);
                
                FolderNode* existing_folder = find_folder(folder_path);
                if (existing_folder != NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: CREATEFOLDER '%s' failed: Folder already exists.", foldername);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Folder already exists.\n", ERROR_FOLDER_EXISTS);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (g_num_servers == 0) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: CREATEFOLDER '%s' failed: No Storage Servers available.", foldername);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: No Storage Servers available.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                // Round-robin distribution: select next available SS
                int selected_ss = g_next_ss_for_file;
                g_next_ss_for_file = (g_next_ss_for_file + 1) % g_num_servers;
                
                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = server_list[selected_ss].ss_nm_port;
                strncpy(ss_ip, server_list[selected_ss].ss_ip_addr, INET_ADDRSTRLEN);
                
                printf("  Assigning folder '%s' to SS%d (Round-robin)\n", foldername, selected_ss);
                log_to_file(client_addr_str, username, "INFO: Assigning folder '%s' to SS%d (Round-robin)", foldername, selected_ss);
                
                pthread_mutex_unlock(&g_system_mutex);
                int result = forward_create_folder_to_ss(ss_ip, ss_nm_port, folder_path);
                
                // Replicate to backup SS asynchronously
                char repl_cmd[BUFFER_SIZE];
                snprintf(repl_cmd, sizeof(repl_cmd), "CREATE_FOLDER %s\n", folder_path);
                replicate_to_backup_ss(selected_ss, repl_cmd);

                if (result == 0) {
                    pthread_mutex_lock(&g_system_mutex);
                    add_folder_unsafe(foldername, folder_path, username);
                    pthread_mutex_unlock(&g_system_mutex);

                    printf("  Successfully created folder '%s'\n", foldername);
                    log_to_file(client_addr_str, username, "RES: CREATEFOLDER '%s' success.", foldername);
                    write(client_socket, "Folder created successfully.\n", sizeof("Folder created successfully.\n") - 1);
                } else {
                    log_to_file(client_addr_str, username, "RES: CREATEFOLDER '%s' failed: SS failed to create folder.", foldername);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Storage Server failed to create folder.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                }

            } else if (strncmp(buffer, "VIEWFOLDER", 10) == 0) {
                char foldername[256];
                if (sscanf(buffer, "VIEWFOLDER %s", foldername) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid VIEWFOLDER format. Use: VIEWFOLDER <foldername>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested VIEWFOLDER '%s'\n", foldername);
                log_to_file(client_addr_str, username, "REQ: VIEWFOLDER '%s'", foldername);

                char folder_path[512];
                snprintf(folder_path, sizeof(folder_path), "/%s", foldername);

                pthread_mutex_lock(&g_system_mutex);
                
                FolderNode* folder = find_folder(folder_path);
                if (folder == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: VIEWFOLDER '%s' failed: Folder not found.", foldername);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Folder not found.\n", ERROR_FOLDER_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (g_num_servers == 0) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: VIEWFOLDER '%s' failed: No Storage Servers available.", foldername);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: No Storage Servers available.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = server_list[0].ss_nm_port;
                strncpy(ss_ip, server_list[0].ss_ip_addr, INET_ADDRSTRLEN);
                
                pthread_mutex_unlock(&g_system_mutex);

                char* folder_listing = malloc(BUFFER_SIZE);
                if (folder_listing == NULL) {
                    log_to_file(client_addr_str, username, "CRITICAL: NM failed to allocate memory for folder listing.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Server memory allocation failed.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                int result = get_folder_listing_from_ss(ss_ip, ss_nm_port, folder_path, folder_listing, BUFFER_SIZE);

                if (result == 0) {
                    printf("  Folder listing:\n%s\n", folder_listing);
                    log_to_file(client_addr_str, username, "RES: VIEWFOLDER '%s' success.", foldername);
                    write(client_socket, folder_listing, strlen(folder_listing));
                } else {
                    log_to_file(client_addr_str, username, "RES: VIEWFOLDER '%s' failed: SS failed to get listing.", foldername);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Storage Server failed to get folder listing.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                }
                free(folder_listing);

            } else if (strncmp(buffer, "MOVE", 4) == 0) {
                char filename[256], foldername[256];
                if (sscanf(buffer, "MOVE %s %s", filename, foldername) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid MOVE format. Use: MOVE <filename> <foldername>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested MOVE '%s' to folder '%s'\n", filename, foldername);
                log_to_file(client_addr_str, username, "REQ: MOVE '%s' to '%s'", filename, foldername);

                char folder_path[512];
                snprintf(folder_path, sizeof(folder_path), "/%s", foldername);

                pthread_mutex_lock(&g_system_mutex);
                
                FolderNode* folder = find_folder(folder_path);
                if (folder == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: MOVE '%s' failed: Folder not found.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Folder not found.\n", ERROR_FOLDER_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                FileLocation* file = hash_map_find_unsafe(filename);
                if (file == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: MOVE '%s' failed: File not found.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                // Check if user has write permission
                if (!check_permission(file, username, 'W')) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: MOVE '%s' failed: Access Denied.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Access Denied (Write permission required).\n", ERROR_ACCESS_DENIED);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = -1;
                int ss_index = -1;
                for (int j = 0; j < g_num_servers; j++) {
                    if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                        server_list[j].ss_client_port == file->ss_client_port) 
                    {
                        strncpy(ss_ip, server_list[j].ss_ip_addr, INET_ADDRSTRLEN);
                        ss_nm_port = server_list[j].ss_nm_port;
                        ss_index = j;
                        break;
                    }
                }

                pthread_mutex_unlock(&g_system_mutex);

                if (ss_nm_port == -1) {
                    log_to_file(client_addr_str, username, "RES: MOVE '%s' failed: SS not found.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Storage Server not found.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                int result = forward_move_to_ss(ss_ip, ss_nm_port, filename, folder_path);
                
                // Replicate to backup SS asynchronously
                if (ss_index >= 0) {
                    char repl_cmd[BUFFER_SIZE];
                    snprintf(repl_cmd, sizeof(repl_cmd), "MOVE_FILE %s %s\n", filename, folder_path);
                    replicate_to_backup_ss(ss_index, repl_cmd);
                }

                if (result == 0) {
                    pthread_mutex_lock(&g_system_mutex);
                    file = hash_map_find_unsafe(filename); // Re-find after lock
                    if (file != NULL) {
                        strncpy(file->folder_path, folder_path, 511);
                        file->folder_path[511] = '\0';
                        save_registry_to_disk_unsafe();
                    }
                    pthread_mutex_unlock(&g_system_mutex);

                    printf("  Successfully moved '%s' to folder '%s'\n", filename, foldername);
                    log_to_file(client_addr_str, username, "RES: MOVE '%s' to '%s' success.", filename, foldername);
                    write(client_socket, "File moved successfully.\n", sizeof("File moved successfully.\n") - 1);
                } else {
                    log_to_file(client_addr_str, username, "RES: MOVE '%s' failed: SS failed to move file.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Storage Server failed to move file.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                }

            } else if (strncmp(buffer, "CREATE", 6) == 0) {
                char filename[256];
                if (sscanf(buffer, "CREATE %s", filename) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid CREATE format. Use: CREATE <filename>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested CREATE for '%s'\n", filename);
                log_to_file(client_addr_str, username, "REQ: CREATE for '%s'", filename);

                pthread_mutex_lock(&g_system_mutex);
                
                FileLocation* file = hash_map_find_unsafe(filename);
                if (file != NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: CREATE for '%s' failed: File already exists.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File already exists.\n", ERROR_FILE_EXISTS);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (g_num_servers == 0) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: CREATE for '%s' failed: No Storage Servers available.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: No Storage Servers available.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                // Round-robin distribution: select next available SS
                int selected_ss = g_next_ss_for_file;
                g_next_ss_for_file = (g_next_ss_for_file + 1) % g_num_servers;
                
                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = server_list[selected_ss].ss_nm_port;
                int ss_client_port = server_list[selected_ss].ss_client_port;
                strncpy(ss_ip, server_list[selected_ss].ss_ip_addr, INET_ADDRSTRLEN);
                
                printf("  Assigning '%s' to SS%d (Round-robin)\n", filename, selected_ss);
                log_to_file(client_addr_str, username, "INFO: Assigning '%s' to SS%d (Round-robin)", filename, selected_ss);
                
                pthread_mutex_unlock(&g_system_mutex);
                int result = forward_create_to_ss(ss_ip, ss_nm_port, filename);
                
                // Replicate to backup SS asynchronously
                char repl_cmd[BUFFER_SIZE];
                snprintf(repl_cmd, sizeof(repl_cmd), "CREATE_FILE %s\n", filename);
                replicate_to_backup_ss(selected_ss, repl_cmd);

                if (result == 0) {
                    FileLocation new_file;
                    strncpy(new_file.filename, filename, 255);
                    new_file.ss_client_port = ss_client_port;
                    strncpy(new_file.ss_ip_addr, ss_ip, INET_ADDRSTRLEN);
                    strncpy(new_file.owner_username, username, 255); 
                    new_file.num_permissions = 0; 
                    strncpy(new_file.last_accessed_by, "N/A", 255);
                    strncpy(new_file.last_accessed_ts, "N/A", 127);
                    strncpy(new_file.folder_path, "/", 511); // Root folder by default
                    
                    pthread_mutex_lock(&g_system_mutex);
                    hash_map_insert_unsafe(new_file);
                    save_registry_to_disk_unsafe(); // Save persistence
                    pthread_mutex_unlock(&g_system_mutex);

                    printf("  Successfully registered new file '%s' for owner '%s'\n", filename, username);
                    log_to_file(client_addr_str, username, "RES: CREATE for '%s' success.", filename);
                    write(client_socket, "File created successfully.\n", sizeof("File created successfully.\n") - 1);
                } else {
                    log_to_file(client_addr_str, username, "RES: CREATE for '%s' failed: SS failed to create file.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Storage Server failed to create file.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                }
            
            } else if (strncmp(buffer, "DELETE", 6) == 0) {
                char filename[256];
                if (sscanf(buffer, "DELETE %s", filename) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid DELETE format. Use: DELETE <filename>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested DELETE for '%s'\n", filename);
                log_to_file(client_addr_str, username, "REQ: DELETE for '%s'", filename);

                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = -1;
                int ss_index = -1;
                int permitted = 0; 
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);
                
                file = hash_map_find_unsafe(filename); // No cache needed, we're deleting
                
                if (file != NULL) {
                    if (strcmp(file->owner_username, username) == 0) {
                        permitted = 1;
                        for (int j = 0; j < g_num_servers; j++) {
                            if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                server_list[j].ss_client_port == file->ss_client_port) 
                            {
                                strncpy(ss_ip, server_list[j].ss_ip_addr, INET_ADDRSTRLEN);
                                ss_nm_port = server_list[j].ss_nm_port;
                                ss_index = j;
                                break;
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                if (file == NULL) {
                    log_to_file(client_addr_str, username, "RES: DELETE for '%s' failed: File not found.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                if (!permitted) {
                    printf("  Access Denied for user '%s'. Not owner.\n", username);
                    log_to_file(client_addr_str, username, "RES: DELETE for '%s' failed: Access Denied (not owner).", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Access Denied. Only the owner can delete a file.\n", ERROR_ACCESS_DENIED);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (ss_nm_port == -1) {
                    log_to_file(client_addr_str, username, "RES: DELETE for '%s' failed: SS not found (directory out of sync).", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Could not find SS for file. Directory out of sync.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                int result = forward_delete_to_ss(ss_ip, ss_nm_port, filename);
                
                // Replicate delete to backup SS asynchronously
                if (ss_index >= 0) {
                    char repl_cmd[BUFFER_SIZE];
                    snprintf(repl_cmd, sizeof(repl_cmd), "DELETE_FILE %s\n", filename);
                    replicate_to_backup_ss(ss_index, repl_cmd);
                }

                if (result == 0) {
                    pthread_mutex_lock(&g_system_mutex);
                    if (hash_map_delete_unsafe(filename)) {
                        cache_delete_unsafe(filename); // Evict from cache
                        save_registry_to_disk_unsafe(); // Save persistence
                        printf("  Successfully deleted file '%s' from directory.\n", filename);
                        log_to_file(client_addr_str, username, "RES: DELETE for '%s' success.", filename);
                        write(client_socket, "File deleted successfully.\n", sizeof("File deleted successfully.\n") - 1);
                    } else {
                        printf("  Error: File was not found in hash map during delete.\n");
                        log_to_file(client_addr_str, username, "ERROR: DELETE for '%s' failed: File not found in map post-check.", filename);
                    }
                    pthread_mutex_unlock(&g_system_mutex);
                } else {
                    log_to_file(client_addr_str, username, "RES: DELETE for '%s' failed: SS failed to delete file.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Storage Server failed to delete file.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                }

            } else if (strncmp(buffer, "LIST", 4) == 0) {
                printf("Client requested LIST\n");
                log_to_file(client_addr_str, username, "REQ: LIST");
                
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
                log_to_file(client_addr_str, username, "RES: LIST success.");

            } else if (strncmp(buffer, "ADDACCESS", 9) == 0) {
                char perm_char_str[2], target_filename[256], target_username[256];
                if (sscanf(buffer, "ADDACCESS %1s %s %s", perm_char_str, target_filename, target_username) != 3) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid format. Use: ADDACCESS <R|W> <filename> <username>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                char perm = perm_char_str[0];
                if (perm != 'R' && perm != 'W') {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Permission must be 'R' or 'W'.\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("User '%s' requested ADDACCESS %c for '%s' to user '%s'\n", username, perm, target_filename, target_username);
                log_to_file(client_addr_str, username, "REQ: ADDACCESS %c for '%s' to user '%s'", perm, target_filename, target_username);

                pthread_mutex_lock(&g_system_mutex);
                
                FileLocation* file = cache_get_unsafe(target_filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(target_filename);
                    if (file != NULL) cache_put_unsafe(target_filename, file);
                }
                
                if (file == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: ADDACCESS for '%s' failed: File not found.", target_filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (strcmp(file->owner_username, username) != 0) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: ADDACCESS for '%s' failed: Not owner.", target_filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: You are not the owner of this file.\n", ERROR_ACCESS_DENIED);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (file->num_permissions < MAX_PERMISSIONS) {
                    AccessControl* new_perm = &file->acl[file->num_permissions++];
                    strncpy(new_perm->username, target_username, 255);
                    new_perm->permission = perm;
                    
                    save_registry_to_disk_unsafe(); // Save persistence
                    printf("  Access granted.\n");
                    log_to_file(client_addr_str, username, "RES: ADDACCESS for '%s' to user '%s' success.", target_filename, target_username);
                    write(client_socket, "Access granted successfully.\n", sizeof("Access granted successfully.\n") - 1);
                } else {
                    log_to_file(client_addr_str, username, "RES: ADDACCESS for '%s' failed: Max permissions reached.", target_filename);
                    write(client_socket, "ERROR: File has reached its maximum permission entries.\n", 56);
                }
                pthread_mutex_unlock(&g_system_mutex);

            } else if (strncmp(buffer, "REMACCESS", 9) == 0) {
                char target_filename[256], target_username[256];
                if (sscanf(buffer, "REMACCESS %s %s", target_filename, target_username) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid format. Use: REMACCESS <filename> <username>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("User '%s' requested REMACCESS for '%s' from user '%s'\n", username, target_filename, target_username);
                log_to_file(client_addr_str, username, "REQ: REMACCESS for '%s' from user '%s'", target_filename, target_username);

                pthread_mutex_lock(&g_system_mutex);
                
                FileLocation* file = cache_get_unsafe(target_filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(target_filename);
                    if (file != NULL) cache_put_unsafe(target_filename, file);
                }

                if (file == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: REMACCESS for '%s' failed: File not found.", target_filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (strcmp(file->owner_username, username) != 0) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: REMACCESS for '%s' failed: Not owner.", target_filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: You are not the owner of this file.\n", ERROR_ACCESS_DENIED);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                int perm_index = -1;
                for (int i = 0; i < file->num_permissions; i++) {
                    if (strcmp(file->acl[i].username, target_username) == 0) {
                        perm_index = i;
                        break;
                    }
                }

                if (perm_index == -1) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: REMACCESS for '%s' failed: User '%s' has no permissions.", target_filename, target_username);
                    snprintf(err_msg, sizeof(err_msg), "ERROR: That user does not have special permissions on this file.\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                for (int i = perm_index; i < file->num_permissions - 1; i++) {
                    file->acl[i] = file->acl[i + 1];
                }
                file->num_permissions--;
                
                save_registry_to_disk_unsafe(); // Save persistence
                printf("  Access removed.\n");
                log_to_file(client_addr_str, username, "RES: REMACCESS for '%s' from user '%s' success.", target_filename, target_username);
                write(client_socket, "Access removed successfully.\n", sizeof("Access removed successfully.\n") - 1);
                pthread_mutex_unlock(&g_system_mutex);
            
            } else if (strncmp(buffer, "INFO", 4) == 0) {
                char filename[256];
                if (sscanf(buffer, "INFO %s", filename) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid INFO format. Use: INFO <filename>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested INFO for '%s'\n", filename);
                log_to_file(client_addr_str, username, "REQ: INFO for '%s'", filename);

                char response_buffer[BUFFER_SIZE] = {0};
                int offset = 0;
                FileLocation file_copy;
                StorageServer* ss = NULL;
                
                pthread_mutex_lock(&g_system_mutex);

                FileLocation* file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }

                if (file == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: INFO for '%s' failed: File not found.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                if (!check_permission(file, username, 'R')) {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  Access Denied for user '%s'.\n", username);
                    log_to_file(client_addr_str, username, "RES: INFO for '%s' failed: Access Denied.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Access Denied.\n", ERROR_ACCESS_DENIED);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                file_copy = *file; // Make a copy
                
                for (int j = 0; j < g_num_servers; j++) {
                    if (strcmp(server_list[j].ss_ip_addr, file_copy.ss_ip_addr) == 0 &&
                        server_list[j].ss_client_port == file_copy.ss_client_port) {
                        ss = &server_list[j];
                        break;
                    }
                }
                
                if (ss == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    log_to_file(client_addr_str, username, "RES: INFO for '%s' failed: SS not found (directory out of sync).", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Could not find SS for file. Directory out of sync.\n", ERROR_SERVER_ERROR);
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
                    log_to_file(client_addr_str, username, "RES: INFO for '%s' failed: Failed to get metadata from SS.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Failed to retrieve file metadata from Storage Server.\n", ERROR_SERVER_ERROR);
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
                log_to_file(client_addr_str, username, "RES: INFO for '%s' success.", filename);
            
            } else if (strncmp(buffer, "EXEC", 4) == 0) {
                char filename[256];
                if (sscanf(buffer, "EXEC %s", filename) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid EXEC format. Use: EXEC <filename>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested EXEC for '%s'\n", filename);
                log_to_file(client_addr_str, username, "REQ: EXEC for '%s'", filename);

                int permitted = 0;
                FileLocation file_copy;
                StorageServer* ss = NULL;
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);
                
                file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }
                
                if (file != NULL) {
                    if (check_permission(file, username, 'R')) {
                        permitted = 1;
                        file_copy = *file; 
                        for (int j = 0; j < g_num_servers; j++) {
                            if (strcmp(server_list[j].ss_ip_addr, file_copy.ss_ip_addr) == 0 &&
                                server_list[j].ss_client_port == file_copy.ss_client_port) {
                                ss = &server_list[j];
                                break;
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                if (file == NULL) {
                    log_to_file(client_addr_str, username, "RES: EXEC for '%s' failed: File not found.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                if (!permitted) {
                    log_to_file(client_addr_str, username, "RES: EXEC for '%s' failed: Access Denied.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Access Denied.\n", ERROR_ACCESS_DENIED);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                if (ss == NULL) {
                    log_to_file(client_addr_str, username, "RES: EXEC for '%s' failed: SS not found (directory out of sync).", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Could not find SS for file. Directory out of sync.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                char* file_content = malloc(EXEC_OUTPUT_BUFFER_SIZE);
                if (file_content == NULL) {
                    log_to_file(client_addr_str, username, "CRITICAL: NM failed to allocate memory for exec.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: NM failed to allocate memory for exec.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                if (get_file_content_from_ss(ss->ss_ip_addr, ss->ss_client_port, file_copy.filename, file_content, EXEC_OUTPUT_BUFFER_SIZE) != 0) {
                    log_to_file(client_addr_str, username, "ERROR: NM failed to fetch file '%s' from SS for EXEC.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: NM failed to fetch file from SS.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    free(file_content);
                    continue;
                }
                
                printf("  Executing content:\n%s\n", file_content);
                log_to_file(client_addr_str, username, "INFO: Executing content for '%s'...", filename);
                FILE* pipe = popen(file_content, "r");
                free(file_content); 

                if (pipe == NULL) {
                    log_to_file(client_addr_str, username, "ERROR: NM failed to popen() for exec.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: NM failed to execute command.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                char* output_buffer = malloc(EXEC_OUTPUT_BUFFER_SIZE);
                if (output_buffer == NULL) {
                    log_to_file(client_addr_str, username, "CRITICAL: NM failed to allocate memory for exec output.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: NM failed to allocate memory for output.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    pclose(pipe);
                    continue;
                }
                
                ssize_t output_size = fread(output_buffer, 1, EXEC_OUTPUT_BUFFER_SIZE - 1, pipe);
                output_buffer[output_size] = '\0';
                pclose(pipe);

                printf("  Sending output to client:\n%s\n", output_buffer);
                log_to_file(client_addr_str, username, "RES: Sending exec output for '%s' to client.", filename);
                write(client_socket, output_buffer, output_size);
                free(output_buffer);

            } else if (strncmp(buffer, "UNDO", 4) == 0) {
                char filename[256];
                if (sscanf(buffer, "UNDO %s", filename) != 1) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid UNDO format. Use: UNDO <filename>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested UNDO for '%s'\n", filename);
                log_to_file(client_addr_str, username, "REQ: UNDO for '%s'", filename);

                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = -1;
                int permitted = 0; 
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);
                
                file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }
                
                if (file != NULL) {
                    if (check_permission(file, username, 'W')) {
                        permitted = 1;
                        for (int j = 0; j < g_num_servers; j++) {
                            if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                server_list[j].ss_client_port == file->ss_client_port) 
                            {
                                strncpy(ss_ip, server_list[j].ss_ip_addr, INET_ADDRSTRLEN);
                                ss_nm_port = server_list[j].ss_nm_port;
                                break;
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                if (file == NULL) {
                    log_to_file(client_addr_str, username, "RES: UNDO for '%s' failed: File not found.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                if (!permitted) {
                    log_to_file(client_addr_str, username, "RES: UNDO for '%s' failed: Access Denied (Write permission required).", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Access Denied (Write permission required to undo).\n", ERROR_ACCESS_DENIED);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                if (ss_nm_port == -1) {
                    log_to_file(client_addr_str, username, "RES: UNDO for '%s' failed: SS not found (directory out of sync).", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Could not find SS for file. Directory out of sync.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }

                int result = forward_undo_to_ss(ss_ip, ss_nm_port, filename);

                if (result == 0) {
                    log_to_file(client_addr_str, username, "RES: UNDO for '%s' success.", filename);
                    write(client_socket, "Undo Successful!\n", 17);
                } else if (result == -2) {
                    log_to_file(client_addr_str, username, "RES: UNDO for '%s' failed: No undo history found.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: No undo history found for this file.\n", ERROR_NO_UNDO_HISTORY);
                    write(client_socket, err_msg, strlen(err_msg));
                } else {
                    log_to_file(client_addr_str, username, "RES: UNDO for '%s' failed: SS failed to undo file.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Storage Server failed to undo file.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                }
            
            } else if (strncmp(buffer, "WRITE", 5) == 0) {
                char filename[1024];
                int sentence_num;
                if (sscanf(buffer, "WRITE %s %d", filename, &sentence_num) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid WRITE format. Use: WRITE <filename> <sentence_num>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested WRITE for '%s', sentence %d\n", filename, sentence_num);
                log_to_file(client_addr_str, username, "REQ: WRITE for '%s', sentence %d.", filename, sentence_num);

                char ss_ip[INET_ADDRSTRLEN];
                int ss_port;
                int permitted = 0;
                int already_locked = 0;
                char locking_user[256];
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);

                file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }
                
                if (file != NULL) {
                    if (check_permission(file, username, 'W')) {
                        permitted = 1;
                        
                        for (int j = 0; j < g_num_locks; j++) {
                            if (strcmp(g_file_locks[j].filename, filename) == 0 && g_file_locks[j].sentence_num == sentence_num) {
                                already_locked = 1;
                                strncpy(locking_user, g_file_locks[j].username, 255);
                                break;
                            }
                        }

                        if (!already_locked && g_num_locks < MAX_LOCKS) {
                            FileLock* new_lock = &g_file_locks[g_num_locks++];
                            strncpy(new_lock->filename, filename, 255);
                            strncpy(new_lock->username, username, 255);
                            new_lock->sentence_num = sentence_num;
                            
                            strncpy(ss_ip, file->ss_ip_addr, INET_ADDRSTRLEN);
                            ss_port = file->ss_client_port;
                            
                            // Check if primary SS is online, if not use replica
                            int primary_ss_index = -1;
                            for (int j = 0; j < g_num_servers; j++) {
                                if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                    server_list[j].ss_client_port == file->ss_client_port) {
                                    primary_ss_index = j;
                                    break;
                                }
                            }
                            
                            if (primary_ss_index >= 0 && !server_list[primary_ss_index].is_online) {
                                // Primary is offline, try to use replica
                                int replica_index = server_list[primary_ss_index].replicated_by;
                                if (replica_index >= 0 && replica_index < g_num_servers && 
                                    server_list[replica_index].is_online) {
                                    strncpy(ss_ip, server_list[replica_index].ss_ip_addr, INET_ADDRSTRLEN);
                                    ss_port = server_list[replica_index].ss_client_port;
                                    printf("  Primary SS offline, using replica SS%d\n", replica_index);
                                    log_to_file(client_addr_str, username, "INFO: Primary SS offline, using replica SS%d for WRITE", replica_index);
                                }
                            }
                            
                            // Build full file path including folder for SS
                            if (strlen(file->folder_path) > 0 && strcmp(file->folder_path, "/") != 0) {
                                // File is in a folder - send full path
                                snprintf(filename, 1024, "%s/%s", 
                                        file->folder_path + 1, file->filename); // +1 to skip leading '/'
                            } else {
                                // File is in root - use base filename  
                                strncpy(filename, file->filename, 1023);
                                filename[1023] = '\0';
                            }
                            
                            printf("  Lock granted to '%s'\n", username);
                            log_to_file(client_addr_str, username, "INFO: Lock granted for '%s' (sent %d).", filename, sentence_num);
                        } else if (already_locked) {
                            // Lock is held
                        } else {
                            already_locked = -1; // Signal max locks
                        }
                    }
                }
                pthread_mutex_unlock(&g_system_mutex);

                char response_buffer[BUFFER_SIZE];
                if (file == NULL) {
                    log_to_file(client_addr_str, username, "RES: WRITE for '%s' failed: File not found.", filename);
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                } else if (!permitted) {
                    log_to_file(client_addr_str, username, "RES: WRITE for '%s' failed: Access Denied (Write permission required).", filename);
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR %d: Access Denied (Write permission required).\n", ERROR_ACCESS_DENIED);
                } else if (already_locked == 1) {
                    log_to_file(client_addr_str, username, "RES: WRITE for '%s' failed: Sentence %d is locked by '%s'.", filename, sentence_num, locking_user);
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR %d: Sentence is currently locked by '%s'.\n", ERROR_FILE_LOCKED, locking_user);
                } else if (already_locked == -1) {
                    log_to_file(client_addr_str, username, "RES: WRITE for '%s' failed: System is at maximum lock capacity.", filename);
                    snprintf(response_buffer, sizeof(response_buffer), "ERROR %d: System is at maximum lock capacity. Try again later.\n", ERROR_MAX_LOCKS);
                } else {
                    log_to_file(client_addr_str, username, "RES: WRITE for '%s' success. Sending SS_LOCATION %s %d %s", file->filename, ss_ip, ss_port, filename);
                    snprintf(response_buffer, sizeof(response_buffer), "SS_LOCATION %s %d %s\n", ss_ip, ss_port, filename);
                }
                write(client_socket, response_buffer, strlen(response_buffer));
            
            } else if (strncmp(buffer, "RELEASE_LOCK", 12) == 0) {
                char filename[256];
                int sentence_num;
                if (sscanf(buffer, "RELEASE_LOCK %s %d", filename, &sentence_num) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid RELEASE_LOCK format.\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested RELEASE_LOCK for '%s', sentence %d\n", filename, sentence_num);
                log_to_file(client_addr_str, username, "REQ: RELEASE_LOCK for '%s' (sent %d).", filename, sentence_num);

                pthread_mutex_lock(&g_system_mutex);
                int lock_index = -1;
                for (int i = 0; i < g_num_locks; i++) {
                    if (strcmp(g_file_locks[i].filename, filename) == 0 &&
                        g_file_locks[i].sentence_num == sentence_num &&
                        strcmp(g_file_locks[i].username, username) == 0)
                    {
                        lock_index = i;
                        break;
                    }
                }

                if (lock_index != -1) {
                    for (int i = lock_index; i < g_num_locks - 1; i++) {
                        g_file_locks[i] = g_file_locks[i + 1];
                    }
                    g_num_locks--;
                    printf("  Lock released.\n");
                    log_to_file(client_addr_str, username, "RES: Lock released for '%s' (sent %d).", filename, sentence_num);
                    write(client_socket, "ACK_LOCK_RELEASED\n", 18);
                } else {
                    printf("  Invalid lock release request.\n");
                    log_to_file(client_addr_str, username, "RES: RELEASE_LOCK failed: Lock not held by user.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: You do not hold that lock.\n", ERROR_ACCESS_DENIED);
                    write(client_socket, err_msg, strlen(err_msg));
                }
                pthread_mutex_unlock(&g_system_mutex);

            } else if (strncmp(buffer, "CHECKPOINT", 10) == 0) {
                char filename[256], checkpoint_tag[128];
                if (sscanf(buffer, "CHECKPOINT %s %s", filename, checkpoint_tag) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid CHECKPOINT format. Use: CHECKPOINT <filename> <tag>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested CHECKPOINT for '%s' with tag '%s'\n", filename, checkpoint_tag);
                log_to_file(client_addr_str, username, "REQ: CHECKPOINT for '%s' with tag '%s'", filename, checkpoint_tag);

                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = -1;
                int permitted = 0;
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);
                
                file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }
                
                if (file != NULL) {
                    if (check_permission(file, username, 'R')) {
                        permitted = 1;
                        for (int j = 0; j < g_num_servers; j++) {
                            if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                server_list[j].ss_client_port == file->ss_client_port) 
                            {
                                strncpy(ss_ip, server_list[j].ss_ip_addr, INET_ADDRSTRLEN);
                                ss_nm_port = server_list[j].ss_nm_port;
                                break;
                            }
                        }
                    }
                }
                
                pthread_mutex_unlock(&g_system_mutex);

                if (!permitted) {
                    printf("  Access Denied or file not found.\n");
                    log_to_file(client_addr_str, username, "RES: CHECKPOINT denied for '%s'.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found or access denied.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                } else {
                    char response[256];
                    if (forward_checkpoint_to_ss(ss_ip, ss_nm_port, filename, checkpoint_tag, response, sizeof(response)) == 0) {
                        printf("  Checkpoint created successfully.\n");
                        log_to_file(client_addr_str, username, "RES: CHECKPOINT for '%s' tag '%s' success.", filename, checkpoint_tag);
                        snprintf(err_msg, sizeof(err_msg), "SUCCESS: Checkpoint created.\n");
                        write(client_socket, err_msg, strlen(err_msg));
                    } else {
                        printf("  Checkpoint creation failed.\n");
                        log_to_file(client_addr_str, username, "RES: CHECKPOINT for '%s' failed.", filename);
                        snprintf(err_msg, sizeof(err_msg), "ERROR %d: Failed to create checkpoint.\n", ERROR_SERVER_ERROR);
                        write(client_socket, err_msg, strlen(err_msg));
                    }
                }

            } else if (strncmp(buffer, "REVERT", 6) == 0) {
                char filename[256], checkpoint_tag[128];
                if (sscanf(buffer, "REVERT %s %s", filename, checkpoint_tag) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid REVERT format. Use: REVERT <filename> <tag>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                printf("Client requested REVERT for '%s' to tag '%s'\n", filename, checkpoint_tag);
                log_to_file(client_addr_str, username, "REQ: REVERT for '%s' to tag '%s'", filename, checkpoint_tag);

                char ss_ip[INET_ADDRSTRLEN];
                int ss_nm_port = -1;
                int permitted = 0;
                FileLocation* file = NULL;

                pthread_mutex_lock(&g_system_mutex);
                
                file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }
                
                if (file != NULL) {
                    if (check_permission(file, username, 'W')) {
                        permitted = 1;
                        for (int j = 0; j < g_num_servers; j++) {
                            if (strcmp(server_list[j].ss_ip_addr, file->ss_ip_addr) == 0 &&
                                server_list[j].ss_client_port == file->ss_client_port) 
                            {
                                strncpy(ss_ip, server_list[j].ss_ip_addr, INET_ADDRSTRLEN);
                                ss_nm_port = server_list[j].ss_nm_port;
                                break;
                            }
                        }
                    }
                }
                
                pthread_mutex_unlock(&g_system_mutex);

                if (!permitted) {
                    printf("  Access Denied or file not found.\n");
                    log_to_file(client_addr_str, username, "RES: REVERT denied for '%s'.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found or access denied (write permission required).\n", ERROR_ACCESS_DENIED);
                    write(client_socket, err_msg, strlen(err_msg));
                } else {
                    char response[256];
                    if (forward_revert_to_ss(ss_ip, ss_nm_port, filename, checkpoint_tag, response, sizeof(response)) == 0) {
                        printf("  File reverted successfully.\n");
                        log_to_file(client_addr_str, username, "RES: REVERT for '%s' to tag '%s' success.", filename, checkpoint_tag);
                        snprintf(err_msg, sizeof(err_msg), "SUCCESS: File reverted to checkpoint.\n");
                        write(client_socket, err_msg, strlen(err_msg));
                    } else {
                        printf("  Revert failed.\n");
                        log_to_file(client_addr_str, username, "RES: REVERT for '%s' failed.", filename);
                        snprintf(err_msg, sizeof(err_msg), "ERROR %d: Failed to revert file.\n", ERROR_SERVER_ERROR);
                        write(client_socket, err_msg, strlen(err_msg));
                    }
                }

            } else if (strncmp(buffer, "REQUEST_ACCESS", 14) == 0) {
                char filename[256], permission_type[10];
                if (sscanf(buffer, "REQUEST_ACCESS %s %s", filename, permission_type) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid REQUEST_ACCESS format. Use: REQUEST_ACCESS <filename> <R/W>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                // Validate permission type
                char requested_perm;
                if (strcmp(permission_type, "R") == 0 || strcmp(permission_type, "READ") == 0) {
                    requested_perm = 'R';
                } else if (strcmp(permission_type, "W") == 0 || strcmp(permission_type, "WRITE") == 0) {
                    requested_perm = 'W';
                } else {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid permission type. Use R (read) or W (write).\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                printf("Client '%s' requested %s access to '%s'\n", username, 
                       requested_perm == 'R' ? "READ" : "WRITE", filename);
                log_to_file(client_addr_str, username, "REQ: REQUEST_ACCESS for '%s' with permission '%c'", filename, requested_perm);

                pthread_mutex_lock(&g_system_mutex);
                
                FileLocation* file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }
                
                if (file == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  File not found.\n");
                    log_to_file(client_addr_str, username, "RES: REQUEST_ACCESS failed: File '%s' not found.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                // Check if requester is the owner
                if (strcmp(file->owner_username, username) == 0) {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  User is already the owner.\n");
                    log_to_file(client_addr_str, username, "RES: REQUEST_ACCESS denied: User is the owner of '%s'.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR: You are the owner of this file.\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                // Check if user already has permission
                int already_has_permission = 0;
                for (int i = 0; i < file->num_permissions; i++) {
                    if (strcmp(file->acl[i].username, username) == 0) {
                        if (file->acl[i].permission == 'W' || 
                            (file->acl[i].permission == 'R' && requested_perm == 'R')) {
                            already_has_permission = 1;
                            break;
                        }
                    }
                }
                
                if (already_has_permission) {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  User already has sufficient permission.\n");
                    log_to_file(client_addr_str, username, "RES: REQUEST_ACCESS denied: User already has permission for '%s'.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR: You already have sufficient permission for this file.\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                // Check if a pending request already exists
                int request_exists = 0;
                for (int i = 0; i < g_num_access_requests; i++) {
                    if (strcmp(g_access_requests[i].filename, filename) == 0 &&
                        strcmp(g_access_requests[i].requester_username, username) == 0 &&
                        g_access_requests[i].status == 0) { // Pending
                        request_exists = 1;
                        break;
                    }
                }
                
                if (request_exists) {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  Request already pending.\n");
                    log_to_file(client_addr_str, username, "RES: REQUEST_ACCESS denied: Request already pending for '%s'.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR: You already have a pending request for this file.\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                // Create new access request
                if (g_num_access_requests >= MAX_ACCESS_REQUESTS) {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  Maximum access requests reached.\n");
                    log_to_file(client_addr_str, username, "RES: REQUEST_ACCESS failed: Maximum requests reached.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Maximum access requests reached.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                AccessRequest* new_request = &g_access_requests[g_num_access_requests];
                strncpy(new_request->filename, filename, 255);
                new_request->filename[255] = '\0';
                strncpy(new_request->requester_username, username, 255);
                new_request->requester_username[255] = '\0';
                strncpy(new_request->owner_username, file->owner_username, 255);
                new_request->owner_username[255] = '\0';
                new_request->requested_permission = requested_perm;
                new_request->status = 0; // Pending
                
                // Add timestamp
                time_t now = time(NULL);
                struct tm* tm_info = localtime(&now);
                strftime(new_request->timestamp, 127, "%Y-%m-%d %H:%M:%S", tm_info);
                
                g_num_access_requests++;
                
                pthread_mutex_unlock(&g_system_mutex);
                
                printf("  Access request created successfully.\n");
                log_to_file(client_addr_str, username, "RES: REQUEST_ACCESS for '%s' with permission '%c' success.", filename, requested_perm);
                snprintf(err_msg, sizeof(err_msg), "SUCCESS: Access request sent to owner.\n");
                write(client_socket, err_msg, strlen(err_msg));

            } else if (strncmp(buffer, "APPROVE_REQUEST", 15) == 0) {
                char filename[256], requester[256];
                if (sscanf(buffer, "APPROVE_REQUEST %s %s", filename, requester) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid APPROVE_REQUEST format. Use: APPROVE_REQUEST <filename> <requester_username>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                printf("Client '%s' approving request from '%s' for '%s'\n", username, requester, filename);
                log_to_file(client_addr_str, username, "REQ: APPROVE_REQUEST for '%s' by '%s'", filename, requester);

                pthread_mutex_lock(&g_system_mutex);
                
                // Find the request
                int request_index = -1;
                for (int i = 0; i < g_num_access_requests; i++) {
                    if (strcmp(g_access_requests[i].filename, filename) == 0 &&
                        strcmp(g_access_requests[i].requester_username, requester) == 0 &&
                        strcmp(g_access_requests[i].owner_username, username) == 0 &&
                        g_access_requests[i].status == 0) { // Pending
                        request_index = i;
                        break;
                    }
                }
                
                if (request_index == -1) {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  Request not found.\n");
                    log_to_file(client_addr_str, username, "RES: APPROVE_REQUEST failed: Request not found.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR: No pending request found.\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                // Get the file
                FileLocation* file = cache_get_unsafe(filename);
                if (file == NULL) {
                    file = hash_map_find_unsafe(filename);
                    if (file != NULL) cache_put_unsafe(filename, file);
                }
                
                if (file == NULL) {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  File not found.\n");
                    log_to_file(client_addr_str, username, "RES: APPROVE_REQUEST failed: File not found.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                // Grant permission
                char requested_perm = g_access_requests[request_index].requested_permission;
                
                // Check if user already has an ACL entry
                int acl_index = -1;
                for (int i = 0; i < file->num_permissions; i++) {
                    if (strcmp(file->acl[i].username, requester) == 0) {
                        acl_index = i;
                        break;
                    }
                }
                
                if (acl_index != -1) {
                    // Update existing permission
                    if (requested_perm == 'W' || file->acl[acl_index].permission == 'R') {
                        file->acl[acl_index].permission = requested_perm;
                    }
                } else if (file->num_permissions < MAX_PERMISSIONS) {
                    // Add new permission
                    strncpy(file->acl[file->num_permissions].username, requester, 255);
                    file->acl[file->num_permissions].username[255] = '\0';
                    file->acl[file->num_permissions].permission = requested_perm;
                    file->num_permissions++;
                } else {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  Maximum permissions reached for file.\n");
                    log_to_file(client_addr_str, username, "RES: APPROVE_REQUEST failed: Max permissions reached.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Maximum permissions reached for this file.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                // Mark request as approved
                g_access_requests[request_index].status = 1;
                
                // Persist changes
                save_registry_to_disk_unsafe();
                
                pthread_mutex_unlock(&g_system_mutex);
                
                printf("  Request approved successfully.\n");
                log_to_file(client_addr_str, username, "RES: APPROVE_REQUEST for '%s' by '%s' success.", filename, requester);
                snprintf(err_msg, sizeof(err_msg), "SUCCESS: Access granted.\n");
                write(client_socket, err_msg, strlen(err_msg));

            } else if (strncmp(buffer, "REJECT_REQUEST", 14) == 0) {
                char filename[256], requester[256];
                if (sscanf(buffer, "REJECT_REQUEST %s %s", filename, requester) != 2) {
                    snprintf(err_msg, sizeof(err_msg), "ERROR: Invalid REJECT_REQUEST format. Use: REJECT_REQUEST <filename> <requester_username>\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                printf("Client '%s' rejecting request from '%s' for '%s'\n", username, requester, filename);
                log_to_file(client_addr_str, username, "REQ: REJECT_REQUEST for '%s' by '%s'", filename, requester);

                pthread_mutex_lock(&g_system_mutex);
                
                // Find the request
                int request_index = -1;
                for (int i = 0; i < g_num_access_requests; i++) {
                    if (strcmp(g_access_requests[i].filename, filename) == 0 &&
                        strcmp(g_access_requests[i].requester_username, requester) == 0 &&
                        strcmp(g_access_requests[i].owner_username, username) == 0 &&
                        g_access_requests[i].status == 0) { // Pending
                        request_index = i;
                        break;
                    }
                }
                
                if (request_index == -1) {
                    pthread_mutex_unlock(&g_system_mutex);
                    printf("  Request not found.\n");
                    log_to_file(client_addr_str, username, "RES: REJECT_REQUEST failed: Request not found.");
                    snprintf(err_msg, sizeof(err_msg), "ERROR: No pending request found.\n");
                    write(client_socket, err_msg, strlen(err_msg));
                    continue;
                }
                
                // Mark request as rejected
                g_access_requests[request_index].status = 2;
                
                pthread_mutex_unlock(&g_system_mutex);
                
                printf("  Request rejected successfully.\n");
                log_to_file(client_addr_str, username, "RES: REJECT_REQUEST for '%s' by '%s' success.", filename, requester);
                snprintf(err_msg, sizeof(err_msg), "SUCCESS: Request rejected.\n");
                write(client_socket, err_msg, strlen(err_msg));

            } else {
                printf("Client sent unknown command: %s\n", buffer);
                log_to_file(client_addr_str, username, "WARN: Unknown command: %s", buffer);
                snprintf(err_msg, sizeof(err_msg), "ERROR: Unknown command.\n");
                write(client_socket, err_msg, strlen(err_msg));
            }
        }
    } else {
        printf("Unknown connection type. Closing.\n");
        log_to_file(client_addr_str, "Unknown", "WARN: Unknown connection type. First line: %s", buffer);
    }

    // --- (Client Disconnect Logic) ---
    pthread_mutex_lock(&g_system_mutex);
    int client_index = -1;
    char disconnected_username[256] = "Unknown";
    for (int i = 0; i < g_num_clients; i++) {
        if (client_list[i].client_socket_fd == client_socket) {
            client_index = i;
            strncpy(disconnected_username, client_list[i].username, 255);
            break;
        }
    }

    if (client_index != -1) {
        printf("User '%s' disconnected. Removing from list.\n", disconnected_username);
        log_to_file(client_addr_str, disconnected_username, "INFO: Client disconnected. Removing from list.");
        
        for (int i = g_num_locks - 1; i >= 0; i--) { // Iterate backwards
            if (strcmp(g_file_locks[i].username, disconnected_username) == 0) {
                printf("  User disconnected, releasing lock on %s (sent %d)\n", g_file_locks[i].filename, g_file_locks[i].sentence_num);
                log_to_file(client_addr_str, disconnected_username, "INFO: Auto-releasing lock on %s (sent %d).", g_file_locks[i].filename, g_file_locks[i].sentence_num);
                for (int j = i; j < g_num_locks - 1; j++) {
                    g_file_locks[j] = g_file_locks[j + 1];
                }
                g_num_locks--;
            }
        }
        
        for (int i = client_index; i < g_num_clients - 1; i++) {
            client_list[i] = client_list[i + 1];
        }
        g_num_clients--;
    }
    pthread_mutex_unlock(&g_system_mutex);

    close(client_socket);
    printf("Connection from %s closed.\n", client_addr_str);
    log_to_file(client_addr_str, disconnected_username, "INFO: Connection closed.");
    return NULL;
}

/*
 * --- Heartbeat Monitoring Thread ---
 */
void* monitor_heartbeats(void* arg) {
    (void)arg;
    
    while (1) {
        sleep(HEARTBEAT_INTERVAL);
        
        pthread_mutex_lock(&g_system_mutex);
        time_t current_time = time(NULL);
        
        for (int i = 0; i < g_num_servers; i++) {
            StorageServer* ss = &server_list[i];
            if (ss->is_online) {
                if (current_time - ss->last_heartbeat > HEARTBEAT_TIMEOUT) {
                    ss->is_online = 0;
                    printf("SS%d marked as OFFLINE (no heartbeat for %ld seconds)\n", 
                           i, current_time - ss->last_heartbeat);
                    log_to_file("Internal", "NameServer", 
                                "WARNING: SS%d marked OFFLINE (no heartbeat for %ld seconds)", 
                                i, current_time - ss->last_heartbeat);
                }
            }
        }
        
        pthread_mutex_unlock(&g_system_mutex);
    }
    
    return NULL;
}

/*
 * --- Main Server Function ---
 */
int main() {
    // Initialize hash map buckets to NULL
    for (int i = 0; i < HASH_MAP_SIZE; i++) {
        g_file_hash_map[i] = NULL;
    }
    
    // Initialize cache map buckets to NULL
    for (int i = 0; i < CACHE_MAP_SIZE; i++) {
        g_cache_map[i] = NULL;
    }

    load_registry_from_disk(); // Load persistent data
    
    // Start heartbeat monitoring thread
    pthread_t heartbeat_thread;
    if (pthread_create(&heartbeat_thread, NULL, monitor_heartbeats, NULL) != 0) {
        perror("pthread_create for heartbeat monitor failed");
        log_to_file("Internal", "NameServer", "ERROR: pthread_create for heartbeat monitor failed: %m");
        exit(EXIT_FAILURE);
    }
    pthread_detach(heartbeat_thread);
    printf("Heartbeat monitoring thread started\n");
    log_to_file("Internal", "NameServer", "INFO: Heartbeat monitoring thread started");
    
    int server_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        log_to_file("Internal", "NameServer", "CRITICAL: socket() failed: %m");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        log_to_file("Internal", "NameServer", "CRITICAL: setsockopt() failed: %m");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; 
    server_addr.sin_port = htons(NAME_SERVER_PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        log_to_file("Internal", "NameServer", "CRITICAL: bind() failed: %m");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) { 
        perror("listen failed");
        log_to_file("Internal", "NameServer", "CRITICAL: listen() failed: %m");
        exit(EXIT_FAILURE);
    }

    printf("Name Server listening on port %d\n", NAME_SERVER_PORT);
    log_to_file("Internal", "NameServer", "INFO: Name Server listening on port %d", NAME_SERVER_PORT);

    while (1) {
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            perror("accept failed");
            log_to_file("Internal", "NameServer", "ERROR: accept() failed: %m");
            continue; 
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        // Don't print here - let handle_connection print based on connection type
        // (to avoid spam from heartbeats)

        pthread_t thread_id;
        int* p_client_socket = malloc(sizeof(int));
        *p_client_socket = client_socket;

        if (pthread_create(&thread_id, NULL, handle_connection, (void*)p_client_socket) != 0) {
            perror("pthread_create failed");
            log_to_file("Internal", "NameServer", "ERROR: pthread_create failed: %m");
            free(p_client_socket);
            close(client_socket);
        }
    }

    close(server_fd);
    return 0;
}