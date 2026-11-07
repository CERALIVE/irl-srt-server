# Shared helpers for the player-admission, reload and logging e2e scripts.
# Source after setting E2E_NAME. Binaries come from $SRT_SERVER, $SRT_CLIENT
# and $SLS_E2E_READER, or ./build/bin.
set -euo pipefail

E2E_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$E2E_DIR/../.." && pwd)"

find_bin() {
    local name="$1" override="$2"
    if [[ -n "$override" ]]; then printf '%s\n' "$override"; return 0; fi
    if [[ -x "$REPO_ROOT/build/bin/$name" ]]; then printf '%s\n' "$REPO_ROOT/build/bin/$name"; return 0; fi
    command -v "$name" 2>/dev/null && return 0
    return 1
}

SRT_SERVER="$(find_bin srt_server "${SRT_SERVER:-}")" || { echo "FAIL: srt_server not found" >&2; exit 1; }
SRT_CLIENT="$(find_bin srt_client "${SRT_CLIENT:-}")" || { echo "FAIL: srt_client not found" >&2; exit 1; }
SLS_E2E_READER="$(find_bin sls_e2e_reader "${SLS_E2E_READER:-}")" || { echo "FAIL: sls_e2e_reader not found" >&2; exit 1; }
for tool in curl jq python3 ffmpeg ffprobe; do
    command -v "$tool" >/dev/null 2>&1 || { echo "FAIL: $tool not found" >&2; exit 1; }
done

WORKDIR="$(mktemp -d)"
SERVER_LOG="$WORKDIR/server.log"
HOOK_LOG="$WORKDIR/hooks.jsonl"
SERVER_PID=""
HOOK_PID=""
CHILD_PIDS=()
LOAD_PIDS=()
SANITIZER_PATTERN='WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:'

e2e_cleanup() {
    local pid
    [[ -n "${LOAD_FLAG:-}" ]] && rm -f "$LOAD_FLAG"
    for pid in "${CHILD_PIDS[@]}" "${LOAD_PIDS[@]}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
    [[ -n "$HOOK_PID" ]] && kill "$HOOK_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    if [[ -n "${E2E_KEEP:-}" ]]; then
        rm -rf "$E2E_KEEP" && cp -r "$WORKDIR" "$E2E_KEEP"
    fi
    rm -rf "$WORKDIR"
}
trap e2e_cleanup EXIT INT TERM

fail() {
    echo "${E2E_NAME} FAIL: $1" >&2
    [[ -f "$SERVER_LOG" ]] && { echo "--- server log (tail) ---" >&2; tail -80 "$SERVER_LOG" >&2; }
    [[ -f "$HOOK_LOG" ]] && { echo "--- hooks (tail) ---" >&2; tail -30 "$HOOK_LOG" >&2; }
    exit 1
}

pass() { echo "PASS: $1"; }

# Stops every client this script started (publishers, readers).
stop_children() {
    local pid
    for pid in "${CHILD_PIDS[@]}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    for pid in "${CHILD_PIDS[@]}"; do
        [[ -n "$pid" ]] && wait "$pid" 2>/dev/null || true
    done
    CHILD_PIDS=()
}

# wait_until SECONDS CMD... : run CMD every 0.1 s until it succeeds.
wait_until() {
    local seconds="$1"; shift
    local i=0
    while (( i < seconds * 10 )); do
        "$@" && return 0
        if [[ -n "$SERVER_PID" ]] && ! kill -0 "$SERVER_PID" 2>/dev/null; then
            fail "srt_server exited while waiting for: $*"
        fi
        ((i += 1))
        sleep 0.1
    done
    return 1
}

# port_free udp|tcp PORT: true when nothing on this host holds PORT. The UDP
# probe binds without SO_REUSEADDR, so a libsrt listener (which sets it) still
# counts as holding the port; the TCP probe sets it, so a socket left in
# TIME_WAIT does not.
port_free() {
    python3 -I -c 'import socket, sys
udp = sys.argv[1] == "udp"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM if udp else socket.SOCK_STREAM)
if not udp:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind(("127.0.0.1", int(sys.argv[2])))
except OSError:
    sys.exit(1)' "$1" "$2"
}
port_taken() { ! port_free "$@"; }

# The SRT listener ports a config declares.
conf_udp_ports() { sed -nE 's/^[[:space:]]*listen_(player|publisher|publisher_srtla)[[:space:]]+([0-9]+);.*/\2/p' "$1"; }

# Fails unless every port srt_server will take is free: a leftover server or a
# port shared with another test would otherwise make the run test the wrong
# process.
assert_ports_free() {
    local conf="$1" port
    for port in $(conf_udp_ports "$conf"); do
        port_free udp "$port" || fail "UDP port $port of $(basename "$conf") is in use before srt_server starts"
    done
    port_free tcp "${HTTP##*:}" || fail "control-plane port ${HTTP##*:} is in use before srt_server starts"
}

# Fails unless srt_server holds every SRT listener port of its config.
wait_listening() {
    local conf="$1" port
    for port in $(conf_udp_ports "$conf"); do
        wait_until 5 port_taken udp "$port" || fail "srt_server is not listening on UDP port $port"
    done
}

# Answers every on_connect the hook server holds (hook_server.py --hold).
release_hooks() { curl -fsS -X POST "http://127.0.0.1:$1/release" -o /dev/null || fail "could not release the held hooks"; }

start_hooks() {
    local port="$1"; shift
    port_free tcp "$port" || fail "hook port $port is in use"
    # Append mode, so a script can truncate the log between phases.
    python3 -I "$E2E_DIR/hook_server.py" "$port" "$@" >>"$HOOK_LOG" 2>"$WORKDIR/hooks.err" &
    HOOK_PID=$!
    wait_until 5 curl -fsS "http://127.0.0.1:${port}/" -o /dev/null || fail "hook server did not start"
}

start_sls() {
    local conf="$1"
    assert_ports_free "$conf"
    "$SRT_SERVER" -c "$conf" >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    wait_until 10 curl -fsS "$HTTP/healthz" -o /dev/null || fail "srt_server control plane did not come up"
    wait_listening "$conf"
}

# Stops srt_server with SIGTERM and requires a clean exit and a clean log.
process_gone() { ! kill -0 "$1" 2>/dev/null; }

stop_sls() {
    [[ -n "$SERVER_PID" ]] || return 0
    local pid="$SERVER_PID" status=0
    SERVER_PID=""
    kill "$pid" 2>/dev/null || true
    wait_until 30 process_gone "$pid" || { kill -9 "$pid" 2>/dev/null; fail "srt_server did not exit within 30 s of SIGTERM"; }
    wait "$pid" 2>/dev/null || status=$?
    (( status == 0 )) || fail "srt_server exited with status $status"
    if grep -Eq "$SANITIZER_PATTERN" "$SERVER_LOG"; then
        fail "sanitizer finding in the server log"
    fi
}

# make_ts FILE SECONDS: intra-only MPEG-2 + AAC, so any byte range of it decodes.
make_ts() {
    ffmpeg -nostdin -loglevel error \
        -f lavfi -i "testsrc=duration=$2:size=320x180:rate=25" \
        -f lavfi -i "sine=frequency=1000:duration=$2" \
        -c:v mpeg2video -g 1 -b:v 1M -c:a aac -ar 48000 -ac 2 \
        -f mpegts "$1"
    [[ -s "$1" ]] || fail "ffmpeg produced no TS"
}

start_publisher() {
    "$SRT_CLIENT" -r "srt://127.0.0.1:${PUB_PORT}?streamid=$1" -i "$2" >"$WORKDIR/publisher-$3.log" 2>&1 &
    CHILD_PIDS+=("$!")
    PUBLISHER_PID=$!
}

stats() { curl -fsS -H "Authorization: $API_KEY" "$HTTP/stats"; }

publisher_live() {
    stats 2>/dev/null | jq -e --arg k "$1" '.publishers[$k].bitrate > 0' >/dev/null 2>&1
}

# start_reader NAME [reader args...]; events go to $WORKDIR/NAME.events.
# stdin is passed through explicitly (<&0): bash would otherwise give a
# background job /dev/null, and --wait-stdin needs the caller's stdin.
start_reader() {
    local name="$1"; shift
    "$SLS_E2E_READER" --port "$PLAY_PORT" --out "$WORKDIR/$name" "$@" <&0 >"$WORKDIR/$name.events" \
        2>"$WORKDIR/$name.err" &
    CHILD_PIDS+=("$!")
    LAST_READER_PID=$!
}

reader_total() {
    jq -rs --argjson s "$2" '[.[] | select(.sock == $s and (.event == "bytes" or .event == "closed")) | .total]
        | max // 0' "$WORKDIR/$1.events" 2>/dev/null || echo 0
}
reader_at_least() { (( $(reader_total "$1" "$2") >= $3 )); }
reader_closed() { jq -es --argjson s "$2" 'any(.[]; .sock == $s and .event == "closed")' "$WORKDIR/$1.events" >/dev/null 2>&1; }
reader_connected() { jq -es --argjson s "$2" 'any(.[]; .sock == $s and .event == "connected")' "$WORKDIR/$1.events" >/dev/null 2>&1; }

# hooks JQ_FILTER: the hook records matching a jq select() filter.
hooks() { jq -c "select($1)" "$HOOK_LOG" 2>/dev/null || true; }
hook_count() { hooks "$1" | wc -l; }
hook_count_at_least() { (( $(hook_count "$1") >= $2 )); }

# api METHOD PATH [curl args...]: prints "<status> <body>".
api() {
    local method="$1" path="$2"; shift 2
    local body status
    body="$(curl -sS -o - -w '\n%{http_code}' -X "$method" "$@" "$HTTP$path")"
    status="${body##*$'\n'}"
    body="${body%$'\n'*}"
    printf '%s %s\n' "$status" "$body"
}

expect_status() {
    local want="$1" label="$2"; shift 2
    local got
    got="$(api "$@")"
    [[ "${got%% *}" == "$want" ]] || fail "$label: expected HTTP $want, got: $got"
}

# decodable_frames FILE OFFSET: video frames ffprobe decodes from byte OFFSET on.
decodable_frames() {
    tail -c +"$(( $2 + 1 ))" "$1" >"$WORKDIR/tail.ts"
    ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 \
        "$WORKDIR/tail.ts" 2>/dev/null | head -1 | tr -dc '0-9'
}
