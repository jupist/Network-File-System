#!/bin/bash

# Fault Tolerance Test Script
# This script tests heartbeat, replication, failover, and recovery

echo "=========================================="
echo "FAULT TOLERANCE TEST SCRIPT"
echo "=========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Clean up function
cleanup() {
    print_step "Cleaning up processes..."
    pkill -f "./nameserver" 2>/dev/null
    pkill -f "./storageserver" 2>/dev/null
    pkill -f "./client" 2>/dev/null
    sleep 1
    print_success "Cleanup complete"
}

# Set trap to cleanup on script exit
trap cleanup EXIT

# Initial cleanup
cleanup

echo ""
print_step "Starting Name Server..."
./nameserver > nameserver_test.log 2>&1 &
NM_PID=$!
sleep 2

if ps -p $NM_PID > /dev/null; then
    print_success "Name Server started (PID: $NM_PID)"
else
    print_error "Name Server failed to start"
    exit 1
fi

echo ""
print_step "Starting Storage Server 1 (SS0) on ports 8081/8082..."
SS_NM_PORT=8081 SS_CLIENT_PORT=8082 ./storageserver > ss0_test.log 2>&1 &
SS0_PID=$!
sleep 2

if ps -p $SS0_PID > /dev/null; then
    print_success "Storage Server SS0 started (PID: $SS0_PID)"
else
    print_error "Storage Server SS0 failed to start"
    exit 1
fi

echo ""
print_step "Starting Storage Server 2 (SS1) on ports 8083/8084..."
SS_NM_PORT=8083 SS_CLIENT_PORT=8084 ./storageserver > ss1_test.log 2>&1 &
SS1_PID=$!
sleep 2

if ps -p $SS1_PID > /dev/null; then
    print_success "Storage Server SS1 started (PID: $SS1_PID)"
    print_warning "SS1 should replicate SS0 (check nameserver_test.log for 'SS1 will replicate SS0')"
else
    print_error "Storage Server SS1 failed to start"
    exit 1
fi

echo ""
echo "=========================================="
echo "TEST 1: HEARTBEAT MECHANISM"
echo "=========================================="
print_step "Checking if heartbeats are being sent..."
print_warning "Waiting 12 seconds to observe heartbeats..."
sleep 12

if grep -q "HEARTBEAT" nameserver_test.log; then
    print_success "Heartbeats detected in nameserver log"
    HEARTBEAT_COUNT=$(grep -c "HEARTBEAT" nameserver_test.log)
    echo "  → Found $HEARTBEAT_COUNT heartbeat messages"
else
    print_error "No heartbeats detected"
fi

echo ""
echo "=========================================="
echo "TEST 2: REPLICATION"
echo "=========================================="
print_step "Creating a test file to verify replication..."

# Create a test client session
{
    echo "testuser"
    sleep 1
    echo "CREATE test_replication.txt"
    sleep 2
} | ./client > client_test.log 2>&1 &
CLIENT_PID=$!
sleep 3

print_step "Checking if file was created on SS0..."
if [ -f "ss_storage/test_replication.txt" ]; then
    print_success "File created on primary SS0"
else
    print_warning "File not found on SS0 (check ss_storage/ directory)"
fi

print_step "Checking replication logs..."
if grep -q "Replicated command to SS1" nameserver_test.log; then
    print_success "Replication command sent to SS1"
else
    print_warning "No replication log found (may not be visible in current setup)"
fi

echo ""
echo "=========================================="
echo "TEST 3: FAILURE DETECTION"
echo "=========================================="
print_step "Simulating SS0 failure by killing its process..."
kill $SS0_PID 2>/dev/null
print_warning "SS0 killed (PID: $SS0_PID)"
print_warning "Waiting 35 seconds for heartbeat timeout (30s + buffer)..."

for i in {35..1}; do
    echo -ne "\r  Waiting... ${i}s remaining   "
    sleep 1
done
echo ""

print_step "Checking if SS0 was marked offline..."
if grep -q "marked as OFFLINE" nameserver_test.log; then
    print_success "SS0 detected as offline by Name Server"
    grep "marked as OFFLINE" nameserver_test.log | tail -1
else
    print_warning "Offline detection not found in logs yet"
fi

echo ""
echo "=========================================="
echo "TEST 4: FAILOVER"
echo "=========================================="
print_step "Attempting to read file with SS0 offline (should failover to SS1)..."

{
    echo "testuser"
    sleep 1
    echo "READ test_replication.txt"
    sleep 2
} | ./client > client_failover.log 2>&1 &
sleep 3

print_step "Checking failover logs..."
if grep -q "Primary SS offline, using replica" nameserver_test.log; then
    print_success "Failover to replica SS1 detected!"
    grep "Primary SS offline, using replica" nameserver_test.log | tail -1
else
    print_warning "Failover not detected (check nameserver_test.log)"
fi

echo ""
echo "=========================================="
echo "TEST 5: RECOVERY"
echo "=========================================="
print_step "Restarting SS0 to test recovery..."
SS_NM_PORT=8081 SS_CLIENT_PORT=8082 ./storageserver > ss0_recovery.log 2>&1 &
SS0_PID=$!
sleep 3

if ps -p $SS0_PID > /dev/null; then
    print_success "SS0 restarted (PID: $SS0_PID)"
    
    print_step "Checking recovery logs..."
    sleep 2
    if grep -q "reconnected" nameserver_test.log; then
        print_success "SS0 reconnection detected!"
        grep "reconnected" nameserver_test.log | tail -1
    else
        print_warning "Reconnection not clearly logged (check nameserver_test.log)"
    fi
else
    print_error "Failed to restart SS0"
fi

echo ""
echo "=========================================="
echo "TEST SUMMARY"
echo "=========================================="
echo ""
print_step "Log files created:"
echo "  - nameserver_test.log     (Name Server logs)"
echo "  - ss0_test.log            (SS0 initial run)"
echo "  - ss1_test.log            (SS1 logs)"
echo "  - ss0_recovery.log        (SS0 after recovery)"
echo "  - client_test.log         (Client test output)"
echo "  - client_failover.log     (Failover test output)"
echo ""

print_step "Key things to verify in nameserver_test.log:"
echo "  1. 'SS1 will replicate SS0' - replication pairing"
echo "  2. 'HEARTBEAT' messages - heartbeat mechanism working"
echo "  3. 'Replicated command to SS1' - async replication"
echo "  4. 'marked as OFFLINE' - failure detection"
echo "  5. 'Primary SS offline, using replica' - failover"
echo "  6. 'reconnected' - recovery detection"
echo ""

print_step "Manual verification commands:"
echo "  grep 'will replicate' nameserver_test.log"
echo "  grep 'HEARTBEAT' nameserver_test.log | wc -l"
echo "  grep 'Replicated command' nameserver_test.log"
echo "  grep 'OFFLINE' nameserver_test.log"
echo "  grep 'Primary SS offline' nameserver_test.log"
echo "  grep 'reconnected' nameserver_test.log"
echo ""

print_success "Fault tolerance test complete!"
print_warning "Press Ctrl+C to stop all processes and exit"
echo ""

# Keep script running to maintain processes
wait
