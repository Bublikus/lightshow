#!/bin/bash

echo "🔍 Looking for processes using /dev/cu.usbserial-*..."

# Find and kill matching processes
PIDS=$(lsof | grep /dev/cu.usbserial | awk '{print $2}' | sort | uniq)

if [ -z "$PIDS" ]; then
  echo "✅ No processes using USB serial ports found."
else
  echo "⚠️ Found processes using USB serial ports: $PIDS"
  for PID in $PIDS; do
    echo "🛑 Killing PID $PID..."
    kill -9 "$PID"
  done
  echo "✅ Done. USB serial ports should now be free."
fi