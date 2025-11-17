#ifndef SS_TYPES_H
#define SS_TYPES_H

#include "common.h"

/*
 * Data structures for Storage Server
 */

// Word/Sentence linked list structure
typedef struct WordNode {
    char* word;
    struct WordNode* next_word;
    struct WordNode* next_sentence;
} WordNode;

// Storage Server Configuration (declared in storageserver.c)
extern int SS_NM_PORT;         // Port for NM to connect to
extern int SS_CLIENT_PORT;     // Port for Clients to connect to
extern char SS_STORAGE_DIR[];  // Storage directory
#define SS_LOG_FILE "storageserver.log"

#endif // SS_TYPES_H
