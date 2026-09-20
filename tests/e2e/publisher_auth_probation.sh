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
STATS_PROBE_PIDS=()

cleanup() {
    for pid in "${CALLER_PIDS[@]}" "${WRITER_PIDS[@]}" "${STATS_PROBE_PIDS[@]}"; do
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
    for log in "$WORKDIR"/*-caller.log; do
        [[ -f "$log" ]] && { echo "--- $(basename "$log") ---" >&2; tail -30 "$log" >&2; }
    done
    for log in "$WORKDIR"/*-stats.log; do
        [[ -f "$log" ]] && { echo "--- $(basename "$log") ---" >&2; tail -30 "$log" >&2; }
    done
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

wait_server_log() {
    local pattern="$1" seconds="$2" i=0
    while (( i < seconds * 10 )); do
        grep -q "$pattern" "$SERVER_LOG" 2>/dev/null && return 0
        ((i += 1))
        sleep 0.1
    done
    return 1
}

wait_process_gone() {
    local pid="$1" seconds="$2" i=0
    while (( i < seconds * 10 )); do
        kill -0 "$pid" 2>/dev/null || return 0
        ((i += 1))
        sleep 0.1
    done
    return 1
}

start_server() {
    local config="$1"
    rm -f "$PID_FILE"
    "$SRT_SERVER" -c "$config" >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    for _ in {1..50}; do
        curl -fsS "$HTTP/healthz" >/dev/null 2>&1 && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || fail "server exited during startup"
        sleep 0.1
    done
    fail "server did not start"
}

stop_server() {
    [[ -n "$SERVER_PID" ]] || return 0
    local pid="$SERVER_PID" status=0
    SERVER_PID=""
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || status=$?
    (( status == 0 )) || fail "server shutdown exited with status $status"
    if grep -Eq 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:' "$SERVER_LOG"; then
        fail "sanitizer finding detected in server log"
    fi
    echo "PASS: server shutdown completed cleanly with no sanitizer finding"
    rm -f "$PID_FILE"
}

stop_silent_callers() {
    for pid in "${CALLER_PIDS[@]}" "${WRITER_PIDS[@]}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    for pid in "${CALLER_PIDS[@]}" "${WRITER_PIDS[@]}"; do
        [[ -n "$pid" ]] && wait "$pid" 2>/dev/null || true
    done
    CALLER_PIDS=()
    WRITER_PIDS=()
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

start_stats_probe() {
    local name="$1" samples="$2"
    (
        local snapshot
        for ((i = 0; i < samples; ++i)); do
            snapshot="$(curl -fsS -H "Authorization: $API_KEY" "$HTTP/stats")"
            jq -e '(.publishers // {}) as $p | ($p | type == "object") and all($p[]; has("bitrate") and has("latency") and has("uptime"))' \
                <<<"$snapshot" >/dev/null
            sleep 0.02
        done
    ) >"$WORKDIR/$name-stats.log" 2>&1 &
    STATS_PROBE_PIDS+=("$!")
}

wait_stats_probes() {
    local pid
    for pid in "${STATS_PROBE_PIDS[@]}"; do
        wait "$pid" || fail "concurrent stats probe failed"
    done
    STATS_PROBE_PIDS=()
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
                time.sleep(2)
            if name == "trickle":
                body = b"0123456789"
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    for byte in body:
                        self.wfile.write(bytes([byte]))
                        self.wfile.flush()
                        time.sleep(1)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                print(f"response trickle-ended event={event} name={name}", flush=True)
                return
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

start_server "$CONF"
sleep 1
wait_count 0 2 || fail "publisher map was not initially empty"

start_stats_probe delay 200
start_silent_caller delay
wait_log "request event=on_connect name=delay" 5 || fail "delayed authorization webhook was not called"
sleep 1
[[ "$(publisher_count)" == "0" ]] || fail "publisher was visible before delayed authorization completed"
wait_log "response status=200 event=on_connect name=delay" 5 || fail "delayed authorization did not complete"
wait_count 1 2 || fail "authorized silent publisher was not published after authorization"
wait_count 0 5 || fail "authorized silent publisher was not reaped after probation"
wait_stats_probes
echo "PASS: delayed authorization remains unpublished, then starts a bounded first-data window"

start_silent_caller allow
wait_log "response status=200 event=on_connect name=allow" 5 || fail "successful authorization did not complete"
wait_count 1 2 || fail "authorized silent publisher was not visible during probation"
wait_count 0 5 || fail "authorized silent publisher escaped first-data probation"
echo "PASS: successful authorization starts first-data probation without media readiness"

start_silent_caller reject
wait_log "response status=403 event=on_connect name=reject" 5 || fail "rejection webhook did not complete"
wait_count 0 3 || fail "rejected silent publisher remained registered"
echo "PASS: rejected silent publisher is never published or handed off"

start_silent_caller trickle
wait_log "request event=on_connect name=trickle" 5 || fail "trickle authorization webhook was not called"
sleep 1
[[ "$(publisher_count)" == "0" ]] || fail "trickle publisher was visible before authorization completed"
wait_count 0 6 || fail "slow-progress authorization exceeded the admission deadline"
wait_server_log "publisher authorization deadline expired" 6 || fail "slow-progress authorization did not hit admission deadline"
echo "PASS: an independently enforced admission deadline rejects before publication"

stop_silent_callers
stop_server
OVERSIZE_CONF="$WORKDIR/oversize.conf"
python3 - "$CONF" "$OVERSIZE_CONF" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
source = source.replace(
    "http://127.0.0.1:8294/admission",
    "http://127.0.0.1:8294/" + "a" * 1980,
)
pathlib.Path(sys.argv[2]).write_text(source)
PY
SERVER_LOG="$WORKDIR/oversize-server.log"
start_server "$OVERSIZE_CONF"
sleep 1
start_silent_caller oversized
caller_index=$((${#CALLER_PIDS[@]} - 1))
oversized_pid="${CALLER_PIDS[$caller_index]}"
wait_server_log "on_event_url is too long" 5 || fail "oversized callback did not hit request-construction guard"
wait_process_gone "$oversized_pid" 5 || fail "request-construction failure left the SRT caller connected"
wait_count 0 2 || fail "request-construction failure registered a publisher"
echo "PASS: authorization request-construction failure is terminal before handoff"

stop_silent_callers
stop_server
echo "PUBLISHER-AUTH-PROBATION PASS: silent authenticated callers remain bounded"
