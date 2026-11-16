#!/bin/sh
# Starts the C server on a spare port and runs the end-to-end API / WebSocket checks (needs python3).
cd "$(dirname "$0")/.." || exit 1
PORT=${PORT:-8765}; export PORT
./build/synclab_server --port "$PORT" >/dev/null 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT
sleep 0.7
rc=0
for t in e2e_algorithms e2e_api e2e_websocket; do
    echo "== $t"; python3 tests/$t.py || rc=1
done
exit $rc
