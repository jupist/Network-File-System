# Fault Tolerance Testing Guide

## ⚠️ IMPORTANT: How to Start Multiple Storage Servers

**The storage servers now support command-line arguments for ports!**

You **CANNOT** run multiple storage servers without specifying different ports.
Each storage server needs its own unique ports.

### Quick Start (Automated)

Use the startup script:
```bash
./start_servers.sh
```

This will automatically start:
- Name Server on port 8000
- Storage Server 0 on ports 9001/9002 with directory `ss0_storage/`
- Storage Server 1 on ports 9003/9004 with directory `ss1_storage/`

### Manual Start (Step by Step)

**Terminal 1 - Name Server:**
```bash
./nameserver
```

**Terminal 2 - Storage Server 0 (Primary):**
```bash
./storageserver 9001 9002 ss0_storage/
```
- First argument: NM Port (9001)
- Second argument: Client Port (9002)
- Third argument: Storage directory (ss0_storage/)

**Terminal 3 - Storage Server 1 (Replica):**
```bash
mkdir -p ss1_storage
./storageserver 9003 9004 ss1_storage/
```
- First argument: NM Port (9003) - DIFFERENT from SS0!
- Second argument: Client Port (9004) - DIFFERENT from SS0!
- Third argument: Storage directory (ss1_storage/) - DIFFERENT from SS0!

**Terminal 4 - Client:**
```bash
./client
```

---

## What You Should See

### In Name Server (Terminal 1):
```
Heartbeat monitoring thread started
Name Server listening on port 8000
Accepted new connection from 127.0.0.1:XXXXX
New connection from a Storage Server (127.0.0.1:XXXXX).
  Registered SS0 (NM Port 9001, Client Port 9002)
  SS1 will replicate SS0
  Registered SS1 (NM Port 9003, Client Port 9004)
```

### In Storage Server 0 (Terminal 2):
```
SS: Starting with NM Port=9001, Client Port=9002, Storage Dir=ss0_storage/
SS: Connected to Name Server.
SS: Heartbeat thread started
SS: Starting NM-facing server on port 9001
SS: Starting client-facing server on port 9002
```

### In Storage Server 1 (Terminal 3):
```
SS: Starting with NM Port=9003, Client Port=9004, Storage Dir=ss1_storage/
SS: Connected to Name Server.
SS: Heartbeat thread started
SS: Starting NM-facing server on port 9003
SS: Starting client-facing server on port 9004
```

---

## TEST 1: Heartbeat Mechanism

**What to look for in Name Server output:**
- "Heartbeat monitoring thread started" on startup
- HEARTBEAT messages appearing periodically

**What to look for in Storage Server output:**
- "Heartbeat thread started" on startup

**Verification:**
```bash
# Check nameserver.log for heartbeat messages
grep "HEARTBEAT" nameserver.log
```

**Expected:** You should see heartbeat messages every 10 seconds from each SS.

---

### TEST 2: Replication Configuration

**What to look for in Name Server output:**
```
Registered SS0 (NM Port 8081, Client Port 8082)
Registered SS1 (NM Port 8083, Client Port 8084)
SS1 will replicate SS0
```

**Verification:**
- Check that SS1 is configured to replicate SS0
- This happens automatically when odd-numbered SS registers

---

### TEST 3: Asynchronous Replication

**Terminal 4 - Client:**
```bash
./client
```

**Client commands:**
```
testuser
CREATE replication_test.txt
WRITE replication_test.txt 1
Hello from primary!
.
```

**What to look for in Name Server output:**
```
Replicated command to SS1: CREATE_FILE replication_test.txt
```

**Verification:**
The CREATE command should be sent to both SS0 (primary) and SS1 (replica) asynchronously.

---

### TEST 4: Failure Detection

**Simulate Failure:**
In Terminal 2 (SS0), press Ctrl+C to kill the storage server.

**What to look for in Name Server output (after ~30 seconds):**
```
SS0 marked as OFFLINE (no heartbeat for XX seconds)
```

**Verification:**
```bash
# Wait 35 seconds after killing SS0, then check:
grep "OFFLINE" nameserver.log
```

**Expected:** SS0 should be automatically marked offline after 30 seconds without heartbeat.

---

### TEST 5: Automatic Failover

**With SS0 still offline, try to read a file:**

**Terminal 4 - Client:**
```
READ replication_test.txt
```

**What to look for in Name Server output:**
```
Primary SS offline, using replica SS1
```

**What client receives:**
```
SS_LOCATION 127.0.0.1 8084 replication_test.txt
```

**Verification:**
- Client should receive location of SS1 (port 8084) instead of SS0 (port 8082)
- File access should work transparently through replica

---

### TEST 6: Storage Server Recovery

**Restart SS0:**

**Terminal 2:**
```bash
SS_NM_PORT=8081 SS_CLIENT_PORT=8082 ./storageserver
```

**What to look for in Name Server output:**
```
SS0 reconnected (NM Port 8081, Client Port 8082)
```

**Verification:**
```bash
grep "reconnected" nameserver.log
```

**Expected:** 
- SS0 should be marked online again
- Heartbeats resume
- New requests can use SS0 as primary again

---

## Verification Commands

Check all fault tolerance features at once:

```bash
# Replication pairing
grep "will replicate" nameserver.log

# Heartbeat activity (count should increase over time)
grep "HEARTBEAT" nameserver.log | wc -l

# Replication commands sent
grep "Replicated command" nameserver.log

# Failure detection
grep "OFFLINE" nameserver.log

# Failover events
grep "Primary SS offline" nameserver.log

# Recovery events
grep "reconnected" nameserver.log
```

---

## Expected Behavior Summary

| Feature | Expected Behavior | Verification |
|---------|------------------|--------------|
| **Heartbeat** | SS sends every 10s | grep "HEARTBEAT" nameserver.log |
| **Replication Pairing** | SS1 replicates SS0 | "SS1 will replicate SS0" in output |
| **Async Replication** | Write ops sent to both | "Replicated command" in nameserver.log |
| **Failure Detection** | Offline after 30s no heartbeat | "marked as OFFLINE" in output |
| **Failover** | Redirect to replica when primary down | "using replica SS" in output |
| **Recovery** | Auto-detect reconnection | "reconnected" in output |

---

## Troubleshooting

**No heartbeats visible:**
- Check storageserver.log for "Heartbeat thread started"
- Check nameserver.log for heartbeat messages
- Ensure both servers are running

**No replication:**
- Ensure SS1 registered after SS0
- Check "SS1 will replicate SS0" message on startup
- File operations must go through Name Server to trigger replication

**Failover not working:**
- Ensure SS0 is actually marked OFFLINE (wait full 30+ seconds)
- Check that SS1 is still running and online
- Verify client is requesting through Name Server

**Recovery not detected:**
- Use same ports when restarting SS0
- Check that SS was previously registered
- Look for "reconnected" vs "Registered" (new registration)

---

## Clean Up

To stop all processes:
```bash
pkill -f nameserver
pkill -f storageserver
pkill -f client
```

Or press Ctrl+C in each terminal window.

---

## Advanced Testing

### Test Multiple Failures
1. Start SS0, SS1, SS2, SS3 (SS1→SS0, SS3→SS2)
2. Kill SS0 and SS2
3. Verify SS1 and SS3 handle requests

### Test Load During Failover
1. Start continuous writes to SS0
2. Kill SS0 mid-operation
3. Verify writes continue through SS1

### Test Recovery Sync
1. Kill SS0
2. Write new files through SS1 (replica)
3. Restart SS0
4. Verify SS0 reconnects (full sync would need additional implementation)
