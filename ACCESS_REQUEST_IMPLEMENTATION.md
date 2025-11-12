# Access Request System Implementation

## ✅ Complete Implementation

The access request system allows users to request access to files they don't own. File owners can then view, approve, or reject these requests.

### Important: Command Matching Order
Commands are checked in specific order to prevent prefix matching issues:
- `VIEW_REQUESTS` (13 chars) checked **before** `VIEW` (4 chars)
- This prevents "VIEW_REQUESTS" from being matched as "VIEW"

---

## Commands Implemented

### 1. REQUEST_ACCESS <filename> <permission>
**Purpose**: Request read or write access to a file you don't own  
**Format**: `REQUEST_ACCESS <filename> <R|W|READ|WRITE>`  
**Permission Types**:
- `R` or `READ` - Request read-only access
- `W` or `WRITE` - Request read and write access

**Example Usage**:
```bash
Docs++ > REQUEST_ACCESS document.txt R
SUCCESS: Access request sent to owner.

Docs++ > REQUEST_ACCESS report.txt WRITE
SUCCESS: Access request sent to owner.
```

**Response**: `SUCCESS: Access request sent to owner.` or error message

**Validations**:
- File must exist
- You cannot request access to your own files
- You cannot request if you already have sufficient permission
- Cannot create duplicate pending requests

---

### 2. VIEW_REQUESTS
**Purpose**: View all pending access requests for files you own  
**Format**: `VIEW_REQUESTS`

**Example Usage**:
```bash
Docs++ > VIEW_REQUESTS
Pending Access Requests for your files:
File                 | Requester            | Permission | Timestamp           
--------------------------------------------------------------------------------
document.txt         | alice                | READ       | 2025-11-12 20:30:45
report.txt           | bob                  | WRITE      | 2025-11-12 20:31:12
```

**Response**: Formatted table of pending requests or "(No pending requests)"

**Display Format**:
- Filename
- Requester username
- Requested permission (READ/WRITE)
- Timestamp of request

---

### 3. APPROVE_REQUEST <filename> <requester>
**Purpose**: Approve an access request and grant permission  
**Format**: `APPROVE_REQUEST <filename> <requester_username>`

**Example Usage**:
```bash
Docs++ > APPROVE_REQUEST document.txt alice
SUCCESS: Access granted.
```

**Response**: `SUCCESS: Access granted.` or error message

**Actions**:
- Grants the requested permission to the user
- Updates the file's ACL (Access Control List)
- Marks the request as approved (status = 1)
- Persists changes to disk
- If user already has an ACL entry, updates it appropriately

---

### 4. REJECT_REQUEST <filename> <requester>
**Purpose**: Reject an access request and deny permission  
**Format**: `REJECT_REQUEST <filename> <requester_username>`

**Example Usage**:
```bash
Docs++ > REJECT_REQUEST report.txt bob
SUCCESS: Request rejected.
```

**Response**: `SUCCESS: Request rejected.` or error message

**Actions**:
- Marks the request as rejected (status = 2)
- Does NOT grant any permissions
- Requester can submit a new request later if needed

---

## Data Structure

### AccessRequest Structure
```c
typedef struct {
    char filename[256];              // File being requested
    char requester_username[256];    // User requesting access
    char owner_username[256];        // File owner
    char requested_permission;       // 'R' or 'W'
    char timestamp[128];             // When request was made
    int status;                      // 0=pending, 1=approved, 2=rejected
} AccessRequest;
```

### Global Storage
- Array: `AccessRequest g_access_requests[MAX_ACCESS_REQUESTS]`
- Counter: `int g_num_access_requests`
- Maximum: `MAX_ACCESS_REQUESTS = 100`

---

## Implementation Details

### Location in Code

**File**: `nameserver.c`

**Command Handlers**:
- `VIEW_REQUESTS` - Lines ~898-940 (checked BEFORE VIEW command)
- `REQUEST_ACCESS` - Lines ~2145-2268
- `APPROVE_REQUEST` - Lines ~2270-2360
- `REJECT_REQUEST` - Lines ~2362-2404

**Data Structure**: `nm_modules/nm_types.h` (Lines ~58-65)

**Note**: VIEW_REQUESTS is positioned early in the command checking order to avoid being matched by the generic VIEW command.

---

## Workflow Example

### Complete Access Request Flow

#### 1. User Requests Access
```bash
# Alice wants to read Bob's file
alice@Docs++ > REQUEST_ACCESS bobfile.txt R
SUCCESS: Access request sent to owner.
```

#### 2. Owner Views Requests
```bash
# Bob checks his pending requests
bob@Docs++ > VIEW_REQUESTS
Pending Access Requests for your files:
File                 | Requester            | Permission | Timestamp           
--------------------------------------------------------------------------------
bobfile.txt          | alice                | READ       | 2025-11-12 20:30:45
```

#### 3a. Owner Approves Request
```bash
# Bob approves Alice's request
bob@Docs++ > APPROVE_REQUEST bobfile.txt alice
SUCCESS: Access granted.

# Alice can now read the file
alice@Docs++ > READ bobfile.txt
[file content displayed]
```

#### 3b. Owner Rejects Request
```bash
# Or Bob rejects the request
bob@Docs++ > REJECT_REQUEST bobfile.txt alice
SUCCESS: Request rejected.

# Alice still cannot access the file
alice@Docs++ > READ bobfile.txt
ERROR: File not found or access denied.
```

---

## Security & Validation

### REQUEST_ACCESS Validations
✅ File must exist  
✅ Cannot request access to own files  
✅ Cannot request if already have sufficient permission  
✅ Cannot create duplicate pending requests  
✅ Validates permission type (R or W)  

### APPROVE_REQUEST Validations
✅ Request must exist and be pending  
✅ Only file owner can approve  
✅ File must still exist  
✅ Handles ACL updates correctly  
✅ Persists changes to disk  

### REJECT_REQUEST Validations
✅ Request must exist and be pending  
✅ Only file owner can reject  
✅ Marks request as rejected  

---

## Permission Integration

### How It Works with ACL
When a request is approved:

1. **Check Existing ACL Entry**:
   - If user already has an entry → update permission
   - If requesting WRITE and user has READ → upgrade to WRITE
   - If requesting READ and user has WRITE → keep WRITE

2. **Add New ACL Entry**:
   - If no entry exists → add new permission
   - Maximum `MAX_PERMISSIONS = 10` per file

3. **Persist Changes**:
   - Changes saved to `nm_registry.dat`
   - Survives server restarts

---

## Request States

| Status | Value | Description |
|--------|-------|-------------|
| Pending | 0 | Request awaiting owner action |
| Approved | 1 | Request approved, permission granted |
| Rejected | 2 | Request denied by owner |

---

## Error Handling

### Common Error Messages

| Error | Cause |
|-------|-------|
| `File not found` | Requested file doesn't exist |
| `You are the owner of this file` | Cannot request access to own files |
| `You already have sufficient permission` | Already have requested or higher permission |
| `You already have a pending request` | Duplicate request detected |
| `Maximum access requests reached` | System limit hit (100 requests) |
| `No pending request found` | Request doesn't exist or already processed |
| `Maximum permissions reached` | File has 10 ACL entries already |

---

## Features

✅ **Simple Storage**: No push notifications needed, just view/approve/reject  
✅ **Timestamp Tracking**: Records when each request was made  
✅ **Status Tracking**: Pending/Approved/Rejected states  
✅ **Permission Types**: Separate READ and WRITE requests  
✅ **ACL Integration**: Automatically updates file permissions  
✅ **Persistence**: Changes saved to disk  
✅ **Validation**: Comprehensive error checking  
✅ **Owner Control**: Only owners can approve/reject  

---

## Build Status

✅ **Successfully compiled** with no errors or warnings!

```bash
make clean && make all
```

---

## Testing Scenarios

### Test 1: Basic Request Flow
1. User A creates file
2. User B requests READ access
3. User A views requests
4. User A approves request
5. User B can now read file

### Test 2: Duplicate Request Prevention
1. User B requests access to file
2. User B tries to request again → ERROR

### Test 3: Permission Upgrade
1. User B has READ permission
2. User B requests WRITE access
3. Owner approves
4. User B now has WRITE permission

### Test 4: Rejection Flow
1. User B requests access
2. Owner rejects request
3. User B can submit new request later

---

## Notes

- Rejected requests can be re-requested (status changes from 2 back to 0)
- Approved requests integrate with existing permission system
- Request history is maintained (approved/rejected requests stay in array)
- Owner can see only PENDING requests in VIEW_REQUESTS
- System supports up to 100 concurrent access requests
- Each file can have up to 10 ACL entries

