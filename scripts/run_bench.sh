#!/usr/bin/env bash
# Runs a quick localhost benchmark: spins up tick-relay, fires the replay
# driver at it for a fixed number of messages, then prints the histogram
# summary the server emits on shutdown.
set -euo pipefail

PORT="${PORT:-9001}"
WORKERS="${WORKERS:-2}"
MESSAGES="${MESSAGES:-500000}"
DURATION_MS="${DURATION_MS:-6000}"
BIN="${BIN:-./tick-relay}"

if [[ ! -x "$BIN" ]]; then
    echo "binary not found at $BIN. run 'make' or 'make release' first." >&2
    exit 1
fi

echo "== starting tick-relay on :$PORT with $WORKERS workers =="
"$BIN" --port "$PORT" --workers "$WORKERS" --duration-ms "$DURATION_MS" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

# Wait for the port to open.
for _ in $(seq 1 50); do
    if command -v nc >/dev/null 2>&1; then
        if nc -z 127.0.0.1 "$PORT" 2>/dev/null; then break; fi
    else
        if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
            exec 3<&-; exec 3>&-
            break
        fi
    fi
    sleep 0.1
done

echo "== replaying $MESSAGES frames =="
python3 tools/replay.py --host 127.0.0.1 --port "$PORT" --messages "$MESSAGES"

echo "== waiting for server to drain and print summary =="
wait "$SERVER_PID" || true
