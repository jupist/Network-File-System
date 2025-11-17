#!/bin/bash

# Fault Tolerance Test Script - Multiple Storage Servers
# This script demonstrates the full fault tolerance system

echo "======================================"
echo "  FAULT TOLERANCE TEST - MULTI SS"
echo "======================================"
echo ""

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Create separate storage directories for each SS
echo -e "${YELLOW}[SETUP]${NC} Creating storage directories..."
mkdir -p ss0_storage ss1_storage
echo ""

# Step 1: Start Name Server
echo -e "${YELLOW}[STEP 1]${NC} Starting Name Server..."
./nameserver > nameserver_test.log 2>&1 &
NS_PID=$!
echo "  Name Server PID: $NS_PID"
sleep 2
echo ""

# Step 2: Start SS0 (Primary - even numbered)
echo -e "${YELLOW}[STEP 2]${NC} Starting Storage Server 0 (Primary)..."
echo "  Ports: NM=9001, Client=9002, Storage=ss0_storage/"
./storageserver 9001 9002 ss0_storage/ > ss0_test.log 2>&1 &
SS0_PID=$!
echo "  SS0 PID: $SS0_PID"
sleep 2
echo ""

# Step 3: Start SS1 (Replica - odd numbered, will replicate SS0)
echo -e "${YELLOW}[STEP 3]${NC} Starting Storage Server 1 (Replica of SS0)..."
echo "  Ports: NM=9003, Client=9004, Storage=ss1_storage/"
./storageserver 9003 9004 ss1_storage/ > ss1_test.log 2>&1 &
SS1_PID=$!
echo "  SS1 PID: $SS1_PID"
sleep 2
echo ""

# Verify both SS registered
echo -e "${YELLOW}[VERIFY]${NC} Checking Name Server logs..."
if grep -q "Registered SS0" nameserver_test.log && grep -q "Registered SS1" nameserver_test.log; then
    echo -e "  ${GREEN}✓${NC} Both Storage Servers registered successfully"
    if grep -q "SS1 will replicate SS0" nameserver_test.log; then
        echo -e "  ${GREEN}✓${NC} Replication pair established: SS1 → SS0"
    fi
else
    echo -e "  ${RED}✗${NC} Registration failed - check logs"
fi
echo ""

# Verify heartbeat mechanism
echo -e "${YELLOW}[HEARTBEAT CHECK]${NC} Waiting for heartbeats (15 seconds)..."
sleep 15
if grep -q "Heartbeat monitoring thread started" nameserver_test.log; then
    echo -e "  ${GREEN}✓${NC} Heartbeat monitoring is active"
fi
if grep -q "Heartbeat thread started" ss0_test.log && grep -q "Heartbeat thread started" ss1_test.log; then
    echo -e "  ${GREEN}✓${NC} Both SS are sending heartbeats"
fi
echo ""

echo -e "${GREEN}[SUCCESS]${NC} Multi-SS setup complete!"
echo ""
echo "======================================"
echo "  SYSTEM STATUS"
echo "======================================"
echo "Name Server  : PID $NS_PID (Port 8000)"
echo "SS0 (Primary): PID $SS0_PID (NM Port 9001, Client Port 9002)"
echo "SS1 (Replica): PID $SS1_PID (NM Port 9003, Client Port 9004)"
echo ""
echo "Logs:"
echo "  nameserver_test.log - Name Server log"
echo "  ss0_test.log        - SS0 log"
echo "  ss1_test.log        - SS1 log"
echo ""
echo "======================================"
echo "  MANUAL TESTING INSTRUCTIONS"
echo "======================================"
echo ""
echo "1. Connect a client and create a file:"
echo "   ./client"
echo "   > alice"
echo "   > CREATE test.txt"
echo ""
echo "2. Check both SS directories:"
echo "   ls -la ss0_storage/  # Should have test.txt"
echo "   ls -la ss1_storage/  # Should have test.txt (replicated)"
echo ""
echo "3. Test failover - Kill SS0:"
echo "   kill $SS0_PID"
echo "   sleep 35  # Wait for heartbeat timeout"
echo ""
echo "4. Try to READ the file (should failover to SS1):"
echo "   ./client"
echo "   > alice"
echo "   > READ test.txt"
echo "   (Should successfully read from SS1)"
echo ""
echo "5. Test recovery - Restart SS0:"
echo "   ./storageserver 9001 9002 ss0_storage/ &"
echo "   (SS0 should reconnect and be marked online)"
echo ""
echo -e "${YELLOW}Press Ctrl+C to stop all servers and cleanup${NC}"
echo ""

# Wait for user interrupt
trap "echo ''; echo 'Cleaning up...'; kill $NS_PID $SS0_PID $SS1_PID 2>/dev/null; echo 'All processes stopped.'; exit" INT TERM

wait
