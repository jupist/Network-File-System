[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/0ek2UV58)

# Docs++ : Distributed File System

A distributed file system implementation with multiple storage servers, client-server architecture, and comprehensive file management features.

## Table of Contents
- [System Architecture](#system-architecture)
- [Getting Started](#getting-started)
- [Command Reference](#command-reference)
  - [File Operations](#file-operations)
  - [Directory Operations](#directory-operations)
  - [Permission Management](#permission-management)
  - [Version Control](#version-control)
  - [Access Request System](#access-request-system)
  - [System Information](#system-information)

---

## System Architecture

The system consists of three main components:

1. **Name Server** - Central directory service that manages file metadata, permissions, and routing
2. **Storage Servers** - Handle actual file storage and operations (supports multiple instances)
3. **Client** - User interface for interacting with the file system

### File Replication Strategy

**Automatic Dual-Server Storage**: When files are created, the system automatically stores them on **two different storage servers** for redundancy and fault tolerance:

- **2+ Storage Servers Available**: Files are replicated across two servers using round-robin selection
- **1 Storage Server Available**: Files are stored on the single available server (no replication)
- **Load Balancing**: Distribution algorithm ensures even load across all available servers
- **Transparency**: Users don't see replication details - all operations appear seamless

### Indexing Convention

**Important**: All indexing in this system is **0-based**:
- **Sentence Numbers**: Start from 0 (first sentence is sentence 0)
- **Word Positions**: Start from 0 (first word in a sentence is position 0)
- **File Operations**: All commands use 0-based indexing for consistency

---

## Getting Started

### Starting the System

1. **Start the Name Server:**
   ```bash
   ./nameserver
   ```

2. **Start Storage Server(s):**
   ```bash
   # Single storage server
   ./storageserver 9001 9002 ss_storage
   
   # Multiple storage servers (for replication)
   ./storageserver 9001 9002 ss_storage
   ./storageserver 9003 9004 ss_storage1
   ```

3. **Start the Client:**
   ```bash
   ./client
   ```
   You will be prompted to enter your username.

### Client Interface

The client provides a user-friendly command prompt interface:

**Prompt Format:**
```
Docs++ <username> > 
```
**Example:**
```
Docs++ Alice > CREATE myfile.txt
Docs++ Bob > READ document.txt
```

**Error Handling:**
- Clear error messages with HTTP-style status codes (400, 404, 403, etc.)
- Automatic validation of command syntax and parameters
- Intelligent error recovery (e.g., won't start write sessions on errors)

---

## Command Reference

### File Operations

#### **CREATE**
Create a new file in the system.

**Syntax:**
```
CREATE <filename>
```

**Example:**
```
CREATE myfile.txt
CREATE documents/report.txt
```

**Replication Behavior:**
- **2+ Storage Servers**: File is automatically replicated to two different servers
- **1 Storage Server**: File is stored on the single available server
- **Server Selection**: Uses intelligent round-robin algorithm for load balancing

**Notes:**
- Files are automatically assigned using round-robin distribution across available servers
- The owner is automatically set to the user creating the file
- Creates an empty file initially
- All replication is transparent to the user

---

#### **DELETE**
Delete a file from the system.

**Syntax:**
```
DELETE <filename>
```

**Example:**
```
DELETE myfile.txt
DELETE docs/report.txt
```

**Replication Handling:**
- Automatically removes file from **both storage servers** if replicated
- If secondary server deletion fails, operation still succeeds (primary deletion successful)
- Metadata is completely removed from the name server

**Notes:**
- Only the file owner can delete a file
- Permanently removes the file from all storage locations and metadata

---

#### **READ**
Read and display the contents of a file.

**Syntax:**
```
READ <filename>
```

**Example:**
```
READ myfile.txt
READ 2/data.txt
```

**Notes:**
- Requires READ permission (owner has automatic access)
- Displays the complete file content
- Updates "Last Accessed" information

---

#### **STREAM**
Stream file contents word by word (alternative to READ).

**Syntax:**
```
STREAM <filename>
```

**Example:**
```
STREAM myfile.txt
```

**Notes:**
- Similar to READ but with streaming output
- Requires READ permission

---

#### **WRITE**
Write or modify a specific sentence in a file.

**Syntax:**
```
WRITE <filename> <sentence_number>
```

**Example:**
```
WRITE myfile.txt 0      # Edit first sentence
WRITE report.txt 2      # Edit third sentence
```

**Flow:**
1. System validates sentence index (must be sequential)
2. System grants you a lock on the specified sentence
3. Client prompts you to enter words at specific positions
4. After writing, the lock is automatically released

**Sequential Writing Rule:**
- You can only write to sentence N if sentence N-1 is properly terminated with `.`, `?`, or `!`
- Example: Cannot write sentence 2 until sentence 1 ends with proper punctuation
- Error: `ERROR 400: Sentence index out of bounds.` if rule is violated

**Notes:**
- Requires WRITE permission
- **Sentence and word indexes are 0-indexed** (first sentence is 0, first word is 0)
- System prevents concurrent edits to the same sentence
- Updates "Last Accessed" information
- If multiple storage servers exist, changes are replicated automatically

---

#### **UNDO**
Revert a file to its previous version (before last write).

**Syntax:**
```
UNDO <filename>
```

**Example:**
```
UNDO myfile.txt
```

**Notes:**
- Requires WRITE permission
- Restores from the `.bak` backup file
- Only stores one previous version

---

#### **EXEC**
Execute a file

**Syntax:**
```
EXEC <filename>
```

**Example:**
```
EXEC script.txt
```

**Notes:**
- Requires READ permission
- File is executed on the name server

---

### Directory Operations

#### **CREATEFOLDER**
Create a new folder.

**Syntax:**
```
CREATEFOLDER <foldername>
```

**Example:**
```
CREATEFOLDER docs
CREATEFOLDER projects
```

**Notes:**
- Folders are persistent across server restarts
- Creates the directory on the storage server

---

#### **VIEWFOLDER**
List the contents of a specific folder.

**Syntax:**
```
VIEWFOLDER <foldername>
```

**Example:**
```
VIEWFOLDER docs
VIEWFOLDER 2
```

**Output:**
```
Files in folder:
file1.txt
file2.txt
```

**Notes:**
- Shows all files in the folder
- Displays "(empty)" if folder has no contents

---

#### **MOVE**
Move a file into a folder.

**Syntax:**
```
MOVE <filename> <foldername>
```

**Example:**
```
MOVE myfile.txt docs
MOVE report.txt projects
```

**Notes:**
- Requires ownership of the file
- Updates file path in the metadata

---

### Permission Management

#### **ADDACCESS**
Grant read or write access to another user.

**Syntax:**
```
ADDACCESS -R <filename> <username>
ADDACCESS -W <filename> <username>
```

**Flags:**
- `-R` - Grant READ permission only
- `-W` - Grant WRITE permission (includes read)

**Example:**
```
ADDACCESS -R report.txt alice
ADDACCESS -W data.txt bob
```

**Notes:**
- Only the file owner can grant access
- `-W` (write) permission includes read access
- If a user already has permissions, they are upgraded/merged:
  - Adding `-W` to a user with `-R`: Upgrades to write
  - Adding `-R` to a user with `-W`: No change (already has write)
- Each user has only one permission entry (no duplicates)

---

#### **REMACCESS**
Remove a user's access to a file.

**Syntax:**
```
REMACCESS <filename> <username>
```

**Example:**
```
REMACCESS report.txt alice
```

**Notes:**
- Only the file owner can remove access
- Completely removes the user from the ACL

---

### Version Control

#### **CHECKPOINT**
Create a named checkpoint (snapshot) of a file.

**Syntax:**
```
CHECKPOINT <filename> <checkpoint_tag>
```

**Example:**
```
CHECKPOINT myfile.txt v1
CHECKPOINT report.txt backup_before_changes
```

**Notes:**
- Requires WRITE permission
- Checkpoint tag is a label you choose
- Stored as `<filename>.checkpoint.<tag>` on storage server

---

#### **LISTCHECKPOINTS**
List all checkpoints for a file.

**Syntax:**
```
LISTCHECKPOINTS <filename>
```

**Example:**
```
LISTCHECKPOINTS myfile.txt
```

**Notes:**
- Requires READ permission
- Shows all checkpoint tags for the file

---

#### **VIEWCHECKPOINT**
View the contents of a specific checkpoint.

**Syntax:**
```
VIEWCHECKPOINT <filename> <checkpoint_tag>
```

**Example:**
```
VIEWCHECKPOINT myfile.txt v1
```

**Notes:**
- Requires READ permission
- Does not modify the current file

---

#### **REVERT**
Restore a file to a specific checkpoint.

**Syntax:**
```
REVERT <filename> <checkpoint_tag>
```

**Example:**
```
REVERT myfile.txt v1
```

**Notes:**
- Requires WRITE permission
- Overwrites current file with checkpoint content
- Creates a backup before reverting

---

### Access Request System

#### **REQUEST_ACCESS**
Request access to a file you don't own.

**Syntax:**
```
REQUEST_ACCESS <filename> <-R|-W>
```

**Flags:**
- `-R` - Request READ permission
- `-W` - Request WRITE permission

**Example:**
```
REQUEST_ACCESS report.txt -R
REQUEST_ACCESS data.txt -W
```

**Notes:**
- File must exist
- Cannot request access to your own files
- Request goes to the file owner for approval

---

#### **VIEW_REQUESTS**
View pending access requests for files you own.

**Syntax:**
```
VIEW_REQUESTS
```

**Output:**
```
Pending Access Requests for your files:
File                 | Requester            | Permission | Timestamp           
--------------------------------------------------------------------------------
document.txt         | alice                | READ       | 2025-11-18 20:30:45
report.txt           | bob                  | WRITE      | 2025-11-18 20:31:12
```

**Notes:**
- Shows only requests for files you own
- Only displays pending (unapproved/unrejected) requests

---

#### **APPROVE_REQUEST**
Approve an access request for your file.

**Syntax:**
```
APPROVE_REQUEST <filename> <requester_username>
```

**Example:**
```
APPROVE_REQUEST report.txt alice
```

**Notes:**
- Only file owners can approve requests
- Grants the requested permission to the user
- Request is marked as approved

---

#### **REJECT_REQUEST**
Reject an access request for your file.

**Syntax:**
```
REJECT_REQUEST <filename> <requester_username>
```

**Example:**
```
REJECT_REQUEST report.txt bob
```

**Notes:**
- Only file owners can reject requests
- Does NOT grant any permissions
- Requester can submit a new request later

---

### System Information

#### **VIEW**
List all files in the system (that you can access).

**Syntax:**
```
VIEW
VIEW -a
VIEW -l
VIEW -al
```

**Flags:**
- (no flag) - List only files you have permission to read
- `-a` - List ALL files in the system
- `-l` - Long format with detailed information
- `-al` or `-la` - All files in long format

**Example:**
```
VIEW
```
Output:
```
myfile.txt
report.txt
data.txt
```

```
VIEW -l
```
Output:
```
| Filename             | Words | Chars | Last Access Time | Owner      |
|----------------------|-------|-------|------------------|------------|
| myfile.txt           |   150 |   800 | 2025-11-18 18:30 | alice      |
| report.txt           |   300 |  1500 | 2025-11-18 19:00 | bob        |
```

---

#### **LIST**
List all users which are connected

**Syntax:**
```
LIST
```

---

#### **INFO**
Display detailed information about a file.

**Syntax:**
```
INFO <filename>
```

**Example:**
```
INFO myfile.txt
```

**Output:**
```
--> File: myfile.txt
--> Owner: alice
--> Created: 2025-11-18 18:00
--> Last Modified: 2025-11-18 18:30
--> Size: 1024 bytes
--> Access: alice (RW)
-->          bob (R)
-->          charlie (RW)
--> Last Accessed: 2025-11-18 18:35 by alice
```

**Notes:**
- Requires READ permission
- Shows owner, timestamps, size, access control list, and last access info
- RW = Read/Write, R = Read-only

---

## Features

### Fault Tolerance & Replication
- **Automatic File Replication** - New files are stored on two different storage servers when available
- **Intelligent Server Selection** - Round-robin algorithm ensures balanced load distribution
- **Graceful Degradation** - System works with any number of storage servers (1 to N)
- **Transparent Operations** - Users don't see backend replication complexity
- **Automatic Failover** - READ/WRITE operations use backup servers if primary fails
- **Consistency Management** - All replicated operations maintain data consistency

### Persistence
- **File Metadata** - Registry persists across nameserver restarts
- **Folder Structure** - Folder hierarchy is preserved
- **Access Control** - Permissions and ACLs are saved

### Concurrency Control
- **Sentence-level Locking** - Multiple users can edit different sentences simultaneously
- **Lock Management** - Prevents write conflicts

### Caching
- **LRU Cache** - Frequently accessed files are cached in the nameserver
- **Performance** - Reduces lookup time for popular files

---

## Implementation Details

### Replication Algorithm
The system implements a **dual-server replication strategy**:

1. **Server Selection**: 
   - Uses round-robin algorithm to select primary server
   - Selects secondary server using offset calculation to ensure different server
   - Falls back gracefully to single-server mode when only one server available

2. **File Creation Process**:
   - Name server validates request and checks available servers
   - Creates file on primary server first
   - Creates file on secondary server (if available)
   - Updates metadata only after successful primary creation
   - Secondary failure is logged but doesn't fail the operation

3. **File Access Process**:
   - READ operations try primary server first, fall back to secondary
   - WRITE operations maintain consistency across both servers
   - DELETE operations remove from both servers when possible

### Data Consistency
- **Write Consistency**: All write operations update both replicas
- **Sentence Validation**: Enforces sequential sentence writing with proper termination
- **Lock Management**: Prevents concurrent writes to same sentence across all replicas
- **Metadata Synchronization**: Central name server maintains authoritative file registry

### Error Recovery
- **Graceful Degradation**: System continues operating with reduced servers
- **Transparent Failover**: Users unaware of backend server failures
- **Validation Layers**: Multiple validation points (name server + storage server)
- **Clean Error Messages**: User-friendly error reporting without technical details

---

## Error Codes

- **400** - Bad Request (invalid sentence index, malformed parameters)
- **403** - Access denied (insufficient permissions)
- **404** - File/Folder not found
- **409** - File/Folder already exists
- **423** - File locked (sentence being edited)
- **500** - Server error
- **503** - Maximum locks/permissions reached

---

## Notes

1. **File Paths**: Use forward slashes for paths (e.g., `folder/file.txt`)
2. **Usernames**: Set at client connection time, used for ownership and permissions
3. **Zero-Based Indexing**: **All indexes start from 0** (sentences, words, positions)
4. **Write Permission**: Includes read permission automatically
5. **Backups**: System maintains `.bak` files for UNDO functionality
6. **Replication**: Files automatically replicated to two servers when available
7. **Sequential Writing**: Sentences must be written in order with proper termination (`.`, `?`, `!`)
8. **Storage Flexibility**: System adapts to 1-N storage servers seamlessly

---

## Building the Project

```bash
make clean
make all
```

This compiles:
- `nameserver` - The name server executable
- `storageserver` - The storage server executable  
- `client` - The client executable

---

## Authors : Team be creative
Aman Jayesh & Anushka Sinha 

