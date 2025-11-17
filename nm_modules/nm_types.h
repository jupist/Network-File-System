#ifndef NM_TYPES_H
#define NM_TYPES_H

#include <arpa/inet.h>
#include <time.h>
#include "../common.h"

/*
 * Data structures for Name Server
 */

typedef struct {
    char username[256];
    char permission; // 'R' for Read, 'W' for Read/Write
} AccessControl;

#define MAX_PERMISSIONS 10 

typedef struct {
    char filename[256];
    char folder_path[512];       // Full folder path (e.g., "/folder1/subfolder2")
    char owner_username[256];        
    AccessControl acl[MAX_PERMISSIONS]; 
    int num_permissions;                

    char last_accessed_by[256]; 
    char last_accessed_ts[128]; 

    int ss_client_port;      
    char ss_ip_addr[INET_ADDRSTRLEN];
    int ss_index;                // Index of primary SS that owns this file
} FileLocation;

typedef struct {
    int ss_nm_port;          
    int ss_client_port;      
    char ss_ip_addr[INET_ADDRSTRLEN];
    int is_online;              // 1 if online, 0 if failed
    time_t last_heartbeat;      // Timestamp of last heartbeat
    int replica_of;             // Index of SS this replicates (-1 if primary)
    int replicated_by;          // Index of SS that replicates this (-1 if none)
} StorageServer;

typedef struct {
    char username[256];
    char ip_addr[INET_ADDRSTRLEN];
    int client_socket_fd; 
} ClientInfo;

typedef struct {
    char filename[256];
    int sentence_num;
    char username[256]; // Who holds the lock
} FileLock;

// Folder Structure
typedef struct FolderNode {
    char foldername[256];
    char folder_path[512];       // Full path (e.g., "/folder1/subfolder2")
    char owner_username[256];
    struct FolderNode* next;
} FolderNode;

// Access Request Structure
typedef struct {
    char filename[256];
    char requester_username[256];
    char owner_username[256];
    char requested_permission; // 'R' for Read, 'W' for Write
    char timestamp[128];
    int status; // 0 = pending, 1 = approved, 2 = rejected
} AccessRequest;

// Hash Map Structures
typedef struct HashNode {
    char key[256];             // The filename
    FileLocation file;         // The FileLocation struct
    struct HashNode* next;     // Pointer to the next node in the chain
} HashNode;

// LRU Cache Structures
typedef struct CacheNode {
    char filename[256];
    FileLocation* file_ptr;     // Pointer to the FileLocation in the main hash map
    struct CacheNode* prev;
    struct CacheNode* next;
} CacheNode;

typedef struct CacheMapEntry {
    char filename[256];
    CacheNode* node_ptr;        // Pointer to the node in the linked list
    struct CacheMapEntry* next;
} CacheMapEntry;

// Global Defines
#define HASH_MAP_SIZE 100
#define CACHE_MAP_SIZE 20
#define MAX_CACHE_SIZE 10
#define MAX_SERVERS 10
#define MAX_CLIENTS 50 
#define MAX_LOCKS 50
#define MAX_FOLDERS 200
#define MAX_ACCESS_REQUESTS 100
#define HEARTBEAT_TIMEOUT 15  // Seconds before SS considered failed (reduced from 30)
#define HEARTBEAT_INTERVAL 5  // Seconds between heartbeats from SS (reduced from 10)
#define EXEC_OUTPUT_BUFFER_SIZE 8192 
#define NM_REGISTRY_FILE "nm_registry.dat"
#define NM_LOG_FILE "nameserver.log"

#endif // NM_TYPES_H
