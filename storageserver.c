#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <ctype.h>     // For isspace()
#include <sys/stat.h>  // For stat(), mkdir()
#include <time.h>      // For strftime()
#include <stdarg.h>    // For va_list
#include <dirent.h>    // For opendir(), readdir()
#include <errno.h>     // For errno, EEXIST, ENOTDIR
#include "common.h" 
// --- Define This Storage Server's Details ---
// Default ports (can be overridden by command-line arguments)
int SS_NM_PORT = 9001;       // Port for NM to connect to
int SS_CLIENT_PORT = 9002;   // Port for Clients to connect to
char SS_STORAGE_DIR[256] = "ss_storage/";  // Storage directory

// Use module-provided types, logging, and document helpers
#include "ss_modules/ss_types.h"
#include "ss_modules/ss_logging.h"
#include "ss_modules/ss_document.h"

// Forward Declarations (implemented later in this file)
void* handle_nm_command(void* arg);
void* start_client_server(void* arg);
void* start_nm_server(void* arg);

/*
 * Helper function to create directories recursively (like mkdir -p)
 * Returns 0 on success, -1 on failure
 */
int mkdir_recursive(const char* path, mode_t mode) {
    char tmp[1024];
    char* p = NULL;
    size_t len;
    struct stat st;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    // Remove trailing slash if present
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    // Iterate through the path and create each directory
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0; // Temporarily truncate
            
            // Check if directory exists
            if (stat(tmp, &st) != 0) {
                // Directory doesn't exist, create it
                if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                    return -1;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                // Path exists but is not a directory
                errno = ENOTDIR;
                return -1;
            }
            
            *p = '/'; // Restore the slash
        }
    }
    
    // Create the final directory
    if (stat(tmp, &st) != 0) {
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    
    return 0;
}

/*
 * Helper function to copy a file
 * Returns 0 on success, -1 on failure
 */
int copy_file(const char* src_path, const char* dest_path) {
    FILE* src = fopen(src_path, "rb");
    if (!src) {
        return -1;
    }
    
    FILE* dest = fopen(dest_path, "wb");
    if (!dest) {
        fclose(src);
        return -1;
    }
    
    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dest) != bytes) {
            fclose(src);
            fclose(dest);
            return -1;
        }
    }
    
    fclose(src);
    fclose(dest);
    return 0;
}

/*
 * Helper function to construct checkpoint file path
 * Format: ss_storage/<filename>.checkpoint.<tag>
 */
void get_checkpoint_path(const char* filename, const char* tag, char* checkpoint_path, size_t size) {
    // Extract directory path and base filename
    const char* last_slash = strrchr(filename, '/');
    if (last_slash) {
        // File is in a subdirectory
        size_t dir_len = last_slash - filename + 1;
        char dir_path[1024];
        snprintf(dir_path, sizeof(dir_path), "%s%.*s", SS_STORAGE_DIR, (int)dir_len, filename);
        const char* base_filename = last_slash + 1;
        snprintf(checkpoint_path, size, "%s.checkpoint.%s", dir_path, base_filename);
        // Now append the tag
        char temp[2048];
        snprintf(temp, sizeof(temp), "%s%s", checkpoint_path, tag);
        snprintf(checkpoint_path, size, "%s", temp);
    } else {
        // File is in root storage directory
        snprintf(checkpoint_path, size, "%s%s.checkpoint.%s", SS_STORAGE_DIR, filename, tag);
    }
}

/*
 * Thread function to handle a direct connection from a Client
 */
void* handle_client_request(void* arg) {
    int client_socket = *((int*)arg);
    free(arg);

    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    getpeername(client_socket, (struct sockaddr*)&peer_addr, &peer_len);
    
    char peer_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip));
    int peer_port = ntohs(peer_addr.sin_port);
    char client_addr_str[INET_ADDRSTRLEN + 7];
    snprintf(client_addr_str, sizeof(client_addr_str), "%s:%d", peer_ip, peer_port);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        printf("SS (Client-Handler): Client %s disconnected before request.\n", client_addr_str);
        log_to_file(client_addr_str, "Client", "Connection closed before sending request.");
        close(client_socket);
        return NULL;
    }
    buffer[bytes_read] = '\0';

    char filename[256];
    int sentence_num, word_index;
    char err_msg[256]; // For sending error messages
    
    if (sscanf(buffer, "READ_FILE %s", filename) == 1) {
        printf("SS (Client-Handler): Client requested to read '%s'\n", filename);
        log_to_file(client_addr_str, "Client", "REQ: READ_FILE for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        
        FILE* file = fopen(file_path, "r");
        if (file == NULL) {
            perror("SS (Client-Handler): fopen failed");
            log_to_file(client_addr_str, "Client", "RES: READ_FILE for '%s' failed: File not found.", filename);
            snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
            write(client_socket, err_msg, strlen(err_msg));
        } else {
            char file_buffer[BUFFER_SIZE];
            size_t bytes_read_from_file;
            while ((bytes_read_from_file = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
                if (write(client_socket, file_buffer, bytes_read_from_file) < 0) {
                    perror("SS (Client-Handler): write to client failed");
                    log_to_file(client_addr_str, "Client", "WARN: Write to client failed during READ_FILE for '%s'.", filename);
                    break; 
                }
            }
            fclose(file);
            printf("SS (Client-Handler): File '%s' sent successfully.\n", filename);
            log_to_file(client_addr_str, "Client", "RES: Successfully sent file '%s'.", filename);
        }

    } else if (sscanf(buffer, "STREAM_FILE %s", filename) == 1) {
        printf("SS (Client-Handler): Client requested to stream '%s'\n", filename);
        log_to_file(client_addr_str, "Client", "REQ: STREAM_FILE for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        
        FILE* file = fopen(file_path, "r");
        if (file == NULL) {
            perror("SS (Client-Handler): fopen failed");
            log_to_file(client_addr_str, "Client", "RES: STREAM_FILE for '%s' failed: File not found.", filename);
            snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
            write(client_socket, err_msg, strlen(err_msg));
        } else {
            char word_buffer[256]; 
            while (fscanf(file, "%255s", word_buffer) == 1) {
                if (write(client_socket, word_buffer, strlen(word_buffer)) < 0) break;
                if (write(client_socket, " ", 1) < 0) break;
                usleep(100000);
            }
            write(client_socket, "\n", 1);
            fclose(file);
            printf("SS (Client-Handler): File '%s' streamed successfully.\n", filename);
            log_to_file(client_addr_str, "Client", "RES: Successfully streamed file '%s'.", filename);
        }

    } else if (sscanf(buffer, "WRITE_START %s %d", filename, &sentence_num) == 2) {
        printf("SS (Client-Handler): Client started WRITE for '%s' (sent %d)\n", filename, sentence_num);
        log_to_file(client_addr_str, "Client", "REQ: WRITE_START for '%s', sentence %d", filename, sentence_num);
        
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);

        FILE* file_ro = fopen(file_path, "r");
        if (file_ro == NULL) {
            perror("SS (Write): Failed to open file for parsing");
            log_to_file(client_addr_str, "Client", "RES: WRITE_START for '%s' failed: File not found.", filename);
            snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
            write(client_socket, err_msg, strlen(err_msg));
            close(client_socket);
            return NULL;
        }
        WordNode* doc_head = parse_file_to_list(file_ro);
        fclose(file_ro);

        WordNode* target_sentence = get_sentence(doc_head, sentence_num);
        if (target_sentence == NULL && !(doc_head == NULL && sentence_num == 0)) { 
            WordNode* prev_sentence = get_sentence(doc_head, sentence_num - 1);
            if (prev_sentence == NULL) {
                printf("SS (Write): ERROR: Sentence index %d out of range.\n", sentence_num);
                log_to_file(client_addr_str, "Client", "RES: WRITE_START for '%s' failed: Sentence index %d out of range.", filename, sentence_num);
                snprintf(err_msg, sizeof(err_msg), "ERROR %d: Sentence index out of range.\n", ERROR_INVALID_INDEX);
                write(client_socket, err_msg, strlen(err_msg));
                free_document(doc_head);
                close(client_socket);
                return NULL;
            }
        }

        write(client_socket, "ACK_WRITE_START\n", 16);
        log_to_file(client_addr_str, "Client", "ACK: WRITE_START for '%s'. Entering session.", filename);

        while ((bytes_read = read(client_socket, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            buffer[strcspn(buffer, "\n")] = 0; // Remove newline

            if (strncmp(buffer, "ETIRW", 5) == 0) {
                printf("SS (Write): Received ETIRW. Finalizing changes.\n");
                log_to_file(client_addr_str, "Client", "REQ: ETIRW for '%s'. Finalizing changes.", filename);
                
                char bak_file_path[1024];
                snprintf(bak_file_path, sizeof(bak_file_path), "%s%s.bak", SS_STORAGE_DIR, filename);
                
                if (rename(file_path, bak_file_path) != 0) {
                    printf("SS (Write): Warning: Failed to create backup file\n");
                    log_to_file(client_addr_str, "Client", "WARN: Failed to create .bak for '%s'. UNDO will not be available.", filename);
                    perror("SS (Write): rename failed");
                }
                
                FILE* file_w = fopen(file_path, "w");
                if (file_w == NULL) {
                    perror("SS (Write): Failed to open file for writing");
                    log_to_file(client_addr_str, "Client", "CRITICAL: Failed to open '%s' for final write. Changes lost.", filename);
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Failed to save changes.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    break;
                }
                
                flatten_list_to_file(doc_head, file_w);
                fclose(file_w);
                
                write(client_socket, "ACK_WRITE_SUCCESS\n", sizeof("ACK_WRITE_SUCCESS\n") - 1);
                log_to_file(client_addr_str, "Client", "ACK: Successfully saved changes to '%s'.", filename);
                break; 
            }
            
            char* first_space = strchr(buffer, ' ');
            if (first_space == NULL) {
                printf("SS (Write): Invalid format. Got: %s\n", buffer);
                log_to_file(client_addr_str, "Client", "WARN: WRITE for '%s': Invalid format from client: %s", filename, buffer);
                snprintf(err_msg, sizeof(err_msg), "ERROR %d: Invalid format. Use: <index> <content>\n", ERROR_INVALID_INDEX);
                write(client_socket, err_msg, strlen(err_msg));
                continue; 
            }

            *first_space = '\0';
            char* content = first_space + 1;
            word_index = atoi(buffer); 

            if (word_index < 0) {
                printf("SS (Write): Invalid index %d.\n", word_index);
                log_to_file(client_addr_str, "Client", "WARN: WRITE for '%s': Invalid negative index: %d", filename, word_index);
                snprintf(err_msg, sizeof(err_msg), "ERROR %d: Index cannot be negative.\n", ERROR_INVALID_INDEX);
                write(client_socket, err_msg, strlen(err_msg));
                continue; 
            }
            
            printf("SS (Write): Updating sent %d, word %d with '%s'\n", sentence_num, word_index, content);
            
            WordNode* new_head = insert_word_at(doc_head, sentence_num, word_index, content);
            
            if (new_head == NULL) {
                log_to_file(client_addr_str, "Client", "RES: WRITE for '%s' failed: Invalid index (sent %d, word %d).", filename, sentence_num, word_index);
                snprintf(err_msg, sizeof(err_msg), "ERROR %d: Invalid word or sentence index.\n", ERROR_INVALID_INDEX);
                write(client_socket, err_msg, strlen(err_msg));
            } else {
                doc_head = new_head; // Success
                log_to_file(client_addr_str, "Client", "ACK: WRITE for '%s' success (sent %d, word %d).", filename, sentence_num, word_index);
                write(client_socket, "ACK_UPDATE_OK\n", 14);
            }
        }
        
        free_document(doc_head);
        printf("SS (Write): Session for '%s' ended.\n", filename);
        log_to_file(client_addr_str, "Client", "INFO: Write session for '%s' ended.", filename);

    } else {
        printf("SS (Client-Handler): Unknown command '%s'\n", buffer);
        log_to_file(client_addr_str, "Client", "WARN: Unknown command: %s", buffer);
    }
    close(client_socket);
    log_to_file(client_addr_str, "Client", "INFO: Connection closed.");
    return NULL;
}

/*
 * Main loop for the SS to listen for direct Client connections
 */
void* start_client_server(void* arg) {
    int server_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // --- **** MODIFIED **** ---
    printf("SS: Starting client-facing server on port %d\n", SS_CLIENT_PORT);
    log_to_file("Internal", "System", "INFO: Starting client-facing server on port %d", SS_CLIENT_PORT);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("SS (Client-Server): socket failed");
        log_to_file("Internal", "System", "CRITICAL: (Client-Server) socket failed: %m"); // %m prints errno string
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SS_CLIENT_PORT);
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("SS (Client-Server): bind failed");
        log_to_file("Internal", "System", "CRITICAL: (Client-Server) bind failed: %m");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("SS (Client-Server): listen failed");
        log_to_file("Internal", "System", "CRITICAL: (Client-Server) listen failed: %m");
        exit(EXIT_FAILURE);
    }
    // --- **** END OF MODIFICATION **** ---

    while (1) {
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            perror("SS (Client-Server): accept failed");
            log_to_file("Internal", "System", "ERROR: (Client-Server) accept failed: %m");
            continue;
        }
        
        // --- **** MODIFIED **** ---
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("SS (Client-Server): Accepted new connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));
        
        char client_addr_str[INET_ADDRSTRLEN + 7];
        snprintf(client_addr_str, sizeof(client_addr_str), "%s:%d", client_ip, ntohs(client_addr.sin_port));
        log_to_file(client_addr_str, "Client", "INFO: Accepted new connection.");
        // --- **** END OF MODIFICATION **** ---

        pthread_t thread_id;
        int* p_client_socket = malloc(sizeof(int));
        *p_client_socket = client_socket;
        if (pthread_create(&thread_id, NULL, handle_client_request, (void*)p_client_socket) != 0) {
            perror("SS (Client-Server): pthread_create failed");
            log_to_file(client_addr_str, "Client", "ERROR: (Client-Server) pthread_create failed: %m");
            free(p_client_socket);
            close(client_socket);
        }
    }
    return NULL;
}


// --- FUNCTIONS FOR NAME SERVER COMMANDS ---

/*
 * Thread function to handle a connection from the Name Server
 */
void* handle_nm_command(void* arg) {
    int nm_socket = *((int*)arg);
    free(arg);

    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    getpeername(nm_socket, (struct sockaddr*)&peer_addr, &peer_len);
    
    char peer_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip));
    int peer_port = ntohs(peer_addr.sin_port);
    char nm_addr_str[INET_ADDRSTRLEN + 7];
    snprintf(nm_addr_str, sizeof(nm_addr_str), "%s:%d", peer_ip, peer_port);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    bytes_read = read(nm_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        printf("SS (NM-Handler): NM %s disconnected before request.\n", nm_addr_str);
        log_to_file(nm_addr_str, "NameServer", "Connection closed before sending request.");
        close(nm_socket);
        return NULL;
    }
    buffer[bytes_read] = '\0';
    char command[64], filename[256];
    if (sscanf(buffer, "%s %s", command, filename) != 2) {
        printf("SS (NM-Handler): Invalid command format from NM.\n");
        log_to_file(nm_addr_str, "NameServer", "ERROR: Invalid command format from NM: %s", buffer);
        close(nm_socket);
        return NULL;
    }

    if (strcmp(command, "CREATE_FILE") == 0) {
        printf("SS (NM-Handler): NM requested to create '%s'\n", filename);
        log_to_file(nm_addr_str, "NameServer", "REQ: CREATE_FILE for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        FILE* file = fopen(file_path, "w");
        if (file == NULL) {
            perror("SS (NM-Handler): fopen failed");
            log_to_file(nm_addr_str, "NameServer", "RES: CREATE_FILE for '%s' failed: fopen failed.", filename);
            write(nm_socket, "ACK_CREATE_FAIL\n", sizeof("ACK_CREATE_FAIL\n") - 1);
        } else {
            fclose(file);
            printf("SS (NM-Handler): File '%s' created successfully.\n", filename);
            log_to_file(nm_addr_str, "NameServer", "RES: CREATE_FILE for '%s' success.", filename);
            write(nm_socket, "ACK_CREATE_SUCCESS\n", sizeof("ACK_CREATE_SUCCESS\n") - 1);
        }
    } else if (strcmp(command, "DELETE_FILE") == 0) {
        printf("SS (NM-Handler): NM requested to delete '%s'\n", filename);
        log_to_file(nm_addr_str, "NameServer", "REQ: DELETE_FILE for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        if (remove(file_path) == 0) {
            printf("SS (NM-Handler): File '%s' deleted successfully.\n", filename);
            log_to_file(nm_addr_str, "NameServer", "RES: DELETE_FILE for '%s' success.", filename);
            write(nm_socket, "ACK_DELETE_SUCCESS\n", sizeof("ACK_DELETE_SUCCESS\n") - 1);
        } else {
            perror("SS (NM-Handler): remove failed");
            log_to_file(nm_addr_str, "NameServer", "RES: DELETE_FILE for '%s' failed: remove failed.", filename);
            write(nm_socket, "ACK_DELETE_FAIL\n", sizeof("ACK_DELETE_FAIL\n") - 1);
        }
    } else if (strcmp(command, "GET_METADATA") == 0) {
        printf("SS (NM-Handler): NM requested metadata for '%s'\n", filename);
        log_to_file(nm_addr_str, "NameServer", "REQ: GET_METADATA for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        int words = 0, chars = 0;
        char created_ts[128], modified_ts[128];
        char response_buf[BUFFER_SIZE];
        if (get_file_metadata(file_path, &words, &chars, created_ts, modified_ts, 128) == 0) {
            snprintf(response_buf, sizeof(response_buf), 
                "METADATA_RESPONSE %d %d %s %s\n", 
                words, chars, created_ts, modified_ts);
            printf("  Sending metadata.\n");
            log_to_file(nm_addr_str, "NameServer", "RES: Sending metadata for '%s'", filename);
        } else {
            perror("SS (NM-Handler): get_file_metadata failed");
            log_to_file(nm_addr_str, "NameServer", "RES: GET_METADATA for '%s' failed.", filename);
            snprintf(response_buf, sizeof(response_buf), "METADATA_FAIL\n");
        }
        write(nm_socket, response_buf, strlen(response_buf));
    } else if (strcmp(command, "UNDO_FILE") == 0) {
        printf("SS (NM-Handler): NM requested to undo '%s'\n", filename);
        log_to_file(nm_addr_str, "NameServer", "REQ: UNDO_FILE for '%s'", filename);
        char file_path[1024];
        char bak_file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        snprintf(bak_file_path, sizeof(bak_file_path), "%s%s.bak", SS_STORAGE_DIR, filename);

        struct stat st;
        if (stat(bak_file_path, &st) != 0) {
            printf("  No backup file found at %s\n", bak_file_path);
            log_to_file(nm_addr_str, "NameServer", "RES: UNDO_FILE for '%s' failed: No .bak file found.", filename);
            write(nm_socket, "ACK_UNDO_FAIL_NO_BAK\n", 21);
        } else {
            if (remove(file_path) != 0) {
                perror("SS (NM-Handler): Failed to remove current file for undo");
                log_to_file(nm_addr_str, "NameServer", "RES: UNDO_FILE for '%s' failed: Could not remove current file.", filename);
                write(nm_socket, "ACK_UNDO_FAIL\n", 14);
            } else if (rename(bak_file_path, file_path) != 0) {
                perror("SS (NM-Handler): Failed to restore backup file");
                log_to_file(nm_addr_str, "NameServer", "RES: UNDO_FILE for '%s' failed: Could not restore .bak file.", filename);
                write(nm_socket, "ACK_UNDO_FAIL\n", 14);
            } else {
                printf("  Undo successful.\n");
                log_to_file(nm_addr_str, "NameServer", "RES: UNDO_FILE for '%s' success.", filename);
                write(nm_socket, "ACK_UNDO_SUCCESS\n", 17);
            }
        }
    } else if (strcmp(command, "CREATE_FOLDER") == 0) {
        printf("SS (NM-Handler): NM requested to create folder '%s'\n", filename);
        log_to_file(nm_addr_str, "NameServer", "REQ: CREATE_FOLDER for '%s'", filename);
        char folder_path[512];
        snprintf(folder_path, sizeof(folder_path), "%s%s", SS_STORAGE_DIR, filename);
        
        // Create directory recursively to support nested subfolders
        if (mkdir_recursive(folder_path, 0755) == 0) {
            printf("SS (NM-Handler): Folder '%s' created successfully.\n", filename);
            log_to_file(nm_addr_str, "NameServer", "RES: CREATE_FOLDER for '%s' success.", filename);
            write(nm_socket, "ACK_FOLDER_SUCCESS\n", sizeof("ACK_FOLDER_SUCCESS\n") - 1);
        } else {
            perror("SS (NM-Handler): mkdir_recursive failed");
            log_to_file(nm_addr_str, "NameServer", "RES: CREATE_FOLDER for '%s' failed: mkdir_recursive failed.", filename);
            write(nm_socket, "ACK_FOLDER_FAIL\n", sizeof("ACK_FOLDER_FAIL\n") - 1);
        }
    } else if (strcmp(command, "MOVE_FILE") == 0) {
        // Format: "MOVE_FILE <filename> <folder_path>"
        char folder_path[256];
        if (sscanf(buffer, "%s %s %s", command, filename, folder_path) != 3) {
            printf("SS (NM-Handler): Invalid MOVE_FILE format\n");
            log_to_file(nm_addr_str, "NameServer", "ERROR: Invalid MOVE_FILE format");
            write(nm_socket, "ACK_MOVE_FAIL\n", sizeof("ACK_MOVE_FAIL\n") - 1);
        } else {
            printf("SS (NM-Handler): NM requested to move '%s' to '%s'\n", filename, folder_path);
            log_to_file(nm_addr_str, "NameServer", "REQ: MOVE_FILE '%s' to '%s'", filename, folder_path);
            
            char old_path[2048], new_path[2048], dest_folder[2048];
            int ret;
            int move_failed = 0;
            
            ret = snprintf(old_path, sizeof(old_path), "%s%s", SS_STORAGE_DIR, filename);
            if (ret >= sizeof(old_path)) {
                fprintf(stderr, "SS (NM-Handler): old_path truncated\n");
                move_failed = 1;
            }
            
            if (!move_failed) {
                ret = snprintf(dest_folder, sizeof(dest_folder), "%s%s", SS_STORAGE_DIR, folder_path);
                if (ret >= sizeof(dest_folder)) {
                    fprintf(stderr, "SS (NM-Handler): dest_folder truncated\n");
                    move_failed = 1;
                }
            }
            
            if (!move_failed) {
                // Extract just the filename from the full path
                const char *base_filename = strrchr(filename, '/');
                if (base_filename) {
                    base_filename++; // Skip the '/'
                } else {
                    base_filename = filename;
                }
                ret = snprintf(new_path, sizeof(new_path), "%s/%s", dest_folder, base_filename);
                if (ret >= sizeof(new_path)) {
                    fprintf(stderr, "SS (NM-Handler): new_path truncated\n");
                    move_failed = 1;
                }
            }
            
            if (move_failed) {
                write(nm_socket, "ACK_MOVE_FAIL\n", sizeof("ACK_MOVE_FAIL\n") - 1);
            } else if (mkdir_recursive(dest_folder, 0755) != 0) {
                perror("SS (NM-Handler): mkdir_recursive failed for destination folder");
                log_to_file(nm_addr_str, "NameServer", "RES: MOVE_FILE '%s' failed: Could not create destination folder '%s'.", filename, folder_path);
                write(nm_socket, "ACK_MOVE_FAIL\n", sizeof("ACK_MOVE_FAIL\n") - 1);
            } else if (rename(old_path, new_path) == 0) {
                printf("SS (NM-Handler): File '%s' moved successfully to '%s'\n", filename, folder_path);
                log_to_file(nm_addr_str, "NameServer", "RES: MOVE_FILE '%s' to '%s' success.", filename, folder_path);
                write(nm_socket, "ACK_MOVE_SUCCESS\n", sizeof("ACK_MOVE_SUCCESS\n") - 1);
            } else {
                perror("SS (NM-Handler): rename failed");
                log_to_file(nm_addr_str, "NameServer", "RES: MOVE_FILE '%s' failed: rename failed.", filename);
                write(nm_socket, "ACK_MOVE_FAIL\n", sizeof("ACK_MOVE_FAIL\n") - 1);
            }
        }
    } else if (strcmp(command, "VIEW_FOLDER") == 0) {
        printf("SS (NM-Handler): NM requested to view folder '%s'\n", filename);
        log_to_file(nm_addr_str, "NameServer", "REQ: VIEW_FOLDER for '%s'", filename);
        
        char folder_path[512];
        snprintf(folder_path, sizeof(folder_path), "%s%s", SS_STORAGE_DIR, filename);
        
        // List directory contents
        DIR* dir = opendir(folder_path);
        if (dir == NULL) {
            perror("SS (NM-Handler): opendir failed");
            log_to_file(nm_addr_str, "NameServer", "RES: VIEW_FOLDER for '%s' failed: opendir failed.", filename);
            write(nm_socket, "ERROR: Folder not found\n", 24);
        } else {
            char response_buf[BUFFER_SIZE] = "Files in folder:\n";
            int offset = strlen(response_buf);
            struct dirent* entry;
            
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                offset += snprintf(response_buf + offset, BUFFER_SIZE - offset, "%s\n", entry->d_name);
                if (offset >= BUFFER_SIZE - 100) break; // Prevent overflow
            }
            closedir(dir);
            
            if (offset == strlen("Files in folder:\n")) {
                snprintf(response_buf + offset, BUFFER_SIZE - offset, "(empty)\n");
            }
            
            printf("  Sending folder listing.\n");
            log_to_file(nm_addr_str, "NameServer", "RES: VIEW_FOLDER for '%s' success.", filename);
            write(nm_socket, response_buf, strlen(response_buf));
        }
    } else if (strcmp(command, "CHECKPOINT") == 0) {
        // Format: "CHECKPOINT <filename> <checkpoint_tag>"
        char checkpoint_tag[128];
        if (sscanf(buffer, "%s %s %s", command, filename, checkpoint_tag) != 3) {
            printf("SS (NM-Handler): Invalid CHECKPOINT format\n");
            log_to_file(nm_addr_str, "NameServer", "ERROR: Invalid CHECKPOINT format");
            write(nm_socket, "ACK_CHECKPOINT_FAIL\n", sizeof("ACK_CHECKPOINT_FAIL\n") - 1);
        } else {
            printf("SS (NM-Handler): NM requested to create checkpoint '%s' for '%s'\n", checkpoint_tag, filename);
            log_to_file(nm_addr_str, "NameServer", "REQ: CHECKPOINT for '%s' with tag '%s'", filename, checkpoint_tag);
            
            char file_path[1024], checkpoint_path[2048];
            snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
            get_checkpoint_path(filename, checkpoint_tag, checkpoint_path, sizeof(checkpoint_path));
            
            // Check if source file exists
            struct stat st;
            if (stat(file_path, &st) != 0) {
                printf("SS (NM-Handler): File '%s' does not exist\n", filename);
                log_to_file(nm_addr_str, "NameServer", "RES: CHECKPOINT failed: File '%s' does not exist.", filename);
                write(nm_socket, "ACK_CHECKPOINT_FAIL\n", sizeof("ACK_CHECKPOINT_FAIL\n") - 1);
            } else if (copy_file(file_path, checkpoint_path) == 0) {
                printf("SS (NM-Handler): Checkpoint '%s' created successfully for '%s'\n", checkpoint_tag, filename);
                log_to_file(nm_addr_str, "NameServer", "RES: CHECKPOINT '%s' for '%s' success.", checkpoint_tag, filename);
                write(nm_socket, "ACK_CHECKPOINT_SUCCESS\n", sizeof("ACK_CHECKPOINT_SUCCESS\n") - 1);
            } else {
                perror("SS (NM-Handler): copy_file failed");
                log_to_file(nm_addr_str, "NameServer", "RES: CHECKPOINT failed: copy_file failed for '%s'.", filename);
                write(nm_socket, "ACK_CHECKPOINT_FAIL\n", sizeof("ACK_CHECKPOINT_FAIL\n") - 1);
            }
        }
    } else if (strcmp(command, "LISTCHECKPOINTS") == 0) {
        // Format: "LISTCHECKPOINTS <filename>"
        printf("SS (NM-Handler): NM requested to list checkpoints for '%s'\n", filename);
        log_to_file(nm_addr_str, "NameServer", "REQ: LISTCHECKPOINTS for '%s'", filename);
        
        // Construct search pattern for checkpoint files
        char search_pattern[1024];
        const char* last_slash = strrchr(filename, '/');
        const char* base_filename = last_slash ? (last_slash + 1) : filename;
        char dir_path[1024];
        
        if (last_slash) {
            size_t dir_len = last_slash - filename + 1;
            snprintf(dir_path, sizeof(dir_path), "%s%.*s", SS_STORAGE_DIR, (int)dir_len, filename);
        } else {
            snprintf(dir_path, sizeof(dir_path), "%s", SS_STORAGE_DIR);
        }
        
        snprintf(search_pattern, sizeof(search_pattern), "%s.checkpoint.", base_filename);
        
        DIR* dir = opendir(dir_path);
        if (!dir) {
            perror("SS (NM-Handler): opendir failed");
            log_to_file(nm_addr_str, "NameServer", "RES: LISTCHECKPOINTS failed: Could not open directory.");
            write(nm_socket, "ACK_LISTCHECKPOINTS_FAIL\n", sizeof("ACK_LISTCHECKPOINTS_FAIL\n") - 1);
        } else {
            char response_buf[BUFFER_SIZE];
            int offset = snprintf(response_buf, BUFFER_SIZE, "Checkpoints for %s:\n", filename);
            
            struct dirent* entry;
            int checkpoint_count = 0;
            while ((entry = readdir(dir)) != NULL) {
                if (strncmp(entry->d_name, search_pattern, strlen(search_pattern)) == 0) {
                    // Extract checkpoint tag from filename
                    const char* tag = entry->d_name + strlen(search_pattern);
                    offset += snprintf(response_buf + offset, BUFFER_SIZE - offset, "  - %s\n", tag);
                    checkpoint_count++;
                    if (offset >= BUFFER_SIZE - 100) break;
                }
            }
            closedir(dir);
            
            if (checkpoint_count == 0) {
                snprintf(response_buf + offset, BUFFER_SIZE - offset, "(no checkpoints)\n");
            }
            
            printf("  Sending checkpoint list (%d checkpoints).\n", checkpoint_count);
            log_to_file(nm_addr_str, "NameServer", "RES: LISTCHECKPOINTS for '%s' success (%d checkpoints).", filename, checkpoint_count);
            write(nm_socket, response_buf, strlen(response_buf));
        }
    } else if (strcmp(command, "VIEWCHECKPOINT") == 0) {
        // Format: "VIEWCHECKPOINT <filename> <checkpoint_tag>"
        char checkpoint_tag[128];
        if (sscanf(buffer, "%s %s %s", command, filename, checkpoint_tag) != 3) {
            printf("SS (NM-Handler): Invalid VIEWCHECKPOINT format\n");
            log_to_file(nm_addr_str, "NameServer", "ERROR: Invalid VIEWCHECKPOINT format");
            write(nm_socket, "ACK_VIEWCHECKPOINT_FAIL\n", sizeof("ACK_VIEWCHECKPOINT_FAIL\n") - 1);
        } else {
            printf("SS (NM-Handler): NM requested to view checkpoint '%s' for '%s'\n", checkpoint_tag, filename);
            log_to_file(nm_addr_str, "NameServer", "REQ: VIEWCHECKPOINT for '%s' with tag '%s'", filename, checkpoint_tag);
            
            char checkpoint_path[2048];
            get_checkpoint_path(filename, checkpoint_tag, checkpoint_path, sizeof(checkpoint_path));
            
            FILE* f = fopen(checkpoint_path, "r");
            if (!f) {
                perror("SS (NM-Handler): fopen failed for checkpoint");
                log_to_file(nm_addr_str, "NameServer", "RES: VIEWCHECKPOINT failed: Checkpoint '%s' does not exist.", checkpoint_tag);
                write(nm_socket, "ACK_VIEWCHECKPOINT_FAIL\n", sizeof("ACK_VIEWCHECKPOINT_FAIL\n") - 1);
            } else {
                char response_buf[BUFFER_SIZE];
                int offset = snprintf(response_buf, BUFFER_SIZE, "Checkpoint '%s' content:\n---\n", checkpoint_tag);
                
                size_t bytes_read = fread(response_buf + offset, 1, BUFFER_SIZE - offset - 10, f);
                offset += bytes_read;
                snprintf(response_buf + offset, BUFFER_SIZE - offset, "\n---\n");
                
                fclose(f);
                
                printf("  Sending checkpoint content (%zu bytes).\n", bytes_read);
                log_to_file(nm_addr_str, "NameServer", "RES: VIEWCHECKPOINT for '%s' tag '%s' success.", filename, checkpoint_tag);
                write(nm_socket, response_buf, strlen(response_buf));
            }
        }
    } else if (strcmp(command, "REVERT") == 0) {
        // Format: "REVERT <filename> <checkpoint_tag>"
        char checkpoint_tag[128];
        if (sscanf(buffer, "%s %s %s", command, filename, checkpoint_tag) != 3) {
            printf("SS (NM-Handler): Invalid REVERT format\n");
            log_to_file(nm_addr_str, "NameServer", "ERROR: Invalid REVERT format");
            write(nm_socket, "ACK_REVERT_FAIL\n", sizeof("ACK_REVERT_FAIL\n") - 1);
        } else {
            printf("SS (NM-Handler): NM requested to revert '%s' to checkpoint '%s'\n", filename, checkpoint_tag);
            log_to_file(nm_addr_str, "NameServer", "REQ: REVERT '%s' to checkpoint '%s'", filename, checkpoint_tag);
            
            char file_path[1024], checkpoint_path[2048];
            snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
            get_checkpoint_path(filename, checkpoint_tag, checkpoint_path, sizeof(checkpoint_path));
            
            // Check if checkpoint exists
            struct stat st;
            if (stat(checkpoint_path, &st) != 0) {
                printf("SS (NM-Handler): Checkpoint '%s' does not exist\n", checkpoint_tag);
                log_to_file(nm_addr_str, "NameServer", "RES: REVERT failed: Checkpoint '%s' does not exist.", checkpoint_tag);
                write(nm_socket, "ACK_REVERT_FAIL\n", sizeof("ACK_REVERT_FAIL\n") - 1);
            } else if (copy_file(checkpoint_path, file_path) == 0) {
                printf("SS (NM-Handler): File '%s' reverted to checkpoint '%s' successfully\n", filename, checkpoint_tag);
                log_to_file(nm_addr_str, "NameServer", "RES: REVERT '%s' to '%s' success.", filename, checkpoint_tag);
                write(nm_socket, "ACK_REVERT_SUCCESS\n", sizeof("ACK_REVERT_SUCCESS\n") - 1);
            } else {
                perror("SS (NM-Handler): copy_file failed during revert");
                log_to_file(nm_addr_str, "NameServer", "RES: REVERT failed: copy_file failed for '%s'.", filename);
                write(nm_socket, "ACK_REVERT_FAIL\n", sizeof("ACK_REVERT_FAIL\n") - 1);
            }
        }
    } else {
        printf("SS (NM-Handler): Unknown command from NM '%s'\n", command);
        log_to_file(nm_addr_str, "NameServer", "WARN: Unknown command from NM: %s", command);
        write(nm_socket, "ACK_UNKNOWN\n", sizeof("ACK_UNKNOWN\n") - 1);
    }
    close(nm_socket);
    log_to_file(nm_addr_str, "NameServer", "INFO: Connection closed.");
    return NULL;
}

/*
 * Main loop for the SS to listen for commands from the Name Server
 */
void* start_nm_server(void* arg) {
    int server_fd;
    struct sockaddr_in server_addr, nm_addr;
    socklen_t nm_len = sizeof(nm_addr);

    // --- **** MODIFIED **** ---
    printf("SS: Starting NM-facing server on port %d\n", SS_NM_PORT);
    log_to_file("Internal", "System", "INFO: Starting NM-facing server on port %d", SS_NM_PORT);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("SS (NM-Server): socket failed");
        log_to_file("Internal", "System", "CRITICAL: (NM-Server) socket failed: %m");
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SS_NM_PORT);
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("SS (NM-Server): bind failed");
        log_to_file("Internal", "System", "CRITICAL: (NM-Server) bind failed: %m");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("SS (NM-Server): listen failed");
        log_to_file("Internal", "System", "CRITICAL: (NM-Server) listen failed: %m");
        exit(EXIT_FAILURE);
    }
    // --- **** END OF MODIFICATION **** ---

    while (1) {
        int nm_socket = accept(server_fd, (struct sockaddr*)&nm_addr, &nm_len);
        if (nm_socket < 0) {
            perror("SS (NM-Server): accept failed");
            log_to_file("Internal", "System", "ERROR: (NM-Server) accept failed: %m");
            continue;
        }

        // --- **** MODIFIED **** ---
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &nm_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("SS (NM-Server): Accepted new connection from %s:%d\n", client_ip, ntohs(nm_addr.sin_port));

        char nm_addr_str[INET_ADDRSTRLEN + 7];
        snprintf(nm_addr_str, sizeof(nm_addr_str), "%s:%d", client_ip, ntohs(nm_addr.sin_port));
        log_to_file(nm_addr_str, "NameServer", "INFO: Accepted new connection.");
        // --- **** END OF MODIFICATION **** ---

        pthread_t thread_id;
        int* p_nm_socket = malloc(sizeof(int));
        *p_nm_socket = nm_socket;
        if (pthread_create(&thread_id, NULL, handle_nm_command, (void*)p_nm_socket) != 0) {
            perror("SS (NM-Server): pthread_create failed");
            log_to_file(nm_addr_str, "NameServer", "ERROR: (NM-Server) pthread_create failed: %m");
            free(p_nm_socket);
            close(nm_socket);
        }
    }
    return NULL;
}

// ---------------------------------------------

/*
 * Connects to the Name Server and sends a registration message.
 */
void register_with_name_server() {
    int sock;
    struct sockaddr_in nm_addr;
    char registration_msg[BUFFER_SIZE];
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("SS: socket creation failed");
        exit(EXIT_FAILURE);
    }
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NAME_SERVER_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) {
        perror("SS: invalid address");
        exit(EXIT_FAILURE);
    }
    if (connect(sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        perror("SS: connection to Name Server failed");
        exit(EXIT_FAILURE);
    }
    // --- **** MODIFIED **** ---
    printf("SS: Connected to Name Server.\n");
    log_to_file("127.0.0.1", "System", "INFO: (NM-Registration) Connected to Name Server.");
    
    int offset = sprintf(registration_msg, "REGISTER_SS %d %d", 
                         SS_NM_PORT, SS_CLIENT_PORT);
    
    // Dynamically scan storage directory for files
    DIR* dir = opendir(SS_STORAGE_DIR);
    if (dir != NULL) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            // Skip . and .. entries, and only include regular files
            if (entry->d_type == DT_REG) {
                offset += sprintf(registration_msg + offset, " %s", entry->d_name);
            }
        }
        closedir(dir);
    } else {
        log_to_file("127.0.0.1", "System", "WARN: (NM-Registration) Could not open storage directory: %s", SS_STORAGE_DIR);
    }
    
    sprintf(registration_msg + offset, "\n");
    
    if (write(sock, registration_msg, strlen(registration_msg)) < 0) {
        perror("SS: failed to send registration message");
        log_to_file("127.0.0.1", "System", "ERROR: (NM-Registration) failed to send registration message: %m");
    } else {
        printf("SS: Sent registration message:\n%s", registration_msg);
        log_to_file("127.0.0.1", "System", "INFO: (NM-Registration) Sent registration message.");
    }
    close(sock);
    printf("SS: Registration complete.\n");
    // --- **** END OF MODIFICATION **** ---
}

/*
 * --- Heartbeat Sending Thread ---
 */
void* send_heartbeats(void* arg) {
    (void)arg;
    
    while (1) {
        sleep(10); // HEARTBEAT_INTERVAL = 10 seconds
        
        int sock;
        struct sockaddr_in nm_addr;
        char heartbeat_msg[64];
        char response[32];
        
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("SS: heartbeat socket creation failed");
            continue;
        }
        
        nm_addr.sin_family = AF_INET;
        nm_addr.sin_port = htons(NAME_SERVER_PORT);
        if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) {
            close(sock);
            continue;
        }
        
        if (connect(sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
            close(sock);
            continue;
        }
        
        sprintf(heartbeat_msg, "HEARTBEAT %d\n", SS_NM_PORT);
        if (write(sock, heartbeat_msg, strlen(heartbeat_msg)) < 0) {
            perror("SS: failed to send heartbeat");
        } else {
            int n = read(sock, response, sizeof(response) - 1);
            if (n > 0) {
                response[n] = '\0';
            }
        }
        
        close(sock);
    }
    
    return NULL;
}

int main(int argc, char* argv[]) {
    // Parse command-line arguments: ./storageserver <nm_port> <client_port> [storage_dir]
    if (argc >= 3) {
        SS_NM_PORT = atoi(argv[1]);
        SS_CLIENT_PORT = atoi(argv[2]);
        if (argc >= 4) {
            snprintf(SS_STORAGE_DIR, sizeof(SS_STORAGE_DIR), "%s", argv[3]);
            // Ensure trailing slash
            int len = strlen(SS_STORAGE_DIR);
            if (len > 0 && SS_STORAGE_DIR[len-1] != '/') {
                strncat(SS_STORAGE_DIR, "/", sizeof(SS_STORAGE_DIR) - len - 1);
            }
        }
        printf("SS: Starting with NM Port=%d, Client Port=%d, Storage Dir=%s\n", 
               SS_NM_PORT, SS_CLIENT_PORT, SS_STORAGE_DIR);
    } else if (argc > 1) {
        printf("Usage: %s [nm_port client_port [storage_dir]]\n", argv[0]);
        printf("  nm_port      - Port for Name Server communication (default: 9001)\n");
        printf("  client_port  - Port for Client communication (default: 9002)\n");
        printf("  storage_dir  - Directory for file storage (default: ss_storage/)\n");
        printf("\nStarting with default ports...\n");
    }
    
    register_with_name_server();
    
    // Start heartbeat sending thread
    pthread_t heartbeat_thread_id;
    if (pthread_create(&heartbeat_thread_id, NULL, send_heartbeats, NULL) != 0) {
        perror("SS: Failed to create heartbeat thread");
        log_to_file("Internal", "System", "ERROR: Failed to create heartbeat thread: %m");
    } else {
        pthread_detach(heartbeat_thread_id);
        printf("SS: Heartbeat thread started\n");
        log_to_file("Internal", "System", "INFO: Heartbeat thread started");
    }
    
    pthread_t client_server_thread_id;
    if (pthread_create(&client_server_thread_id, NULL, start_client_server, NULL) != 0) {
        perror("SS: Failed to create client server thread");
        log_to_file("Internal", "System", "CRITICAL: Failed to create client server thread: %m");
        exit(EXIT_FAILURE);
    }
    
    // Start the NM-facing server in the main thread
    start_nm_server(NULL); 
    
    pthread_join(client_server_thread_id, NULL);
    return 0;
}