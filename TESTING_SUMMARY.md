# Fault Tolerance Implementation - Testing Guide

## 🚀 Quick Start

Choose your testing method:

### Option 1: Automated Full Test (Recommended)
```bash
./test_fault_tolerance.sh
```
- **Duration:** ~60 seconds
- **What it does:** Tests all features automatically
- **Output:** Colored status messages and log files

### Option 2: Quick 3-Minute Demo
```bash
./quick_demo_fault_tolerance.sh
```
- **Duration:** ~3 minutes
- **What it does:** Simple demonstration of key features
- **Output:** Step-by-step progress with verification

### Option 3: Manual Testing
See `FAULT_TOLERANCE_TESTING.md` for detailed step-by-step instructions.

---

## 📊 Real-Time Monitoring

While testing, run this in a separate terminal:
```bash
./monitor_fault_tolerance.sh
```

This will show real-time events:
- 🔵 **[HEARTBEAT]** - Heartbeat received
- 🟢 **[REPLICATION PAIR]** - SS pairing configured
- 🔷 **[ASYNC REPL]** - File replicated to backup
- 🔴 **[FAILURE DETECTED]** - SS marked offline
- 🟡 **[FAILOVER]** - Request redirected to replica
- 🟣 **[RECOVERY]** - SS reconnected

---

## 🧪 What Gets Tested

| Feature | Test Method | Expected Result |
|---------|-------------|-----------------|
| **Heartbeat Mechanism** | Wait 10-12 seconds | HEARTBEAT messages in log |
| **Replication Pairing** | Start 2 SS | "SS1 will replicate SS0" |
| **Async Replication** | CREATE/DELETE file | "Replicated command to SS1" |
| **Failure Detection** | Kill SS0, wait 35s | "SS0 marked as OFFLINE" |
| **Automatic Failover** | READ with SS0 down | "Primary SS offline, using replica" |
| **Recovery Detection** | Restart SS0 | "SS0 reconnected" |

---

## 📝 Test Files & Logs

After running tests, check these files:

**Automated test logs:**
- `nameserver_test.log` - Name Server events
- `ss0_test.log` - Primary SS logs
- `ss1_test.log` - Replica SS logs
- `ss0_recovery.log` - Recovery test logs
- `client_test.log` - Client operations
- `client_failover.log` - Failover test

**Quick demo logs (in /tmp):**
- `/tmp/nm_demo.log`
- `/tmp/ss0_demo.log`
- `/tmp/ss1_demo.log`
- `/tmp/client_demo.log`

---

## 🔍 Verification Commands

### Check Replication Setup
```bash
grep "will replicate" nameserver.log
```
**Expected:** `SS1 will replicate SS0`

### Count Heartbeats
```bash
grep "HEARTBEAT" nameserver.log | wc -l
```
**Expected:** Number increases every 10 seconds

### Check Replication Activity
```bash
grep "Replicated command" nameserver.log
```
**Expected:** CREATE_FILE, DELETE_FILE, etc. sent to replica

### Verify Failure Detection
```bash
grep "OFFLINE" nameserver.log
```
**Expected:** "SS0 marked as OFFLINE" after 30+ seconds no heartbeat

### Verify Failover
```bash
grep "Primary SS offline" nameserver.log
```
**Expected:** "Primary SS offline, using replica SS1"

### Verify Recovery
```bash
grep "reconnected" nameserver.log
```
**Expected:** "SS0 reconnected"

---

## 🎯 Testing Scenarios

### Scenario 1: Basic Functionality Test
1. Start nameserver + 2 storage servers
2. Create a file
3. Verify file appears on primary and gets replicated
4. Read the file successfully

### Scenario 2: Failure and Failover
1. Complete Scenario 1
2. Kill primary SS (Ctrl+C or `kill -9`)
3. Wait 35 seconds
4. Try to read the file → should work via replica
5. Try to write → should work via replica

### Scenario 3: Recovery
1. Complete Scenario 2
2. Restart primary SS
3. Verify it reconnects
4. New operations should work on primary again

### Scenario 4: Multiple SS Pairs
1. Start 4 storage servers (SS0, SS1, SS2, SS3)
2. Verify pairing: SS1→SS0, SS3→SS2
3. Test failover for each pair independently

---

## 🐛 Troubleshooting

### "No heartbeats detected"
**Problem:** Heartbeat thread not starting
**Solution:** 
- Check `storageserver.log` for "Heartbeat thread started"
- Ensure SS connected to nameserver successfully

### "Replication not working"
**Problem:** Commands not being replicated
**Solution:**
- Ensure SS1 started AFTER SS0
- Check "SS1 will replicate SS0" appears in logs
- Only write operations (CREATE, DELETE, MOVE) are replicated

### "Failover not happening"
**Problem:** Still using offline SS
**Solution:**
- Wait full 30+ seconds after killing SS
- Verify SS actually marked OFFLINE in logs
- Ensure replica SS is still running

### "Recovery not detected"
**Problem:** SS not recognized as reconnecting
**Solution:**
- Use EXACT same ports when restarting
- Check SS was previously registered
- Look for "reconnected" vs "Registered SS" (new registration)

---

## 🧹 Cleanup

After testing, stop all processes:

```bash
# Kill all components
pkill -f nameserver
pkill -f storageserver
pkill -f client

# Or use the automated cleanup
./test_fault_tolerance.sh  # Press Ctrl+C to trigger cleanup
```

Remove test files:
```bash
rm -f *_test.log *_demo.log /tmp/*_demo.log
rm -f ss_storage/test_*.txt ss_storage/fault_test.txt
```

---

## 📈 Performance Notes

**Heartbeat Overhead:**
- Heartbeat sent every 10 seconds
- Minimal network overhead (~50 bytes per heartbeat)
- CPU impact: negligible

**Replication Overhead:**
- Async replication (non-blocking)
- No impact on client latency
- Network: 1 extra message per write operation

**Failover Time:**
- Detection: 30 seconds (configurable via HEARTBEAT_TIMEOUT)
- Redirection: <1ms (immediate)
- Total: ~30 seconds from failure to automatic failover

---

## 🎓 Understanding the Implementation

### Heartbeat Flow
```
SS0 ──(every 10s)──> NM: "HEARTBEAT 8081"
                     NM: Updates last_heartbeat, is_online=1
                     
NM Monitor Thread ──(every 10s)──> Check all SS
                                   If (now - last_heartbeat) > 30s
                                   → Mark is_online=0
```

### Replication Flow
```
Client: "CREATE test.txt"
   ↓
NM: forward_create_to_ss(SS0) ──> SS0 creates file
   ↓                              (waits for ACK)
NM: replicate_to_backup_ss(SS0) ──> SS1 creates file
                                     (async, no wait)
```

### Failover Flow
```
Client: "READ test.txt"
   ↓
NM: Find file → belongs to SS0
   ↓
NM: Check SS0.is_online
   ↓
   if (SS0.is_online == 0):
       Use SS0.replicated_by (SS1)
       Return SS1 location to client
   ↓
Client: Connects to SS1 instead of SS0
```

---

## 📚 Additional Resources

- **Full Manual Testing Guide:** `FAULT_TOLERANCE_TESTING.md`
- **Implementation Details:** See comments in:
  - `nm_modules/nm_types.h` (data structures)
  - `nameserver.c` (heartbeat monitoring, failover logic)
  - `storageserver.c` (heartbeat sending)

---

## ✅ Success Checklist

Before considering testing complete, verify:

- [ ] Name Server starts with "Heartbeat monitoring thread started"
- [ ] Storage Servers show "Heartbeat thread started"
- [ ] Replication pairing appears: "SS1 will replicate SS0"
- [ ] Heartbeats appear in logs every ~10 seconds
- [ ] File creation shows "Replicated command to SS1"
- [ ] Killing SS0 → "marked as OFFLINE" after 30+ seconds
- [ ] Read with SS0 down → "Primary SS offline, using replica"
- [ ] Restarting SS0 → "reconnected"
- [ ] All features work without errors

If all items checked, **fault tolerance is working correctly!** 🎉
