#!/bin/bash
# Quick Fault Tolerance Demo
# Simple 3-minute demonstration of fault tolerance features

echo "================================================"
echo "FAULT TOLERANCE QUICK DEMO"
echo "================================================"
echo ""
echo "This demo will:"
echo "1. Start Name Server + 2 Storage Servers"
echo "2. Show replication pairing"
echo "3. Create a file (replicated to both SS)"
echo "4. Kill primary SS"
echo "5. Read file from replica (automatic failover)"
echo ""
echo "Press Enter to start..."
read

# Cleanup
pkill -f "./nameserver" 2>/dev/null
pkill -f "./storageserver" 2>/dev/null
sleep 1

# Start Name Server
echo "[1/7] Starting Name Server..."
./nameserver > /tmp/nm_demo.log 2>&1 &
NM_PID=$!
sleep 2
echo "      ✓ Name Server running (PID: $NM_PID)"

# Start SS0
echo "[2/7] Starting Storage Server 0 (Primary)..."
SS_NM_PORT=8081 SS_CLIENT_PORT=8082 ./storageserver > /tmp/ss0_demo.log 2>&1 &
SS0_PID=$!
sleep 2
echo "      ✓ SS0 running (PID: $SS0_PID)"

# Start SS1
echo "[3/7] Starting Storage Server 1 (Replica)..."
SS_NM_PORT=8083 SS_CLIENT_PORT=8084 ./storageserver > /tmp/ss1_demo.log 2>&1 &
SS1_PID=$!
sleep 2
echo "      ✓ SS1 running (PID: $SS1_PID)"

# Check pairing
sleep 1
if grep -q "SS1 will replicate SS0" /tmp/nm_demo.log; then
    echo "      ✓ Replication pairing configured: SS1 → SS0"
else
    echo "      ⚠ Replication pairing not detected"
fi

# Create file
echo "[4/7] Creating test file (will be replicated)..."
{
    echo "testuser"
    sleep 1
    echo "CREATE fault_test.txt"
    sleep 2
    echo "quit"
} | ./client > /tmp/client_demo.log 2>&1 &
sleep 4

if [ -f "ss_storage/fault_test.txt" ]; then
    echo "      ✓ File created on SS0"
fi

if grep -q "Replicated command to SS1" /tmp/nm_demo.log; then
    echo "      ✓ File replicated to SS1"
fi

# Wait for heartbeats
echo "[5/7] Waiting 12 seconds for heartbeats..."
sleep 12
HEARTBEAT_COUNT=$(grep -c "HEARTBEAT" /tmp/nm_demo.log)
echo "      ✓ Heartbeats received: $HEARTBEAT_COUNT"

# Kill SS0
echo "[6/7] Simulating SS0 failure..."
kill $SS0_PID 2>/dev/null
echo "      ✓ SS0 killed (PID: $SS0_PID)"
echo "      ⏳ Waiting 35 seconds for failure detection..."
sleep 35

if grep -q "marked as OFFLINE" /tmp/nm_demo.log; then
    echo "      ✓ SS0 detected as OFFLINE"
else
    echo "      ⚠ Offline detection not yet visible"
fi

# Try to read (should failover)
echo "[7/7] Reading file (should use replica SS1)..."
{
    echo "testuser"
    sleep 1
    echo "READ fault_test.txt"
    sleep 2
    echo "quit"
} | ./client > /tmp/client_failover_demo.log 2>&1 &
sleep 4

if grep -q "Primary SS offline, using replica" /tmp/nm_demo.log; then
    echo "      ✓ FAILOVER SUCCESSFUL! Request redirected to SS1"
else
    echo "      ⚠ Failover not clearly logged (check /tmp/nm_demo.log)"
fi

echo ""
echo "================================================"
echo "DEMO COMPLETE!"
echo "================================================"
echo ""
echo "Key logs to check:"
echo "  cat /tmp/nm_demo.log              # Name Server"
echo "  cat /tmp/ss0_demo.log             # SS0 (before failure)"
echo "  cat /tmp/ss1_demo.log             # SS1 (replica)"
echo ""
echo "Verify features:"
echo "  grep 'will replicate' /tmp/nm_demo.log"
echo "  grep 'HEARTBEAT' /tmp/nm_demo.log | wc -l"
echo "  grep 'Replicated command' /tmp/nm_demo.log"
echo "  grep 'OFFLINE' /tmp/nm_demo.log"
echo "  grep 'Primary SS offline' /tmp/nm_demo.log"
echo ""
echo "Cleanup:"
echo "  pkill -f nameserver"
echo "  pkill -f storageserver"
echo ""
