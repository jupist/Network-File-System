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


// --- Define This Storage Server's Details ---
#define SS_NM_PORT 9001       // Port for NM to connect to
#define SS_CLIENT_PORT 9002   // Port for Clients to connect to
#define SS_STORAGE_DIR "ss_storage/" // All files are in here

const char* my_files[] = {
    "wowee.txt",
    "nuh_uh.txt"
};
int num_files = 2;


// --- **** UPDATED METADATA HELPER **** ---
/*
 * Reads a file's stats and calculates word/char counts.
 * Fills the provided pointers with REAL timestamps.
 * Returns 0 on success, -1 on failure.
 */
int get_file_metadata(const char* file_path, int* word_count, int* char_count, 
                      char* created_ts, char* modified_ts, int ts_len) 
{
    struct stat file_stat;

    if (stat(file_path, &file_stat) != 0) {
        perror("SS: stat failed");
        return -1;
    }

    *char_count = (int)file_stat.st_size; // Get byte count from stat

    // Now, open the file just to count words
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

    // Format timestamps
    struct tm *tm_info;

    // Created time (st_ctime)
    tm_info = localtime(&file_stat.st_ctime);
    strftime(created_ts, ts_len, "%Y-%m-%d %H:%M", tm_info);

    // Modified time (st_mtime)
    tm_info = localtime(&file_stat.st_mtime);
    strftime(modified_ts, ts_len, "%Y-%m-%d %H:%M", tm_info);

    return 0;
}


/*
 * Thread function to handle a direct connection from a Client
 */
void* handle_client_request(void* arg) {
    // (This function is unchanged)
    int client_socket = *((int*)arg);
    free(arg);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        printf("SS (Client-Handler): Client disconnected before request.\n");
        close(client_socket);
        return NULL;
    }
    buffer[bytes_read] = '\0';

    char command[64], filename[256];
    if (sscanf(buffer, "%s %s", command, filename) != 2) {
        printf("SS (Client-Handler): Invalid command format.\n");
        close(client_socket);
        return NULL;
    }

    if (strcmp(command, "READ_FILE") == 0) {
        printf("SS (Client-Handler): Client requested to read '%s'\n", filename);

        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);

        FILE* file = fopen(file_path, "r");
        if (file == NULL) {
            perror("SS (Client-Handler): fopen failed");
            const char* err_msg = "ERROR: File not found or permission denied.\n";
            write(client_socket, err_msg, strlen(err_msg));
            close(client_socket);
            return NULL;
        }

        char file_buffer[BUFFER_SIZE];
        size_t bytes_read_from_file;
        while ((bytes_read_from_file = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
            if (write(client_socket, file_buffer, bytes_read_from_file) < 0) {
                perror("SS (Client-Handler): write to client failed");
                break; 
            }
        }
        fclose(file);
        printf("SS (Client-Handler): File '%s' sent successfully.\n", filename);

    } else if (strcmp(command, "STREAM_FILE") == 0) {
        printf("SS (Client-Handler): Client requested to stream '%s'\n", filename);

        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);

        FILE* file = fopen(file_path, "r");
        if (file == NULL) {
            perror("SS (Client-Handler): fopen failed");
            const char* err_msg = "ERROR: File not found or permission denied.\n";
            write(client_socket, err_msg, strlen(err_msg));
            close(client_socket);
            return NULL;
        }

        char word_buffer[256]; 
        while (fscanf(file, "%255s", word_buffer) == 1) {
            if (write(client_socket, word_buffer, strlen(word_buffer)) < 0) {
                perror("SS (Client-Stream): write word failed");
                break;
            }
            if (write(client_socket, " ", 1) < 0) {
                perror("SS (Client-Stream): write space failed");
                break;
            }
            usleep(100000);
        }
        write(client_socket, "\n", 1);
        fclose(file);
        printf("SS (Client-Handler): File '%s' streamed successfully.\n", filename);

    } else {
        printf("SS (Client-Handler): Unknown command '%s'\n", command);
    }
    close(client_socket);
    return NULL;
}

/*
 * Main loop for the SS to listen for direct Client connections
 */
void* start_client_server(void* arg) {
    // (This function is unchanged)
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
void* handle_nm_command(void* arg) {
    int nm_socket = *((int*)arg);
    free(arg);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    bytes_read = read(nm_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        printf("SS (NM-Handler): NM disconnected before request.\n");
        close(nm_socket);
        return NULL;
    }
    buffer[bytes_read] = '\0';

    char command[64], filename[256];
    if (sscanf(buffer, "%s %s", command, filename) != 2) {
        printf("SS (NM-Handler): Invalid command format from NM.\n");
        close(nm_socket);
        return NULL;
    }

    if (strcmp(command, "CREATE_FILE") == 0) {
        // (This section is unchanged)
        printf("SS (NM-Handler): NM requested to create '%s'\n", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        FILE* file = fopen(file_path, "w");
        if (file == NULL) {
            perror("SS (NM-Handler): fopen failed");
            write(nm_socket, "ACK_CREATE_FAIL\n", sizeof("ACK_CREATE_FAIL\n") - 1);
        } else {
            fclose(file);
            printf("SS (NM-Handler): File '%s' created successfully.\n", filename);
            write(nm_socket, "ACK_CREATE_SUCCESS\n", sizeof("ACK_CREATE_SUCCESS\n") - 1);
        }
    
    } else if (strcmp(command, "DELETE_FILE") == 0) {
        // (This section is unchanged)
        printf("SS (NM-Handler): NM requested to delete '%s'\n", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);
        if (remove(file_path) == 0) {
            printf("SS (NM-Handler): File '%s' deleted successfully.\n", filename);
            write(nm_socket, "ACK_DELETE_SUCCESS\n", sizeof("ACK_DELETE_SUCCESS\n") - 1);
        } else {
            perror("SS (NM-Handler): remove failed");
            write(nm_socket, "ACK_DELETE_FAIL\n", sizeof("ACK_DELETE_FAIL\n") - 1);
        }
    
    } else if (strcmp(command, "GET_METADATA") == 0) {
        // --- **** UPDATED METADATA LOGIC **** ---
        printf("SS (NM-Handler): NM requested metadata for '%s'\n", filename);
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);

        int words = 0, chars = 0;
        char created_ts[128], modified_ts[128]; // No 'accessed' timestamp
        char response_buf[BUFFER_SIZE];

        if (get_file_metadata(file_path, &words, &chars, created_ts, modified_ts, 128) == 0) {
            // Send real timestamps AND word count (no accessed time)
            snprintf(response_buf, sizeof(response_buf), 
                "METADATA_RESPONSE %d %d %s %s\n", 
                words, chars,
                created_ts,  // e.g., "2025-11-05 16:30"
                modified_ts
            );
            printf("  Sending metadata: %s", response_buf);
        } else {
            perror("SS (NM-Handler): get_file_metadata failed");
            snprintf(response_buf, sizeof(response_buf), "METADATA_FAIL\n");
        }
        write(nm_socket, response_buf, strlen(response_buf));
        // --- **** END OF UPDATED METADATA LOGIC **** ---

    } else {
        printf("SS (NM-Handler): Unknown command from NM '%s'\n", command);
        write(nm_socket, "ACK_UNKNOWN\n", sizeof("ACK_UNKNOWN\n") - 1);
    }

    close(nm_socket);
    return NULL;
}

/*
 * Main loop for the SS to listen for commands from the Name Server
 */
void* start_nm_server(void* arg) {
    // (This function is unchanged)
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