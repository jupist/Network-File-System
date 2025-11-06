#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// --- **** ADD THESE ERROR CODES **** ---
#define ERROR_FILE_NOT_FOUND 404
#define ERROR_ACCESS_DENIED 403
#define ERROR_FILE_LOCKED 423
#define ERROR_FILE_EXISTS 409
#define ERROR_INVALID_INDEX 400
#define ERROR_NO_UNDO_HISTORY 410
#define ERROR_SERVER_ERROR 500
#define ERROR_MAX_LOCKS 503
// --- **** END OF ADDITION **** ---

#define NAME_SERVER_PORT 8080
#define BUFFER_SIZE 4096

#endif //COMMON_H