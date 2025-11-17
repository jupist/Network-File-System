#!/bin/bash

# Simple script to start multiple storage servers with correct ports

echo "=========================================="
echo "  Starting Multi-Storage Server Setup"
echo "=========================================="
echo ""

# Create storage directories
echo "[1/4] Creating storage directories..."
mkdir -p ss0_storage ss1_storage
echo "      ✓ ss0_storage/ and ss1_storage/ created"
echo ""

# Start Name Server
echo "[2/4] Starting Name Server on port 8000..."
./nameserver &
NS_PID=$!
echo "      ✓ Name Server started (PID: $NS_PID)"
sleep 2
echo ""

# Start SS0
echo "[3/4] Starting Storage Server 0..."
echo "      Ports: NM=9001, Client=9002"
echo "      Storage: ss0_storage/"
./storageserver 9001 9002 ss0_storage/ &
SS0_PID=$!
echo "      ✓ SS0 started (PID: $SS0_PID)"
sleep 2
echo ""

# Start SS1
echo "[4/4] Starting Storage Server 1 (Replica)..."
echo "      Ports: NM=9003, Client=9004"
echo "      Storage: ss1_storage/"
./storageserver 9003 9004 ss1_storage/ &
SS1_PID=$!
echo "      ✓ SS1 started (PID: $SS1_PID)"
sleep 2
echo ""

echo "=========================================="
echo "  ALL SERVERS RUNNING!"
echo "=========================================="
echo ""
echo "Name Server:  PID $NS_PID (Port 8000)"
echo "SS0 (Primary): PID $SS0_PID (NM=9001, Client=9002)"
echo "SS1 (Replica): PID $SS1_PID (NM=9003, Client=9004)"
echo ""
echo "Replication: SS1 → SS0 (odd replicates even)"
echo ""
echo "=========================================="
echo "  NEXT STEPS"
echo "=========================================="
echo ""
echo "1. Start a client in another terminal:"
echo "   ./client"
echo ""
echo "2. Create a file to test replication:"
echo "   > alice"
echo "   > CREATE test.txt"
echo ""
echo "3. Check both storage dirs:"
echo "   ls -la ss0_storage/  # Primary"
echo "   ls -la ss1_storage/  # Replica (should have same file!)"
echo ""
echo "4. Test failover - kill SS0:"
echo "   kill $SS0_PID"
echo "   (Wait 35 seconds for heartbeat timeout)"
echo "   Then try: READ test.txt (should work via SS1!)"
echo ""
echo "=========================================="
echo "  TO STOP ALL SERVERS"
echo "=========================================="
echo ""
echo "Run these commands:"
echo "  kill $NS_PID $SS0_PID $SS1_PID"
echo ""
echo "Or use:"
echo "  killall nameserver storageserver"
echo ""
