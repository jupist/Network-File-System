# 🔥 FAULT TOLERANCE - QUICK REFERENCE

## Why You Couldn't Run Two Storage Servers

**The Problem:**
- Storage servers had **hardcoded ports** (9001, 9002)
- Running `./storageserver` twice tried to use the **same ports**
- Second server got "Address already in use" error and crashed

**The Solution:**
- Modified storage server to **accept command-line arguments**
- Each storage server gets **unique ports**

---

## How to Run Multiple Storage Servers NOW

### Option 1: Use the Automated Script (EASIEST!)

```bash
./start_servers.sh
```

Done! All servers start automatically with correct ports.

---

### Option 2: Manual Start (4 Terminals)

**Terminal 1:**
```bash
./nameserver
```

**Terminal 2:**
```bash
./storageserver 9001 9002 ss0_storage/
```

**Terminal 3:**
```bash
./storageserver 9003 9004 ss1_storage/
```

**Terminal 4:**
```bash
./client
```

---

## Command Format

```bash
./storageserver <nm_port> <client_port> [storage_dir]
```

**Example:**
```bash
./storageserver 9001 9002 ss0_storage/
```
- `9001` = Port for Name Server to talk to this SS
- `9002` = Port for Clients to talk to this SS
- `ss0_storage/` = Where this SS stores files

---

## What Happens with Fault Tolerance

### 1. Registration
```
Name Server: "SS0 registered on ports 9001/9002"
Name Server: "SS1 registered on ports 9003/9004"
Name Server: "SS1 will replicate SS0"
```

### 2. Heartbeats (Every 10 seconds)
```
SS0 → Name Server: "HEARTBEAT 9001"
SS1 → Name Server: "HEARTBEAT 9003"
Name Server: "SS0 is online" ✓
Name Server: "SS1 is online" ✓
```

### 3. Replication (On every write)
```
Client: "CREATE test.txt"
Name Server → SS0: "CREATE_FILE test.txt"
Name Server → SS1: "CREATE_FILE test.txt" (async replication!)
```

### 4. Failure Detection (After 30 seconds no heartbeat)
```
Name Server: "SS0 marked as OFFLINE (no heartbeat for 35 seconds)"
```

### 5. Failover (Automatic!)
```
Client: "READ test.txt"
Name Server: "SS0 is offline, redirecting to SS1"
Client gets file from SS1! ✓
```

### 6. Recovery (When SS restarts)
```
SS0: "REGISTER_SS 9001 9002 ..."
Name Server: "SS0 reconnected. Marked as online."
```

---

## Testing Checklist

- [ ] Start all 3 components (NS, SS0, SS1)
- [ ] Verify both SS registered (check NS logs)
- [ ] Create a file → check both ss0_storage/ and ss1_storage/
- [ ] Kill SS0 → wait 35 seconds
- [ ] Try to READ file → should work via SS1
- [ ] Restart SS0 → should reconnect automatically

---

## Common Issues

### "Address already in use"
**Cause:** Another process is using those ports
**Fix:**
```bash
killall storageserver nameserver
# Then restart with correct ports
```

### "Only one SS shows up"
**Cause:** Second SS crashed due to port conflict
**Fix:** Use **different ports** for each SS (like 9001/9002 and 9003/9004)

### "Files not replicating"
**Cause:** SS1 might not be registered as replica
**Check:** Name Server logs should show "SS1 will replicate SS0"

---

## Files Created

- `START_MULTI_SS.md` - Detailed instructions
- `start_servers.sh` - Automated startup script
- `FAULT_TOLERANCE_TESTING.md` - Updated testing guide

---

## Summary

✅ **Fixed:** Storage servers can now run on different ports
✅ **Added:** Command-line argument support
✅ **Created:** Easy startup script
✅ **Ready:** Full fault tolerance with replication and failover!

**Next:** Run `./start_servers.sh` and test it!
