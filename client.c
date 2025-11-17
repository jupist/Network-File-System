#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "common.h"

/*
 * This function connects to the Storage Server, requests the file,
 * and prints the content to stdout.
 */
void fetch_file_from_ss(const char* ss_ip, int ss_port, const char* filename) {
    int ss_sock;
    struct sockaddr_in ss_addr;

    printf("--- Connecting to Storage Server at %s:%d...\n", ss_ip, ss_port);

    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Client (SS): socket creation failed");
        return;
    }

    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        perror("Client (SS): invalid address");
        close(ss_sock);
        return;
    }

    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Client (SS): connection to Storage Server failed");
        close(ss_sock);
        return;
    }

    char request_msg[BUFFER_SIZE];
    snprintf(request_msg, sizeof(request_msg), "READ_FILE %s\n", filename);
    if (write(ss_sock, request_msg, strlen(request_msg)) < 0) {
        perror("Client (SS): failed to send file request");
        close(ss_sock);
        return;
    }

    char file_buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    printf("--- File Content ---\n");
    while ((bytes_read = read(ss_sock, file_buffer, sizeof(file_buffer) - 1)) > 0) {
        file_buffer[bytes_read] = '\0';
        printf("%s", file_buffer); 
    }
    printf("\n--- End of File ---\n");

    if (bytes_read < 0) {
        perror("Client (SS): read from SS failed");
    }

    close(ss_sock);
}

/*
 * This function connects to the Storage Server and prints the
 * data as it arrives, simulating a stream.
 */
void stream_file_from_ss(const char* ss_ip, int ss_port, const char* filename) {
    int ss_sock;
    struct sockaddr_in ss_addr;

    printf("--- Connecting to Storage Server at %s:%d for streaming...\n", ss_ip, ss_port);

    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Client (SS-Stream): socket creation failed");
        return;
    }

    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        perror("Client (SS-Stream): invalid address");
        close(ss_sock);
        return;
    }

    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Client (SS-Stream): connection to Storage Server failed");
        close(ss_sock);
        return;
    }

    char request_msg[BUFFER_SIZE];
    snprintf(request_msg, sizeof(request_msg), "STREAM_FILE %s\n", filename);
    if (write(ss_sock, request_msg, strlen(request_msg)) < 0) {
        perror("Client (SS-Stream): failed to send file request");
        close(ss_sock);
        return;
    }

    char stream_buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    printf("--- Start of Stream ---\n");
    while ((bytes_read = read(ss_sock, stream_buffer, sizeof(stream_buffer) - 1)) > 0) {
        stream_buffer[bytes_read] = '\0';
        printf("%s", stream_buffer); 
        fflush(stdout); 
    }
    printf("--- End of Stream ---\n");

    if (bytes_read < 0) {
        perror("Client (SS-Stream): read from SS failed");
    }

    close(ss_sock);
}

/*
 * Connects to SS and enters the interactive write session.
 */
void write_session_to_ss(int nm_socket, const char* ss_ip, int ss_port, const char* base_filename, const char* ss_filename, int sentence_num) {
    int ss_sock;
    struct sockaddr_in ss_addr;
    char command_buffer[BUFFER_SIZE];
    char response_buffer[BUFFER_SIZE];

    printf("--- Connecting to Storage Server at %s:%d for writing...\n", ss_ip, ss_port);

    // 1. Connect to SS
    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Client (SS-Write): socket creation failed");
        return;
    }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        perror("Client (SS-Write): invalid address");
        close(ss_sock);
        return;
    }
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Client (SS-Write): connection to Storage Server failed");
        close(ss_sock);
        return;
    }

    // 2. Send the WRITE_START command
    snprintf(command_buffer, sizeof(command_buffer), "WRITE_START %s %d\n", ss_filename, sentence_num);
    if (write(ss_sock, command_buffer, strlen(command_buffer)) < 0) {
        perror("Client (SS-Write): failed to send start command");
        close(ss_sock);
        return;
    }

    // --- **** FIX: Read the initial ACK/ERROR from SS **** ---
    ssize_t bytes_read = read(ss_sock, response_buffer, sizeof(response_buffer) - 1);
    if (bytes_read <= 0) {
        printf("Storage Server disconnected or failed to send initial ACK.\n");
        close(ss_sock);
        // We still hold the lock. We must tell the NM to release it.
        // The original code did this *after* the loop, so we'll do it there.
        return; // Exit the function, the 'finally' block below will run.
    }
    response_buffer[bytes_read] = '\0';
    printf("SS: %s", response_buffer);

    // If the server sent an error on START (e.g., bad sentence index), abort.
    if (strncmp(response_buffer, "ERROR:", 6) == 0) {
        // We don't enter the loop. We'll just fall through to the lock release.
    } else {
        // 3. Enter interactive write loop
        printf("Write session started. Enter '<word_index> <content>' or 'ETIRW' to finish.\n");
        while (1) {
            printf("Write > ");
            if (fgets(command_buffer, sizeof(command_buffer), stdin) == NULL) {
                printf("Error reading input. Sending ETIRW to finalize.\n");
                strncpy(command_buffer, "ETIRW\n", sizeof(command_buffer));
            }

            // Send the command (e.g., "3 new_word" or "ETIRW")
            if (write(ss_sock, command_buffer, strlen(command_buffer)) < 0) {
                perror("Failed to send command to SS. Aborting");
                break;
            }

            // --- **** FIX: Wait for a response from SS for *every* command **** ---
            bytes_read = read(ss_sock, response_buffer, sizeof(response_buffer) - 1);
            if (bytes_read <= 0) {
                printf("Storage Server disconnected unexpectedly.\n");
                break; // Exit loop, connection is dead
            }
            response_buffer[bytes_read] = '\0';
            printf("SS: %s", response_buffer); // Print ACK_UPDATE_OK or ERROR: ...
            // --- **** END OF FIX **** ---

            // If we just sent ETIRW, the response was the final ACK. Exit.
            if (strncmp(command_buffer, "ETIRW", 5) == 0) {
                break; // Exit loop
            }
        }
    }
    
    close(ss_sock);

    // 4. Tell NM to release the lock
    printf("Write session ended. Releasing lock...\n");
    snprintf(command_buffer, sizeof(command_buffer), "RELEASE_LOCK %s %d\n", base_filename, sentence_num);
    if (write(nm_socket, command_buffer, strlen(command_buffer)) < 0) {
        perror("Failed to send RELEASE_LOCK to NM");
    } else {
        // Read the final ACK from NM
        bytes_read = read(nm_socket, response_buffer, sizeof(response_buffer) - 1);
        if (bytes_read > 0) {
            response_buffer[bytes_read] = '\0';
            printf("NM: %s", response_buffer);
        }
    }
}


/*
 * Connects to the Name Server and returns the socket descriptor.
 */
int connect_to_name_server() {
    int sock;
    struct sockaddr_in nm_addr;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Client: socket creation failed");
        return -1;
    }

    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NAME_SERVER_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr) <= 0) {
        perror("Client: invalid address");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        perror("Client: connection to Name Server failed");
        close(sock);
        return -1;
    }

    printf("Connected to Name Server!\n");
    return sock;
}

/*
 * Main loop to handle user commands.
 */
void command_loop(int nm_socket) {
    char command_buffer[BUFFER_SIZE];
    char response_buffer[BUFFER_SIZE];

    while (1) {
        printf("Docs++ > ");
        
        if (fgets(command_buffer, sizeof(command_buffer), stdin) == NULL) {
            printf("Error reading input or EOF reached. Exiting.\n");
            break;
        }

        // --- **** CORRECTED LOGIC ORDER **** ---
        char command[64], arg1[256], arg2_str[256];
        int arg2_int;
        int is_read_cmd = 0;
        int is_stream_cmd = 0; 
        int is_write_cmd = 0;
        
        // --- CHECK FOR 3-ARG COMMANDS FIRST (with integer arg) ---
        if (sscanf(command_buffer, "%s %s %d", command, arg1, &arg2_int) == 3) {
             if (strcmp(command, "WRITE") == 0) {
                is_write_cmd = 1;
            }
        // --- THEN CHECK FOR 2-ARG COMMANDS ---
        } else if (sscanf(command_buffer, "%s %s %s", command, arg1, arg2_str) == 3) {
            // Commands with 2 string arguments (like MOVE filename foldername)
            // No special handling needed - will use normal response flow
        } else if (sscanf(command_buffer, "%s %s", command, arg1) == 2) {
            if (strcmp(command, "READ") == 0) {
                is_read_cmd = 1;
            } else if (strcmp(command, "STREAM") == 0) {
                is_stream_cmd = 1;
            }
        }
        // --- **** END OF FIX **** ---

        // 3. Send the command to the Name Server
        if (write(nm_socket, command_buffer, strlen(command_buffer)) < 0) {
            perror("Failed to send command to NM");
            break;
        }
        
        // Handle interactive WRITE
        if (is_write_cmd) {
            ssize_t bytes_read = read(nm_socket, response_buffer, sizeof(response_buffer) - 1);
            if (bytes_read <= 0) {
                printf("Name Server closed the connection. Exiting.\n");
                break;
            }
            response_buffer[bytes_read] = '\0';
            
            if (strncmp(response_buffer, "SS_LOCATION", 11) == 0) {
                char ss_ip[INET_ADDRSTRLEN];
                int ss_port;
                char full_path[512];
                
                // Try to parse with 3 parameters (ip, port, full_path)
                if (sscanf(response_buffer, "SS_LOCATION %s %d %s", ss_ip, &ss_port, full_path) == 3) {
                    write_session_to_ss(nm_socket, ss_ip, ss_port, arg1, full_path, arg2_int);
                } else if (sscanf(response_buffer, "SS_LOCATION %s %d", ss_ip, &ss_port) == 2) {
                    // Fallback to 2 parameters (old format)
                    write_session_to_ss(nm_socket, ss_ip, ss_port, arg1, arg1, arg2_int);
                } else {
                    printf("Error: Could not parse SS_LOCATION response.\n");
                }
            } else {
                printf("%s", response_buffer);
            }
            continue; 
        }
        
        // 4. Read response for all other commands
        ssize_t bytes_read = read(nm_socket, response_buffer, sizeof(response_buffer) - 1);
        if (bytes_read <= 0) {
            printf("Name Server closed the connection. Exiting.\n");
            break;
        }
        response_buffer[bytes_read] = '\0'; 

        // 5. Check response and act
        if ((is_read_cmd || is_stream_cmd) && strncmp(response_buffer, "SS_LOCATION", 11) == 0) {
            char ss_ip[INET_ADDRSTRLEN];
            int ss_port;
            char full_path[512];
            
            // Try to parse with 3 parameters (ip, port, full_path)
            if (sscanf(response_buffer, "SS_LOCATION %s %d %s", ss_ip, &ss_port, full_path) == 3) {
                if (is_read_cmd) {
                    fetch_file_from_ss(ss_ip, ss_port, full_path); 
                } else if (is_stream_cmd) {
                    stream_file_from_ss(ss_ip, ss_port, full_path); 
                }
            } else if (sscanf(response_buffer, "SS_LOCATION %s %d", ss_ip, &ss_port) == 2) {
                // Fallback to 2 parameters (old format)
                if (is_read_cmd) {
                    fetch_file_from_ss(ss_ip, ss_port, arg1); 
                } else if (is_stream_cmd) {
                    stream_file_from_ss(ss_ip, ss_port, arg1); 
                }
            } else {
                printf("Error: Could not parse SS_LOCATION response.\n");
            }
        } else {
            // This is a normal response (VIEW, ERROR, etc.)
            printf("%s", response_buffer);
        }
    }
}

int main() {
    char username[256];

    printf("Enter your username: ");
    if (fgets(username, sizeof(username), stdin) == NULL) {
        perror("Failed to read username");
        exit(EXIT_FAILURE);
    }
    username[strcspn(username, "\n")] = 0; // Remove trailing newline

    int nm_socket = connect_to_name_server();
    if (nm_socket < 0) {
        exit(EXIT_FAILURE);
    }

    char reg_msg[BUFFER_SIZE];
    snprintf(reg_msg, sizeof(reg_msg), "REGISTER_CLIENT %s\n", username);
    
    if (write(nm_socket, reg_msg, strlen(reg_msg)) < 0) {
        perror("Failed to send client registration");
        close(nm_socket);
        exit(EXIT_FAILURE);
    }

    // Wait for registration response from nameserver
    char response[BUFFER_SIZE];
    ssize_t bytes_read = read(nm_socket, response, sizeof(response) - 1);
    if (bytes_read > 0) {
        response[bytes_read] = '\0';
        // Check if it's an error message
        if (strncmp(response, "ERROR", 5) == 0) {
            printf("%s", response);
            close(nm_socket);
            exit(EXIT_FAILURE);
        }
        // If it's not an error, registration was successful, continue
    } else if (bytes_read == 0) {
        printf("ERROR: Nameserver closed connection during registration.\n");
        close(nm_socket);
        exit(EXIT_FAILURE);
    } else {
        perror("Failed to receive registration response");
        close(nm_socket);
        exit(EXIT_FAILURE);
    }

    command_loop(nm_socket);

    close(nm_socket);
    return 0;
}