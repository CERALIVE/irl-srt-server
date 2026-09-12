#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CONF="$SCRIPT_DIR/sls-publisher-auth.conf"
HTTP="http://127.0.0.1:8284"
API_KEY="publisher-auth-test-key"
PUB_PORT=5601
WEBHOOK_PORT=8294
PID_FILE=/tmp/sls_publisher_auth_probation.pid

find_bin() {
    local name="$1" override="$2"
    if [[ -n "$override" ]]; then printf '%s\n' "$override"; return 0; fi
    if [[ -x "$REPO_ROOT/build/bin/$name" ]]; then printf '%s\n' "$REPO_ROOT/build/bin/$name"; return 0; fi
    command -v "$name" 2>/dev/null && return 0
    return 1
}

SRT_SERVER="$(find_bin srt_server "${SRT_SERVER:-}")" || { echo "FAIL: srt_server not found" >&2; exit 1; }
if [[ -n "${SRT_LIVE_TRANSMIT:-}" ]]; then
    SRT_TRANSMIT="$SRT_LIVE_TRANSMIT"
elif [[ -x /usr/local/srt-ceralive/bin/srt-live-transmit ]]; then
    SRT_TRANSMIT=/usr/local/srt-ceralive/bin/srt-live-transmit
else
    SRT_TRANSMIT="$(command -v srt-live-transmit 2>/dev/null || true)"
fi
[[ -n "$SRT_TRANSMIT" ]] || { echo "FAIL: srt-live-transmit not found" >&2; exit 1; }
for tool in curl jq python3 mkfifo tail; do
    command -v "$tool" >/dev/null 2>&1 || { echo "FAIL: $tool not found" >&2; exit 1; }
done

WORKDIR="$(mktemp -d)"
SERVER_LOG="$WORKDIR/server.log"
WEBHOOK_LOG="$WORKDIR/webhook.log"
SERVER_PID=""
WEBHOOK_PID=""
CALLER_PIDS=()
WRITER_PIDS=()

cleanup() {
    for pid in "${CALLER_PIDS[@]}" "${WRITER_PIDS[@]}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
    [[ -n "$WEBHOOK_PID" ]] && kill "$WEBHOOK_PID" 2>/dev/null || true
    rm -f "$PID_FILE"
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

fail() {
    echo "PUBLISHER-AUTH-PROBATION FAIL: $1" >&2
    [[ -f "$SERVER_LOG" ]] && { echo "--- server log ---" >&2; tail -60 "$SERVER_LOG" >&2; }
    [[ -f "$WEBHOOK_LOG" ]] && { echo "--- webhook log ---" >&2; tail -60 "$WEBHOOK_LOG" >&2; }
    exit 1
}

publisher_count() {
    curl -fsS -H "Authorization: $API_KEY" "$HTTP/stats" 2>/dev/null |
        jq -r '(.publishers // {}) | length' 2>/dev/null || printf '%s\n' ""
}

wait_count() {
    local wanted="$1" seconds="$2" i=0
    while (( i < seconds * 10 )); do
        [[ "$(publisher_count)" == "$wanted" ]] && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || fail "server exited while waiting for publisher count $wanted"
        ((i += 1))
        sleep 0.1
    done
    return 1
}

wait_log() {
    local pattern="$1" seconds="$2" i=0
    while (( i < seconds * 10 )); do
        grep -q "$pattern" "$WEBHOOK_LOG" 2>/dev/null && return 0
        ((i += 1))
        sleep 0.1
    done
    return 1
}

start_silent_caller() {
    local name="$1"
    local fifo="$WORKDIR/$name.fifo"
    mkfifo "$fifo"
    tail -f /dev/null >"$fifo" &
    WRITER_PIDS+=("$!")
    "$SRT_TRANSMIT" -q -a:no -to:20 file://con \
        "srt://127.0.0.1:${PUB_PORT}?streamid=publish/live/${name}&latency=200" \
        <"$fifo" >"$WORKDIR/$name-caller.log" 2>&1 &
    CALLER_PIDS+=("$!")
}

rm -f "$PID_FILE"
python3 -u - "$WEBHOOK_PORT" >"$WEBHOOK_LOG" 2>&1 <<'PY' &
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

port = int(sys.argv[1])

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        self.send_response(200)
        self.end_headers()

    def do_POST(self):
        query = parse_qs(urlparse(self.path).query)
        event = query.get("on_event", [""])[0]
        sid = query.get("srt_url", [""])[0]
        name = sid.rsplit("/", 1)[-1]
        print(f"request event={event} name={name}", flush=True)
        if event == "on_close":
            status = 200
        elif name == "reject":
            status = 403
        else:
            if name == "delay":
                time.sleep(4)
            status = 200
        body = json.dumps({"status": "ok"}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        print(f"response status={status} event={event} name={name}", flush=True)

ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
PY
WEBHOOK_PID=$!

for _ in {1..50}; do
    curl -fsS "http://127.0.0.1:${WEBHOOK_PORT}/health" >/dev/null 2>&1 && break
    kill -0 "$WEBHOOK_PID" 2>/dev/null || fail "webhook server exited during startup"
    sleep 0.1
done
curl -fsS "http://127.0.0.1:${WEBHOOK_PORT}/health" >/dev/null || fail "webhook server did not start"

"$SRT_SERVER" -c "$CONF" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
for _ in {1..50}; do
    curl -fsS "$HTTP/healthz" >/dev/null 2>&1 && break
    kill -0 "$SERVER_PID" 2>/dev/null || fail "server exited during startup"
    sleep 0.1
done
curl -fsS "$HTTP/healthz" >/dev/null || fail "server did not start"
wait_count 0 2 || fail "publisher map was not initially empty"

start_silent_caller delay
wait_log "request event=on_connect name=delay" 5 || fail "delayed authorization webhook was not called"
wait_count 1 3 || fail "delayed silent publisher was not admitted"
sleep 2
[[ "$(publisher_count)" == "1" ]] || fail "publisher probation ran while authorization was pending"
wait_log "response status=200 event=on_connect name=delay" 5 || fail "delayed authorization did not complete"
wait_count 0 5 || fail "authorized silent publisher was not reaped after probation"
echo "PASS: delayed authorization suspends probation, then starts a bounded first-data window"

start_silent_caller allow
wait_log "response status=200 event=on_connect name=allow" 5 || fail "successful authorization did not complete"
wait_count 1 2 || fail "authorized silent publisher was not visible during probation"
wait_count 0 5 || fail "authorized silent publisher escaped first-data probation"
echo "PASS: successful authorization starts first-data probation without media readiness"

start_silent_caller reject
wait_log "response status=403 event=on_connect name=reject" 5 || fail "rejection webhook did not complete"
wait_count 0 3 || fail "rejected silent publisher remained registered"
echo "PASS: rejected silent publisher is torn down without a media event"

echo "PUBLISHER-AUTH-PROBATION PASS: silent authenticated callers remain bounded"
