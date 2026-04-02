#!/bin/bash
# run_server.sh — Start the ORAM storage server.
# Usage:
#   bash scripts/run_server.sh [port]
#
# Examples:
#   bash scripts/run_server.sh          # default port 12345
#   bash scripts/run_server.sh 9999     # custom port

set -e

PORT=${1:-12345}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
SERVER="$BUILD_DIR/oram_server"

# Build if needed
if [ ! -f "$SERVER" ]; then
    echo "Building oram_server..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) oram_server > /dev/null 2>&1
    cd "$PROJECT_DIR"
    echo "  Build OK."
fi

echo "============================================"
echo " ORAM Storage Server"
echo " Port: $PORT"
echo " PID:  $$"
echo " Stop: Ctrl+C"
echo "============================================"

exec "$SERVER" --port=$PORT
