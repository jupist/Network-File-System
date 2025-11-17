#!/bin/bash
# Real-time monitoring of fault tolerance features

echo "================================================"
echo "FAULT TOLERANCE MONITOR"
echo "================================================"
echo "Monitoring nameserver.log for fault tolerance events..."
echo "Press Ctrl+C to stop"
echo ""

LOG_FILE="nameserver.log"

if [ ! -f "$LOG_FILE" ]; then
    echo "Error: $LOG_FILE not found"
    echo "Start the nameserver first!"
    exit 1
fi

# Track what we've already seen
SEEN_LINES=0

while true; do
    # Get new lines
    CURRENT_LINES=$(wc -l < "$LOG_FILE")
    
    if [ $CURRENT_LINES -gt $SEEN_LINES ]; then
        # Process new lines
        tail -n +$((SEEN_LINES + 1)) "$LOG_FILE" | while IFS= read -r line; do
            # Highlight important events
            if echo "$line" | grep -q "will replicate"; then
                echo -e "\033[0;36m[REPLICATION PAIR]\033[0m $line"
            elif echo "$line" | grep -q "HEARTBEAT"; then
                echo -e "\033[0;32m[HEARTBEAT]\033[0m $(date '+%H:%M:%S')"
            elif echo "$line" | grep -q "Replicated command"; then
                echo -e "\033[0;34m[ASYNC REPL]\033[0m $line"
            elif echo "$line" | grep -q "marked as OFFLINE"; then
                echo -e "\033[0;31m[FAILURE DETECTED]\033[0m $line"
            elif echo "$line" | grep -q "Primary SS offline, using replica"; then
                echo -e "\033[1;33m[FAILOVER]\033[0m $line"
            elif echo "$line" | grep -q "reconnected"; then
                echo -e "\033[0;35m[RECOVERY]\033[0m $line"
            elif echo "$line" | grep -q "Heartbeat monitoring thread started"; then
                echo -e "\033[0;36m[INIT]\033[0m Heartbeat monitoring active"
            fi
        done
        
        SEEN_LINES=$CURRENT_LINES
    fi
    
    sleep 1
done
