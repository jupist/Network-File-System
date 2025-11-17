#!/bin/bash

# Manual Storage Server Creation Guide
# This script shows you the commands to run in each terminal

NUM_SERVERS=${1:-4}  # Default to 4 servers if no argument provided

echo "=========================================="
echo "  Manual Storage Server Setup"
echo "  Creating $NUM_SERVERS Storage Servers"
echo "=========================================="
echo ""

echo "Step 1: Start Name Server"
echo "----------------------------------------"
echo "Terminal 1:"
echo "  ./nameserver"
echo ""

echo "Step 2: Create Storage Directories"
echo "----------------------------------------"
echo "Run this ONCE in any terminal:"
MKDIR_CMD="mkdir -p"
for i in $(seq 0 $((NUM_SERVERS - 1))); do
    MKDIR_CMD="$MKDIR_CMD ss${i}_storage"
done
echo "  $MKDIR_CMD"
echo ""

echo "Step 3: Start Storage Servers"
echo "----------------------------------------"
for i in $(seq 0 $((NUM_SERVERS - 1))); do
    NM_PORT=$((9001 + i * 2))
    CLIENT_PORT=$((9002 + i * 2))
    STORAGE_DIR="ss${i}_storage/"
    
    TERMINAL_NUM=$((i + 2))
    echo "Terminal $TERMINAL_NUM (Storage Server $i):"
    echo "  ./storageserver $NM_PORT $CLIENT_PORT $STORAGE_DIR"
    
    # Show replication info
    if [ $((i % 2)) -eq 1 ]; then
        REPLICA_OF=$((i - 1))
        echo "  → This will REPLICATE SS$REPLICA_OF"
    fi
    echo ""
done

echo "Step 4: Start Client"
echo "----------------------------------------"
TERMINAL_NUM=$((NUM_SERVERS + 2))
echo "Terminal $TERMINAL_NUM:"
echo "  ./client"
echo ""

echo "=========================================="
echo "  Port Allocation Summary"
echo "=========================================="
echo ""
printf "%-5s %-10s %-12s %-15s %-15s\n" "SS#" "NM Port" "Client Port" "Storage Dir" "Replicates"
echo "----------------------------------------------------------------------"
for i in $(seq 0 $((NUM_SERVERS - 1))); do
    NM_PORT=$((9001 + i * 2))
    CLIENT_PORT=$((9002 + i * 2))
    STORAGE_DIR="ss${i}_storage/"
    
    if [ $((i % 2)) -eq 1 ]; then
        REPLICA_OF=$((i - 1))
        REPL_INFO="SS$REPLICA_OF"
    else
        REPL_INFO="(primary)"
    fi
    
    printf "%-5s %-10s %-12s %-15s %-15s\n" "SS$i" "$NM_PORT" "$CLIENT_PORT" "$STORAGE_DIR" "$REPL_INFO"
done
echo ""

echo "=========================================="
echo "  Copy-Paste Commands"
echo "=========================================="
echo ""
echo "# Terminal 1 - Name Server"
echo "./nameserver"
echo ""
echo "# Create all directories (run once)"
echo "$MKDIR_CMD"
echo ""
for i in $(seq 0 $((NUM_SERVERS - 1))); do
    NM_PORT=$((9001 + i * 2))
    CLIENT_PORT=$((9002 + i * 2))
    STORAGE_DIR="ss${i}_storage/"
    echo "# Terminal $((i + 2)) - SS$i"
    echo "./storageserver $NM_PORT $CLIENT_PORT $STORAGE_DIR"
    echo ""
done
echo "# Client"
echo "./client"
echo ""

echo "=========================================="
echo "  To Stop All Servers"
echo "=========================================="
echo ""
echo "killall nameserver storageserver"
echo ""
