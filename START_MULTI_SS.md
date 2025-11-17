# How to Start Multiple Storage Servers for Fault Tolerance

## The Problem
When you run `./storageserver` without arguments, it uses default ports (9001, 9002).
You **cannot** run multiple instances on the same ports!

## The Solution
Pass different ports to each storage server as command-line arguments.

---

## Step-by-Step Instructions

### Terminal 1: Start Name Server
```bash
./nameserver
```
**Keep this running**

---

### Terminal 2: Start Storage Server 0 (Primary)
```bash
./storageserver 9001 9002 ss0_storage/
```
**Arguments:**
- `9001` = NM Port (for Name Server communication)
- `9002` = Client Port (for Client communication)
- `ss0_storage/` = Storage directory

**Keep this running**

---

### Terminal 3: Start Storage Server 1 (Replica)
```bash
mkdir -p ss1_storage
./storageserver 9003 9004 ss1_storage/
```
**Arguments:**
- `9003` = NM Port (DIFFERENT from SS0!)
- `9004` = Client Port (DIFFERENT from SS0!)
- `ss1_storage/` = Storage directory (DIFFERENT from SS0!)

**Keep this running**

---

### Terminal 4: Start Client
```bash
./client
```

---

## What Should Happen

### In Name Server Log (Terminal 1):
```
Heartbeat monitoring thread started
Name Server listening on port 8000
Accepted new connection from 127.0.0.1:XXXXX
New connection from a Storage Server (127.0.0.1:XXXXX).
  Registered SS0 (NM Port 9001, Client Port 9002)
  SS1 will replicate SS0
  Registered SS1 (NM Port 9003, Client Port 9004)
```

### In SS0 Log (Terminal 2):
```
SS: Starting with NM Port=9001, Client Port=9002, Storage Dir=ss0_storage/
SS: Connected to Name Server.
SS: Sent registration message:
REGISTER_SS 9001 9002 ...
SS: Registration complete.
SS: Heartbeat thread started
SS: Starting NM-facing server on port 9001
SS: Starting client-facing server on port 9002
```

### In SS1 Log (Terminal 3):
```
SS: Starting with NM Port=9003, Client Port=9004, Storage Dir=ss1_storage/
SS: Connected to Name Server.
SS: Sent registration message:
REGISTER_SS 9003 9004 ...
SS: Registration complete.
SS: Heartbeat thread started
SS: Starting NM-facing server on port 9003
SS: Starting client-facing server on port 9004
```

---

## Testing Fault Tolerance

### 1. Create a file (both SS should get it via replication)
```
./client
> alice
> CREATE test.txt
> WRITE test.txt 1
  Hello from primary!
> ETIRW
> EXIT
```

Check both directories:
```bash
ls -la ss0_storage/  # Should have test.txt
ls -la ss1_storage/  # Should have test.txt (replicated!)
```

### 2. Kill SS0 to test failover
In Terminal 2, press **Ctrl+C** to stop SS0.

Wait 35 seconds (heartbeat timeout is 30 seconds + buffer).

### 3. Try to read the file (should use SS1 automatically)
```
./client
> alice
> READ test.txt
  (Should still work! Name Server redirects to SS1)
```

### 4. Restart SS0 to test recovery
In Terminal 2:
```bash
./storageserver 9001 9002 ss0_storage/
```

The Name Server should detect the reconnection and mark SS0 as online again.

---

## Quick Start Script

Instead of manually opening terminals, use the provided script:

```bash
chmod +x test_fault_tolerance_multi_ss.sh
./test_fault_tolerance_multi_ss.sh
```

This will:
1. Create storage directories
2. Start Name Server
3. Start SS0 (9001/9002)
4. Start SS1 (9003/9004)
5. Show you the PIDs and instructions

---

## Troubleshooting

### "Address already in use"
- Another storageserver is already using those ports
- Kill all storageserver processes: `killall storageserver`
- Or use different port numbers

### "No such file or directory" for storage dir
- Create the directory first: `mkdir -p ss0_storage ss1_storage`

### Only one SS shows in logs
- Make sure you're using **different ports** for each SS
- Check that the second SS didn't crash due to port conflict

---

## Port Reference

| Component | Port | Used For |
|-----------|------|----------|
| Name Server | 8000 | Client & SS connections |
| SS0 NM | 9001 | Name Server ↔ SS0 |
| SS0 Client | 9002 | Client ↔ SS0 |
| SS1 NM | 9003 | Name Server ↔ SS1 |
| SS1 Client | 9004 | Client ↔ SS1 |

You can use any free ports, just make sure they're unique!
