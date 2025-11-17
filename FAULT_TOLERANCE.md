# Fault Tolerance Implementation

## Overview
Comprehensive fault tolerance has been implemented in the distributed file system with replication, failure detection, and automatic recovery mechanisms.

## 1. Replication Strategy

### File Replication
- **Replication Pairs**: Storage Servers (SS) are organized in pairs where odd-numbered SS replicates even-numbered SS
  - SS0 (primary) ← replicated by → SS1 (replica)
  - SS2 (primary) ← replicated by → SS3 (replica)
  - etc.

- **Asynchronous Replication**: All write operations are replicated asynchronously to backup SS
  - CREATE: New files are created on both primary and replica SS
  - DELETE: File deletions are propagated to replica
  - WRITE: File modifications are tracked and can be replicated
  - MOVE: Folder operations are replicated

### Key Functions
- `replicate_to_backup_ss()`: Sends commands asynchronously to replica SS without waiting for acknowledgment
- `copy_file_between_ss()`: Copies file content from one SS to another during resync
- `get_file_content_from_ss()`: Retrieves file content for replication

## 2. Failure Detection

### Heartbeat Monitoring
- **Heartbeat Thread**: Dedicated thread `monitor_heartbeats()` runs continuously
- **Heartbeat Interval**: Storage Servers send heartbeat every 10 seconds
- **Timeout**: SS marked offline if no heartbeat received for 30 seconds
- **Automatic Detection**: Name Server automatically detects SS failures

### Configuration
```c
#define HEARTBEAT_TIMEOUT 30   // Seconds before SS considered failed
#define HEARTBEAT_INTERVAL 10  // Seconds between heartbeats from SS
```

### SS State Tracking
```c
typedef struct {
    int ss_nm_port;          
    int ss_client_port;      
    char ss_ip_addr[INET_ADDRSTRLEN];
    int is_online;              // 1 if online, 0 if failed
    time_t last_heartbeat;      // Timestamp of last heartbeat
    int replica_of;             // Index of SS this replicates (-1 if primary)
    int replicated_by;          // Index of SS that replicates this (-1 if none)
} StorageServer;
```

## 3. Failover Mechanism

### Automatic Failover
When a primary SS is offline, the Name Server automatically redirects requests to the replica SS:

- **READ Operations**: If primary SS is offline, replica SS location is sent to client
- **STREAM Operations**: Same failover logic as READ
- **WRITE Operations**: Lock requests are redirected to replica if primary is down

### Failover Logic
```c
// Check if primary SS is online, if not use replica
int primary_ss_index = file->ss_index;
if (primary_ss_index >= 0 && !server_list[primary_ss_index].is_online) {
    int replica_index = server_list[primary_ss_index].replicated_by;
    if (replica_index >= 0 && server_list[replica_index].is_online) {
        // Use replica SS
        strncpy(ss_ip, server_list[replica_index].ss_ip_addr, INET_ADDRSTRLEN);
        ss_port = server_list[replica_index].ss_client_port;
        printf("Primary SS offline, using replica SS%d\n", replica_index);
    }
}
```

## 4. SS Recovery and Resync

### Reconnection Detection
- When an SS reconnects, it's identified by matching `ss_nm_port` and `ss_ip_addr`
- Status is updated: `is_online = 1` and `last_heartbeat` is refreshed

### Automatic Resync
A dedicated thread `resync_from_replica_thread()` is spawned when SS reconnects:

1. **Identify Files to Sync**: Scans file registry for all files belonging to the reconnected SS
2. **Copy from Replica**: Reads each file from replica SS and writes to primary SS
3. **SYNC_FILE Command**: Uses special `SYNC_FILE` command to overwrite files with latest content
4. **Progress Tracking**: Logs progress and completion status

### Resync Process
```
[Resync Thread] Starting for SS0
[Resync Thread] Copying files from SS1 (replica) to SS0 (primary)
[Resync Thread] Found 15 files to resync
[Resync Thread] 1/15: file1.txt
[Resync Thread] 2/15: file2.txt
...
[Resync Thread] Complete: 15/15 files synchronized to SS0
```

## 5. File Tracking

### SS Index Tracking
Each file tracks its primary SS owner:
```c
typedef struct {
    char filename[256];
    // ... other fields ...
    int ss_index;  // Index of primary SS that owns this file
} FileLocation;
```

This enables:
- Quick identification of which SS owns a file
- Efficient resync by finding all files for a specific SS
- Proper failover routing

## 6. Storage Server Commands

### New Commands for Replication
- **SYNC_FILE**: Overwrites file with content sent from Name Server
  ```
  SYNC_FILE <filename>
  <file content here>
  ```
  Used during resync to restore files to primary SS

### Existing Commands Enhanced
- **CREATE_FILE**: Creates empty file, used by both primary and replica
- **DELETE_FILE**: Deletes file, replicated to backup
- **HEARTBEAT**: Periodic liveness signal from SS

## 7. Thread Safety

All replication and failover operations are thread-safe:
- `g_system_mutex` protects access to `server_list`, file registry, and locks
- Mutex is released before network operations to prevent blocking
- Resync runs in detached threads to avoid blocking main server

## 8. Testing Fault Tolerance

### Test Scenario 1: SS Failure
1. Start two Storage Servers (SS0 on port 9001, SS1 on port 9002)
2. Create files - they'll be distributed and replicated
3. Kill SS0 (primary)
4. Try to READ a file that was on SS0
5. **Expected**: File is retrieved from SS1 (replica) automatically

### Test Scenario 2: SS Recovery
1. Start SS0 and SS1
2. Create files on SS0
3. Kill SS0
4. Create more files (will go to SS1 or other servers)
5. Restart SS0
6. **Expected**: Resync thread starts, copies files from SS1 to SS0

### Test Commands
```bash
# Terminal 1: Start Name Server
./nameserver

# Terminal 2: Start SS0
./storageserver 9001 9091 ss_storage/

# Terminal 3: Start SS1  
./storageserver 9002 9092 ss_storage_replica/

# Terminal 4: Client operations
./client
REGISTER_CLIENT alice
CREATE test.txt
# Kill SS0 in Terminal 2
READ test.txt  # Should still work via SS1
```

## 9. Implementation Files Modified

### nm_modules/nm_types.h
- Added `ss_index` field to `FileLocation` struct
- Existing: `is_online`, `last_heartbeat`, `replica_of`, `replicated_by` in `StorageServer`

### nameserver.c
- Added `get_file_content_from_ss()`: Retrieve file content for resync
- Added `copy_file_between_ss()`: Copy files during resync
- Added `resync_from_replica_thread()`: Background thread for resync
- Enhanced `replicate_to_backup_ss()`: Async replication
- Updated SS reconnection logic to trigger resync
- Added failover logic in READ/STREAM/WRITE handlers
- Updated CREATE to set `ss_index`

### storageserver.c
- Added `SYNC_FILE` command handler
- Existing heartbeat mechanism maintained

## 10. Limitations and Future Enhancements

### Current Limitations
1. Only one replica per primary SS
2. Write replication happens at command level, not continuous sync
3. No circular replication or quorum-based writes

### Future Enhancements
1. **Multiple Replicas**: Support N-way replication
2. **Periodic Sync**: Background thread to sync file changes continuously
3. **Checksum Verification**: Verify file integrity during resync
4. **Priority-based Recovery**: Prioritize frequently accessed files during resync
5. **Replica Chain**: Support replica of replica for better redundancy

## Summary

The implementation provides:
✅ **Automatic Replication** - Files automatically copied to replica SS
✅ **Failure Detection** - Heartbeat-based monitoring detects failures in 30 seconds
✅ **Seamless Failover** - Clients transparently redirected to replica on failure
✅ **Automatic Recovery** - Reconnected SS automatically resynced from replica
✅ **Thread-Safe** - All operations protected with proper locking
✅ **Asynchronous** - Replication doesn't block client operations
