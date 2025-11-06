#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <ctype.h>     // For isspace()
#include <sys/stat.h>  // For stat()
#include <time.h>      // For strftime()

#include "common.h" 
#include <stdarg.h>


// --- Define This Storage Server's Details ---
#define SS_NM_PORT 9001       // Port for NM to connect to
#define SS_CLIENT_PORT 9002   // Port for Clients to connect to
#define SS_STORAGE_DIR "ss_storage/" // All files are in here

// --- **** ADD THESE **** ---
#define SS_LOG_FILE "storageserver.log"
pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
// --- **** END OF ADDITION **** ---

const char* my_files[] = {
    "wowee.txt",
    "nuh_uh.txt"
};
int num_files = 2;

// --- **** FIX: Forward Declarations **** ---
void* handle_nm_command(void* arg);
void* start_client_server(void* arg);
// --- **** END OF FIX **** ---


// --- **** WORD & SENTENCE LINKED LIST **** ---

typedef struct WordNode {
    char* word;
    struct WordNode* next_word;
    struct WordNode* next_sentence; // Only used by the first word of a sentence
} WordNode;

// Helper to create a new word node
WordNode* create_word_node(const char* word) {
    WordNode* new_node = (WordNode*)malloc(sizeof(WordNode));
    new_node->word = strdup(word);
    new_node->next_word = NULL;
    new_node->next_sentence = NULL;
    return new_node;
}

// Helper to check if a character is a sentence delimiter
int is_delimiter(char c) {
    return (c == '.' || c == '!' || c == '?');
}

/*
 * Frees the entire document structure
 */
void free_document(WordNode* doc_head) {
    WordNode* current_sentence = doc_head;
    while (current_sentence != NULL) {
        WordNode* current_word = current_sentence;
        WordNode* next_sentence = current_sentence->next_sentence;
        while (current_word != NULL) {
            WordNode* next_word = current_word->next_word;
            free(current_word->word);
            free(current_word);
            current_word = next_word;
        }
        current_sentence = next_sentence;
    }
}

/*
 * Parses the file content into the linked list structure
 * Returns the head of the first sentence.
 */
WordNode* parse_file_to_list(FILE* file) {
    WordNode* doc_head = NULL;
    WordNode* current_sentence_head = NULL;
    WordNode* current_word_node = NULL; // Tracks the *last node added*

    char buffer[256];
    int buffer_idx = 0;
    char c;

    while ((c = fgetc(file)) != EOF) {
        if (isspace(c) || is_delimiter(c)) {
            // We have a word boundary
            if (buffer_idx > 0) {
                if (is_delimiter(c)) { // Add delimiter to the word
                    buffer[buffer_idx++] = c;
                }
                buffer[buffer_idx] = '\0';
                
                WordNode* new_node = create_word_node(buffer);
                
                if (current_sentence_head == NULL) {
                    // This is the first word of a new sentence
                    current_sentence_head = new_node;
                    if (doc_head == NULL) {
                        doc_head = current_sentence_head;
                    } else {
                        // Link the previous sentence to this one
                        WordNode* temp_sent = doc_head;
                        while(temp_sent->next_sentence != NULL) {
                            temp_sent = temp_sent->next_sentence;
                        }
                        temp_sent->next_sentence = current_sentence_head;
                    }
                } else {
                    // This is a subsequent word in the same sentence
                    current_word_node->next_word = new_node;
                }
                current_word_node = new_node;
                buffer_idx = 0; // Reset buffer
            }
            
            if (is_delimiter(c)) {
                // This word was the end of a sentence
                current_sentence_head = NULL;
                current_word_node = NULL;
            }
        } else {
            // Regular character
            if (buffer_idx < 255) {
                buffer[buffer_idx++] = c;
            }
        }
    }
    // Handle any trailing word (if file doesn't end with delimiter/space)
    if (buffer_idx > 0) {
        buffer[buffer_idx] = '\0';
        WordNode* new_node = create_word_node(buffer);
        if (current_sentence_head == NULL) {
            current_sentence_head = new_node;
            if (doc_head == NULL) doc_head = current_sentence_head;
        } else {
            current_word_node->next_word = new_node;
        }
    }
    return doc_head;
}

/*
 * Flattens the linked list structure back into a file
 */
void flatten_list_to_file(WordNode* doc_head, FILE* file) {
    WordNode* current_sentence = doc_head;
    while (current_sentence != NULL) {
        WordNode* current_word = current_sentence;
        while (current_word != NULL) {
            fprintf(file, "%s", current_word->word);
            if (current_word->next_word != NULL) {
                fprintf(file, " "); // Add space between words
            }
            current_word = current_word->next_word;
        }
        current_sentence = current_sentence->next_sentence;
        if (current_sentence != NULL) {
            fprintf(file, " "); // Add space between sentences
        }
    }
}

/*
 * Helper to get the head node of the Nth sentence
 */
WordNode* get_sentence(WordNode* doc_head, int sentence_index) {
    WordNode* current_sentence = doc_head;
    for (int i = 0; i < sentence_index && current_sentence != NULL; i++) {
        current_sentence = current_sentence->next_sentence;
    }
    return current_sentence;
}

/*
 * Inserts a new word at the given index in a sentence.
 * Returns the new head of the document on success, or NULL on failure.
 */
WordNode* insert_word_at(WordNode* doc_head, int sentence_index, int word_index, const char* content) {
    
    // --- Fix for inserting into an empty document ---
    if (doc_head == NULL && sentence_index == 0 && word_index == 0) {
        printf("SS (Write): Inserting into empty doc\n");
        WordNode* new_word_node = create_word_node(content);
        doc_head = new_word_node; // This is now the head of the doc
        return doc_head;
    }
    
    WordNode* sentence_head = get_sentence(doc_head, sentence_index);
    
    // --- Special case: Appending a new sentence ---
    if (sentence_head == NULL && sentence_index > 0 && word_index == 0) {
        WordNode* prev_sentence = get_sentence(doc_head, sentence_index - 1);
        if (prev_sentence != NULL) {
             printf("SS (Write): Appending new sentence\n");
             WordNode* new_word_node = create_word_node(content);
             prev_sentence->next_sentence = new_word_node;
             return doc_head;
        } else {
             printf("SS (Write): ERROR: Invalid sentence index %d (non-contiguous)\n", sentence_index);
             return NULL; // <--- FIX: Return NULL on error
        }
    }
    
    if (sentence_head == NULL) {
        printf("SS (Write): ERROR: Invalid sentence index %d\n", sentence_index);
        return NULL; // <--- FIX: Return NULL on error
    }
    
    WordNode* new_word_node = create_word_node(content);

    if (word_index == 0) {
        // Insert at the beginning of the sentence
        new_word_node->next_word = sentence_head;
        new_word_node->next_sentence = sentence_head->next_sentence; 
        sentence_head->next_sentence = NULL; 
        
        if (sentence_index == 0) {
            doc_head = new_word_node; // New document head
        } else {
            WordNode* prev_sentence = get_sentence(doc_head, sentence_index - 1);
            prev_sentence->next_sentence = new_word_node;
        }
        return doc_head;
    }

    // Insert at index > 0
    WordNode* current_word = sentence_head;
    
    // --- **** FIX: Corrected loop for index checking **** ---
    int i = 0;
    // Loop to the (word_index - 1)th node
    for (i = 0; i < word_index - 1; i++) {
        if (current_word->next_word == NULL) {
            // We hit the end of the sentence early
            break;
        }
        current_word = current_word->next_word;
    }

    // Check *why* we stopped
    if (i != word_index - 1) {
        // We broke early, meaning the index is out of bounds
        printf("SS (Write): ERROR: Invalid word index %d (too large for sentence).\n", word_index);
        free(new_word_node->word);
        free(new_word_node);
        return NULL; // <--- FIX: Return NULL on error
    }
    // --- **** END OF FIX **** ---
    
    // If we're here, current_word is the (index - 1)th node. Insert after it.
    new_word_node->next_word = current_word->next_word;
    current_word->next_word = new_word_node;
    
    return doc_head;
}


// --- (get_file_metadata helper is unchanged) ---
int get_file_metadata(const char* file_path, int* word_count, int* char_count, 
                      char* created_ts, char* modified_ts, int ts_len) 
{
    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0) {
        perror("SS: stat failed");
        return -1;
    }
    *char_count = (int)file_stat.st_size; 

    FILE* file = fopen(file_path, "r");
    if (file == NULL) {
        perror("SS: fopen for word count failed");
        return -1;
    }
    *word_count = 0;
    int in_word = 0;
    char c;
    while ((c = fgetc(file)) != EOF) {
        if (isspace(c)) {
            in_word = 0;
        } else if (in_word == 0) {
            in_word = 1;
            (*word_count)++;
        }
    }
    fclose(file);

    struct tm *tm_info;
    tm_info = localtime(&file_stat.st_ctime);
    strftime(created_ts, ts_len, "%Y-%m-%d %H:%M", tm_info);
    tm_info = localtime(&file_stat.st_mtime);
    strftime(modified_ts, ts_len, "%Y-%m-%d %H:%M", tm_info);

    return 0;
}

/*
 * Writes a formatted message to the log file in a thread-safe way.
 */
void log_message(const char* format, ...) {
    va_list args;
    va_start(args, format);

    time_t now = time(NULL);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    pthread_mutex_lock(&g_log_mutex);
    
    FILE* log_file = fopen(SS_LOG_FILE, "a");
    if (log_file) {
        fprintf(log_file, "[%s] ", time_buf);
        vfprintf(log_file, format, args);
        fprintf(log_file, "\n");
        fclose(log_file);
    }
    
    pthread_mutex_unlock(&g_log_mutex);
    va_end(args);
}


/*
 * Thread function to handle a direct connection from a Client
 */
void* handle_client_request(void* arg) {
    int client_socket = *((int*)arg);
    free(arg);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        log_message("Client disconnected before sending request.");
        close(client_socket);
        return NULL;
    }
    buffer[bytes_read] = '\0';

    char filename[256];
    int sentence_num, word_index;
    char err_msg[256]; // For sending error messages
    
    if (sscanf(buffer, "READ_FILE %s", filename) == 1) {
        log_message("Client requested READ_FILE for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        
        FILE* file = fopen(file_path, "r");
        if (file == NULL) {
            log_message("Failed READ_FILE for '%s': File not found.", filename);
            snprintf(err_msg, sizeof(err_msg), "ERROR %d: File not found.\n", ERROR_FILE_NOT_FOUND);
            write(client_socket, err_msg, strlen(err_msg));
        } else {
            char file_buffer[BUFFER_SIZE];
            size_t bytes_read_from_file;
            while ((bytes_read_from_file = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
                if (write(client_socket, file_buffer, bytes_read_from_file) < 0) {
                    log_message("Write to client failed during READ_FILE for '%s'.", filename);
                    break; 
                }
            }
            fclose(file);
            log_message("Successfully sent file '%s'.", filename);
        }

    } else if (sscanf(buffer, "STREAM_FILE %s", filename) == 1) {
        log_message("Client requested STREAM_FILE for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        
        FILE* file = fopen(file_path, "r");
        if (file == NULL) {
            log_message("Failed STREAM_FILE for '%s': File not found.", filename);
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
            log_message("Successfully streamed file '%s'.", filename);
        }

    } else if (sscanf(buffer, "WRITE_START %s %d", filename, &sentence_num) == 2) {
        log_message("Client started WRITE_START for '%s', sentence %d", filename, sentence_num);
        
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);

        FILE* file_ro = fopen(file_path, "r");
        if (file_ro == NULL) {
            log_message("Failed WRITE_START for '%s': File not found.", filename);
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
                log_message("Failed WRITE_START for '%s': Sentence index %d out of range.", filename, sentence_num);
                snprintf(err_msg, sizeof(err_msg), "ERROR %d: Sentence index out of range.\n", ERROR_INVALID_INDEX);
                write(client_socket, err_msg, strlen(err_msg));
                free_document(doc_head);
                close(client_socket);
                return NULL;
            }
        }

        write(client_socket, "ACK_WRITE_START\n", 16);
        log_message("WRITE_START for '%s' acknowledged. Entering session.", filename);

        while ((bytes_read = read(client_socket, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            buffer[strcspn(buffer, "\n")] = 0; // Remove newline

            if (strncmp(buffer, "ETIRW", 5) == 0) {
                log_message("Received ETIRW for '%s'. Finalizing changes.", filename);
                
                char bak_file_path[512];
                snprintf(bak_file_path, sizeof(bak_file_path), "%s%s.bak", SS_STORAGE_DIR, filename);
                
                if (rename(file_path, bak_file_path) != 0) {
                    log_message("Warning: Failed to create .bak for '%s'. UNDO will not be available.", filename);
                    perror("SS (Write): rename failed");
                }
                
                FILE* file_w = fopen(file_path, "w");
                if (file_w == NULL) {
                    log_message("CRITICAL: Failed to open '%s' for final write. Changes lost.", filename);
                    perror("SS (Write): fopen failed");
                    snprintf(err_msg, sizeof(err_msg), "ERROR %d: Failed to save changes.\n", ERROR_SERVER_ERROR);
                    write(client_socket, err_msg, strlen(err_msg));
                    break;
                }
                
                flatten_list_to_file(doc_head, file_w);
                fclose(file_w);
                
                write(client_socket, "ACK_WRITE_SUCCESS\n", sizeof("ACK_WRITE_SUCCESS\n") - 1);
                log_message("Successfully saved changes to '%s'.", filename);
                break; 
            }
            
            char* first_space = strchr(buffer, ' ');
            if (first_space == NULL) {
                log_message("WRITE for '%s': Invalid format from client: %s", filename, buffer);
                snprintf(err_msg, sizeof(err_msg), "ERROR %d: Invalid format. Use: <index> <content>\n", ERROR_INVALID_INDEX);
                write(client_socket, err_msg, strlen(err_msg));
                continue; 
            }

            *first_space = '\0';
            char* content = first_space + 1;
            word_index = atoi(buffer); 

            if (word_index < 0) {
                log_message("WRITE for '%s': Invalid negative index: %d", filename, word_index);
                snprintf(err_msg, sizeof(err_msg), "ERROR %d: Index cannot be negative.\n", ERROR_INVALID_INDEX);
                write(client_socket, err_msg, strlen(err_msg));
                continue; 
            }
            
            WordNode* new_head = insert_word_at(doc_head, sentence_num, word_index, content);
            
            if (new_head == NULL) {
                log_message("WRITE for '%s': Invalid index (sent %d, word %d).", filename, sentence_num, word_index);
                snprintf(err_msg, sizeof(err_msg), "ERROR %d: Invalid word or sentence index.\n", ERROR_INVALID_INDEX);
                write(client_socket, err_msg, strlen(err_msg));
            } else {
                doc_head = new_head; // Success
                log_message("WRITE for '%s': Updated sent %d, word %d.", filename, sentence_num, word_index);
                write(client_socket, "ACK_UPDATE_OK\n", 14);
            }
        }
        
        free_document(doc_head);
        log_message("Write session for '%s' ended.", filename);

    } else {
        log_message("Unknown command from client: %s", buffer);
    }
    close(client_socket);
    return NULL;
}

/*
 * Main loop for the SS to listen for direct Client connections
 */
void* start_client_server(void* arg) {
    // (This function IS present)
    int server_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    printf("SS: Starting client-facing server on port %d\n", SS_CLIENT_PORT);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("SS (Client-Server): socket failed");
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SS_CLIENT_PORT);
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("SS (Client-Server): bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("SS (Client-Server): listen failed");
        exit(EXIT_FAILURE);
    }

    while (1) {
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            perror("SS (Client-Server): accept failed");
            continue;
        }
        pthread_t thread_id;
        int* p_client_socket = malloc(sizeof(int));
        *p_client_socket = client_socket;
        if (pthread_create(&thread_id, NULL, handle_client_request, (void*)p_client_socket) != 0) {
            perror("SS (Client-Server): pthread_create failed");
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
/*
 * Thread function to handle a connection from the Name Server
 */
void* handle_nm_command(void* arg) {
    int nm_socket = *((int*)arg);
    free(arg);
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    bytes_read = read(nm_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        log_message("NM disconnected before sending request.");
        close(nm_socket);
        return NULL;
    }
    buffer[bytes_read] = '\0';
    char command[64], filename[256];
    if (sscanf(buffer, "%s %s", command, filename) != 2) {
        log_message("Invalid command format from NM: %s", buffer);
        close(nm_socket);
        return NULL;
    }

    if (strcmp(command, "CREATE_FILE") == 0) {
        log_message("NM requested CREATE_FILE for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        FILE* file = fopen(file_path, "w");
        if (file == NULL) {
            log_message("Failed CREATE_FILE for '%s': fopen failed.", filename);
            write(nm_socket, "ACK_CREATE_FAIL\n", sizeof("ACK_CREATE_FAIL\n") - 1);
        } else {
            fclose(file);
            log_message("Successfully created file '%s'.", filename);
            write(nm_socket, "ACK_CREATE_SUCCESS\n", sizeof("ACK_CREATE_SUCCESS\n") - 1);
        }
    } else if (strcmp(command, "DELETE_FILE") == 0) {
        log_message("NM requested DELETE_FILE for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        if (remove(file_path) == 0) {
            log_message("Successfully deleted file '%s'.", filename);
            write(nm_socket, "ACK_DELETE_SUCCESS\n", sizeof("ACK_DELETE_SUCCESS\n") - 1);
        } else {
            log_message("Failed DELETE_FILE for '%s': remove failed.", filename);
            write(nm_socket, "ACK_DELETE_FAIL\n", sizeof("ACK_DELETE_FAIL\n") - 1);
        }
    } else if (strcmp(command, "GET_METADATA") == 0) {
        log_message("NM requested GET_METADATA for '%s'", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        int words = 0, chars = 0;
        char created_ts[128], modified_ts[128];
        char response_buf[BUFFER_SIZE];
        if (get_file_metadata(file_path, &words, &chars, created_ts, modified_ts, 128) == 0) {
            snprintf(response_buf, sizeof(response_buf), 
                "METADATA_RESPONSE %d %d %s %s\n", 
                words, chars, created_ts, modified_ts);
            log_message("Sending metadata for '%s'", filename);
        } else {
            log_message("Failed GET_METADATA for '%s': get_file_metadata failed.", filename);
            snprintf(response_buf, sizeof(response_buf), "METADATA_FAIL\n");
        }
        write(nm_socket, response_buf, strlen(response_buf));

    } else if (strcmp(command, "UNDO_FILE") == 0) {
        log_message("NM requested UNDO_FILE for '%s'", filename);
        char file_path[512];
        char bak_file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        snprintf(bak_file_path, sizeof(bak_file_path), "%s%s.bak", SS_STORAGE_DIR, filename);

        struct stat st;
        if (stat(bak_file_path, &st) != 0) {
            log_message("Failed UNDO_FILE for '%s': No .bak file found.", filename);
            write(nm_socket, "ACK_UNDO_FAIL_NO_BAK\n", 21);
        } else {
            if (remove(file_path) != 0) {
                log_message("Failed UNDO_FILE for '%s': Could not remove current file.", filename);
                write(nm_socket, "ACK_UNDO_FAIL\n", 14);
            } else if (rename(bak_file_path, file_path) != 0) {
                log_message("Failed UNDO_FILE for '%s': Could not restore .bak file.", filename);
                write(nm_socket, "ACK_UNDO_FAIL\n", 14);
            } else {
                log_message("Successfully reverted '%s' from backup.", filename);
                write(nm_socket, "ACK_UNDO_SUCCESS\n", 17);
            }
        }
    } else {
        log_message("Unknown command from NM: %s", command);
        write(nm_socket, "ACK_UNKNOWN\n", sizeof("ACK_UNKNOWN\n") - 1);
    }
    close(nm_socket);
    return NULL;
}

/*
 * Main loop for the SS to listen for commands from the Name Server
 */
void* start_nm_server(void* arg) {
    // (This function IS present)
    int server_fd;
    struct sockaddr_in server_addr, nm_addr;
    socklen_t nm_len = sizeof(nm_addr);

    printf("SS: Starting NM-facing server on port %d\n", SS_NM_PORT);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("SS (NM-Server): socket failed");
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SS_NM_PORT);
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("SS (NM-Server): bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("SS (NM-Server): listen failed");
        exit(EXIT_FAILURE);
    }

    while (1) {
        int nm_socket = accept(server_fd, (struct sockaddr*)&nm_addr, &nm_len);
        if (nm_socket < 0) {
            perror("SS (NM-Server): accept failed");
            continue;
        }
        pthread_t thread_id;
        int* p_nm_socket = malloc(sizeof(int));
        *p_nm_socket = nm_socket;
        if (pthread_create(&thread_id, NULL, handle_nm_command, (void*)p_nm_socket) != 0) {
            perror("SS (NM-Server): pthread_create failed");
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
    // (This function is unchanged)
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
    printf("SS: Connected to Name Server.\n");
    int offset = sprintf(registration_msg, "REGISTER_SS %d %d", 
                         SS_NM_PORT, SS_CLIENT_PORT);
    for (int i = 0; i < num_files; i++) {
        offset += sprintf(registration_msg + offset, " %s", my_files[i]);
    }
    sprintf(registration_msg + offset, "\n");
    if (write(sock, registration_msg, strlen(registration_msg)) < 0) {
        perror("SS: failed to send registration message");
    } else {
        printf("SS: Sent registration message:\n%s", registration_msg);
    }
    close(sock);
    printf("SS: Registration complete.\n");
}

int main() {
    // (This function is unchanged)
    register_with_name_server();
    pthread_t client_server_thread_id;
    if (pthread_create(&client_server_thread_id, NULL, start_client_server, NULL) != 0) {
        perror("SS: Failed to create client server thread");
        exit(EXIT_FAILURE);
    }
    start_nm_server(NULL); 
    pthread_join(client_server_thread_id, NULL);
    return 0;
}