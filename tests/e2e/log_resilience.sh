#!/usr/bin/env bash
# A log sink can never stop media.
#   A. stdout is a pipe nobody reads: a handshake flood fills it while a
#      publisher and a reader stream. Media keeps flowing, the control plane
#      answers, /stats reports the stall and counts dropped lines; once the
#      pipe is drained again the stall clears and is reported.
#   B. stdout is a pipe whose reader has exited: with SIGPIPE ignored, SLS
#      survives the broken pipe and keeps streaming under the same flood.
#   C. SIGTERM while stdout is still blocked: SLS exits promptly and cleanly
#      instead of hanging on a final flush.
#   D. SIGHUP while stdout is blocked, with a log_file configured: the reload
#      adds the file sink without waiting on the blocked stdout write, so the
#      new listeners come back and media keeps flowing.
#   E. SIGTERM moments after stdout blocks, before /stats would report the
#      stall: exit drains the log queue for at most its deadline, counts what
#      it drops, and exits.
# Every wait is bounded: a hang fails the phase instead of the CI job.
E2E_NAME=LOG-RESILIENCE
source "$(dirname "$0")/lib_e2e.sh"

CONF="$E2E_DIR/sls-log-resilience.conf"
HTTP="http://127.0.0.1:8288"
API_KEY="log-resilience-test-key"
PUB_PORT=5721
PLAY_PORT=5720
STREAM_KEY="publish/live/resilience"
# A small queue so the flood overruns it quickly.
export SLS_LOG_QUEUE_SIZE=256

make_ts "$WORKDIR/in.ts" 120

# start_sls_on STDOUT STDERR: like start_sls, with stdout and stderr split.
start_sls_on() {
    SERVER_LOG="$2"
    assert_ports_free "$CONF"
    "$SRT_SERVER" -c "$CONF" >"$1" 2>"$2" &
    SERVER_PID=$!
    wait_until 10 curl -fsS "$HTTP/healthz" -o /dev/null || fail "srt_server control plane did not come up"
    wait_listening "$CONF"
}

logging() { stats | jq -c '.logging'; }
logging_field() { stats | jq -e "$1" >/dev/null 2>&1; }

stream_and_flood() {
    local tag="$1"
    cp "$WORKDIR/in.ts" "$WORKDIR/in-$tag.ts"
    start_publisher "$STREAM_KEY" "$WORKDIR/in-$tag.ts" "$tag"
    wait_until 15 publisher_live "$STREAM_KEY" || fail "[$tag] publisher never went live"
    start_reader "reader-$tag" --streamid play/live/resilience --duration-ms 120000
    wait_until 10 reader_at_least "reader-$tag" 0 65536 || fail "[$tag] reader received no media"
    # Junk handshakes: each one is accepted, logs several debug lines and a
    # refusal, then is closed.
    "$SLS_E2E_READER" --port "$PLAY_PORT" --streamid "junk-$tag" --flood 400 --sockets 4 \
        >"$WORKDIR/flood-$tag.events" 2>&1 &
    FLOOD_PID=$!
    CHILD_PIDS+=("$FLOOD_PID")
    local mark
    mark="$(reader_total "reader-$tag" 0)"
    wait_until 20 reader_at_least "reader-$tag" 0 $((mark + 262144)) ||
        fail "[$tag] media stalled while the log sink was blocked or broken"
    # Bounded: libsrt's own error lines once went to stderr, flushing the
    # blocked stdout from inside srt_accept, and the listener froze for good.
    wait_until 60 process_gone "$FLOOD_PID" || fail "[$tag] the junk flood never finished: SLS stopped taking handshakes"
    wait "$FLOOD_PID" || true
    mark="$(reader_total "reader-$tag" 0)"
    wait_until 20 reader_at_least "reader-$tag" 0 $((mark + 262144)) || fail "[$tag] media stalled after the flood"
    reader_closed "reader-$tag" 0 && fail "[$tag] the reader was disconnected"
    kill -0 "$SERVER_PID" 2>/dev/null || fail "[$tag] srt_server died"
    attempts="$(jq -r 'select(.event == "flood") | .attempts' "$WORKDIR/flood-$tag.events")"
    (( attempts >= 1000 )) || fail "[$tag] the flood made only $attempts handshakes"
}

# --- A. stdout blocked ---
mkfifo "$WORKDIR/blocked.fifo"
exec 8<>"$WORKDIR/blocked.fifo" # holds the pipe open; nothing reads it
start_sls_on "$WORKDIR/blocked.fifo" "$WORKDIR/stderr-a.log"
stream_and_flood a
wait_until 10 logging_field '.logging.sinkStalled == true' || fail "[a] /stats did not report the stalled sink: $(logging)"
logging_field '.logging.droppedMessages > 0 and .logging.sinkStallEvents >= 1' ||
    fail "[a] no dropped lines counted: $(logging)"
logging_field '.logging.securityEvents.admissionRefusals >= 1000' ||
    fail "[a] refusals were not all counted while their lines were dropped: $(logging)"
pass "[a] media flowed and the control plane answered with stdout blocked: $(logging)"

# 8>&-: the drain must not inherit our write end, or it never sees EOF.
cat "$WORKDIR/blocked.fifo" 8>&- >"$WORKDIR/drained-a.log" &
DRAIN_PID=$!
# Cleanup kills it if a step below fails; its bare wait would otherwise block
# on a drain that never sees end-of-file while we still hold fd 8.
LOAD_PIDS+=("$DRAIN_PID")
wait_until 10 logging_field '.logging.sinkStalled == false' || fail "[a] the stall did not clear after draining"
wait_until 10 grep -q "log sink was blocked for" "$WORKDIR/drained-a.log" || fail "[a] the stall was not reported"
wait_until 15 grep -q "log queue overrun:" "$WORKDIR/drained-a.log" || fail "[a] the overrun summary was not logged"
pass "[a] draining stdout cleared the stall; the stall and the overrun were reported"
stop_children
stop_sls
# Drop our own write end too, so the drain sees end-of-file.
exec 8>&-
wait "$DRAIN_PID" 2>/dev/null || true

# --- B. stdout broken (reader gone) ---
mkfifo "$WORKDIR/broken.fifo"
head -c 2048 "$WORKDIR/broken.fifo" >/dev/null &
HEAD_PID=$!
start_sls_on "$WORKDIR/broken.fifo" "$WORKDIR/stderr-b.log"
wait "$HEAD_PID" || true
stream_and_flood b
expect_status 200 "/stats with stdout broken" GET "/stats" -H "Authorization: $API_KEY"
pass "[b] SLS survived a closed stdout pipe and kept streaming: $(logging)"
stop_children
stop_sls

# --- C. SIGTERM while stdout is blocked ---
mkfifo "$WORKDIR/exit.fifo"
exec 9<>"$WORKDIR/exit.fifo"
start_sls_on "$WORKDIR/exit.fifo" "$WORKDIR/stderr-c.log"
status=0
timeout 60 "$SLS_E2E_READER" --port "$PLAY_PORT" --streamid junk-c --flood 200 --sockets 4 >/dev/null 2>&1 || status=$?
(( status != 124 )) || fail "[c] the junk flood never finished: SLS stopped taking handshakes"
wait_until 10 logging_field '.logging.sinkStalled == true' || fail "[c] the sink never stalled: $(logging)"
pid="$SERVER_PID"
SERVER_PID=""
kill "$pid"
for _ in $(seq 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
kill -0 "$pid" 2>/dev/null && { kill -9 "$pid"; fail "[c] SLS hung on exit with stdout blocked"; }
status=0
wait "$pid" || status=$?
(( status == 0 )) || fail "[c] SLS exited with status $status"
exec 9>&-
pass "[c] SLS exited promptly with status 0 while stdout was blocked"

# --- D. SIGHUP while stdout is blocked ---
sed "/log_level debug;/a\\    log_file $WORKDIR/sls-file.log;" "$E2E_DIR/sls-log-resilience.conf" >"$WORKDIR/sls-log-file.conf"
grep -q "log_file $WORKDIR/sls-file.log;" "$WORKDIR/sls-log-file.conf" || fail "[d] could not add log_file to the config"
CONF="$WORKDIR/sls-log-file.conf"
mkfifo "$WORKDIR/reload.fifo"
exec 6<>"$WORKDIR/reload.fifo" # holds the pipe open; nothing reads it
start_sls_on "$WORKDIR/reload.fifo" "$WORKDIR/stderr-d.log"
stream_and_flood d
wait_until 10 logging_field '.logging.sinkStalled == true' || fail "[d] the sink never stalled: $(logging)"
kill -HUP "$SERVER_PID"
# The reloaded generation's listeners take a new publisher and a new reader.
cp "$WORKDIR/in.ts" "$WORKDIR/in-d2.ts"
start_publisher "publish/live/afterreload" "$WORKDIR/in-d2.ts" d2
wait_until 20 publisher_live "publish/live/afterreload" ||
    fail "[d] the listeners did not come back after a SIGHUP with stdout blocked"
start_reader reader-d2 --streamid play/live/afterreload --duration-ms 60000
wait_until 10 reader_at_least reader-d2 0 65536 || fail "[d] a reader of the reloaded generation received no media"
mark="$(reader_total reader-d 0)"
wait_until 10 reader_at_least reader-d 0 $((mark + 131072)) || fail "[d] the reader from before the reload stopped"
logging_field '.logging.sinkStalled == true' || fail "[d] stdout was not blocked through the reload: $(logging)"
pass "[d] with stdout blocked, SIGHUP brought the listeners back and media kept flowing"
cat "$WORKDIR/reload.fifo" 6>&- >/dev/null &
DRAIN_PID=$!
LOAD_PIDS+=("$DRAIN_PID")
wait_until 10 logging_field '.logging.sinkStalled == false' || fail "[d] the stall did not clear after draining"
stop_children
stop_sls
exec 6>&-
wait "$DRAIN_PID" 2>/dev/null || true

# --- E. SIGTERM moments after stdout blocks ---
# A 4 KiB pipe fills within a short flood, so the stall is fresh at SIGTERM:
# exit must not join a log thread blocked on it.
CONF="$E2E_DIR/sls-log-resilience.conf"
mkfifo "$WORKDIR/fresh.fifo"
exec 5<>"$WORKDIR/fresh.fifo" # holds the pipe open; nothing reads it
# 1031 is F_SETPIPE_SZ.
python3 -I -c 'import fcntl, sys; fcntl.fcntl(int(sys.argv[1]), 1031, 4096)' 5 || fail "[e] could not shrink the pipe"
start_sls_on "$WORKDIR/fresh.fifo" "$WORKDIR/stderr-e.log"
status=0
timeout 60 "$SLS_E2E_READER" --port "$PLAY_PORT" --streamid junk-e --flood 50 --sockets 2 >/dev/null 2>&1 || status=$?
(( status != 124 )) || fail "[e] the junk flood never finished: SLS stopped taking handshakes"
pid="$SERVER_PID"
SERVER_PID=""
kill "$pid"
wait_until 10 process_gone "$pid" || { kill -9 "$pid"; fail "[e] SLS hung on exit moments after stdout blocked"; }
status=0
wait "$pid" || status=$?
(( status == 0 )) || fail "[e] SLS exited with status $status"
exit_line="$(grep -m1 '\[log\] exit: a log sink was still blocked' "$WORKDIR/stderr-e.log")" ||
    fail "[e] exit did not report the lines it dropped"
exec 5>&-
pass "[e] SLS exited within the drain deadline with stdout just blocked: ${exit_line#*; }"

for log in "$WORKDIR"/stderr-*.log; do
    grep -Eq "$SANITIZER_PATTERN" "$log" && { cat "$log" >&2; fail "sanitizer finding in $(basename "$log")"; }
done
echo "LOG-RESILIENCE PASS"
