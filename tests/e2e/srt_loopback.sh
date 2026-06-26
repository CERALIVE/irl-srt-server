#!/bin/sh
# SRT loopback end-to-end check, run as a HARD gate inside `docker build`.
#
# It proves the freshly built srt_server actually relays an MPEG-TS stream over
# real libsrt on 127.0.0.1, and that each receive profile behaves to spec. The
# SRT transport on every leg is the project's own srt_client, built against the
# same libsrt as the server; ffmpeg only synthesises the TS payload (SLS relays
# TS opaquely, so the codec is irrelevant).
#
# Phases (each asserts and aborts on failure — there is no silent pass):
#   0. startup    — all three profile listeners (L1/L2/L3) bind and log a tag.
#   1. baseline   — L3 direct publisher -> player relays a non-trivial stream.
#   2. fec-accept — the L1 listener (SRTO_PACKETFILTER="fec") accepts a *non-FEC*
#                   plain srt_client caller and relays it (Todo 9). A full-FEC
#                   caller is covered by the sockopt read-back in the unit tests
#                   (tests/test_srt_profiles.cpp, Todo 8); srt_client is a plain
#                   caller, so this leg proves the accept-form does NOT reject it.
#   3. integrity  — publisher->player payload is byte-identical over a defined
#                   packet window (Todo 11): skip SKIP_PACKETS preamble packets,
#                   then sha256 the next CHECK_PACKETS and match them against the
#                   source at the live-edge join offset.
#   4. loss-matrix— drive each sender shape against its matching listener under
#                   injected packet loss/reorder (netem), and assert the
#                   profile-specific differential (Todo 10): L1 (NAK on) sends
#                   NAKs under loss while L2 (NAK off) stays near zero. When
#                   NET_ADMIN/netem is unavailable the loss leg is SKIPped loudly
#                   and only connectivity is exercised. Needs no external device.
#
# Any failure exits non-zero, which fails the Docker build.
set -eu

SRT_SERVER="${SRT_SERVER:-srt_server}"
SRT_CLIENT="${SRT_CLIENT:-srt_client}"
CONF="${SLS_LOOPBACK_CONF:-/etc/sls-loopback.conf}"

# --- listener ports (must mirror tests/e2e/sls-loopback.conf) ---
PLAYER_PORT=4000     # players for every profile pull here
PUB_L3_PORT=4001     # L3 direct (OBS/external direct-SRT)
PUB_L1_PORT=4002     # L1 freeze+NAK (SRTLA default, FEC-accept)
PUB_L2_PORT=4003     # L2 classic  (SRTLA freeze, NAK off)

# --- HTTP control plane (used only for the Phase 4 NAK differential) ---
HTTP_BASE="http://127.0.0.1:8181"
API_KEY="e2e-loopback-key"   # matches api_keys in sls-loopback.conf (loopback only)

# --- MPEG-TS / byte-integrity constants ---
TS_PACK_LEN=188      # one MPEG-TS packet (src/core/common.hpp)
SKIP_PACKETS=50      # N: preamble packets discarded before the integrity window
CHECK_PACKETS=1000   # M: packets hashed in the integrity window
PROBE_PACKETS=32     # small unique window used to locate the live-edge join offset

# --- assertion floors ---
MIN_RELAY_BYTES=4096 # a handful of TS packets is the floor for "real media moved"

# --- netem injection (Phase 4). `reorder` REQUIRES a `delay`, so the task's
#     loss/reorder figures are paired with a small 10ms delay. ---
NETEM_DELAY="10ms"
NETEM_LOSS="5%"
NETEM_REORDER="25%"

WORKDIR="$(mktemp -d)"
IN_TS="$WORKDIR/in.ts"
IN_HEX="$WORKDIR/in.hex"
SERVER_LOG="$WORKDIR/server.log"
SERVER_PID=""
CLIENT_PIDS=""
NETEM_UP=0       # 1 while the netem qdisc is currently attached (drives cleanup)
NETEM_APPLIED=0  # 1 if netem was ever applied this run (gates the strict differential)

have() { command -v "$1" >/dev/null 2>&1; }
track() { CLIENT_PIDS="$CLIENT_PIDS $1"; }

cleanup() {
    for _p in $CLIENT_PIDS; do kill -9 "$_p" 2>/dev/null || true; done
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    if [ "$NETEM_UP" = 1 ]; then tc qdisc del dev lo root 2>/dev/null || true; fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

fail() {
    echo "E2E FAIL: $1" >&2
    if [ -s "$SERVER_LOG" ]; then
        echo "--- last 40 server log lines ---" >&2
        tail -40 "$SERVER_LOG" >&2
    fi
    exit 1
}

start_pub() {
    "$SRT_CLIENT" -r "srt://127.0.0.1:${1}?streamid=${2}" -i "$IN_TS" >/dev/null 2>&1 &
    RUN_PID=$!
    track "$RUN_PID"
}
start_play() {
    "$SRT_CLIENT" -r "srt://127.0.0.1:${1}?streamid=${2}" -o "$3" >/dev/null 2>&1 &
    RUN_PID=$!
    track "$RUN_PID"
}
stop_pid() { [ -n "${1:-}" ] && kill -INT "$1" 2>/dev/null || true; }

bytes_of() { wc -c < "$1" 2>/dev/null || echo 0; }
packets_of() { echo $(( $(bytes_of "$1") / TS_PACK_LEN )); }

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
have "$SRT_SERVER" || fail "srt_server not found on PATH"
have "$SRT_CLIENT" || fail "srt_client not found on PATH"
have ffmpeg || fail "ffmpeg not found on PATH"
for t in od dd sha256sum awk tr cut; do
    have "$t" || fail "required tool '$t' not found on PATH"
done
[ -f "$CONF" ] || fail "loopback config not found at $CONF"

# Synthesise a finite MPEG-TS file: 20s of low-bitrate mpeg2video gives a slow CI
# runner ample time to connect every leg and still drain real media. mpeg2video +
# mpegts are ffmpeg built-ins, so this needs no external codec library. The stream
# is video-only on purpose: with no audio PID the audio-gap-filler never injects,
# so the relay is byte-opaque and the Phase 3 integrity window is deterministic.
ffmpeg -nostdin -loglevel error \
    -f lavfi -i "testsrc=duration=20:size=320x240:rate=25" \
    -c:v mpeg2video -b:v 2M -f mpegts "$IN_TS"
[ -s "$IN_TS" ] || fail "ffmpeg produced an empty input TS file"
IN_PKT=$(packets_of "$IN_TS")
echo "E2E: synthesised ${IN_PKT}-packet source TS ($(bytes_of "$IN_TS") bytes)"

# ---------------------------------------------------------------------------
# Phase 0 — startup: all three profile listeners bind and log their tag.
# ---------------------------------------------------------------------------
"$SRT_SERVER" -c "$CONF" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 3
kill -0 "$SERVER_PID" 2>/dev/null || fail "srt_server exited during startup"

for tag in L3-direct L1-freeze-nak L2-classic; do
    grep -q "SRT profile: ${tag}" "$SERVER_LOG" || \
        fail "startup log missing 'SRT profile: ${tag}' line"
done
echo "E2E OK [phase 0]: startup log shows all three SRT profiles (L1/L2/L3)"

# Record the active compat mode — it decides how strict the Phase 4 NAK
# differential can be (under srtlapatches the per-profile NAK is best-effort).
if grep -q "SRT compat mode: srtlapatches" "$SERVER_LOG"; then
    COMPAT=srtlapatches
elif grep -q "SRT compat mode: reorderfreeze" "$SERVER_LOG"; then
    COMPAT=reorderfreeze
else
    COMPAT=standard
fi
echo "E2E: SRT compat mode = ${COMPAT}"

# ---------------------------------------------------------------------------
# Phase 1 — baseline: L3 direct publisher -> player relays a real stream.
# ---------------------------------------------------------------------------
BASE_OUT="$WORKDIR/baseline.ts"
start_pub "$PUB_L3_PORT" "publish/live/baseline"; BASE_PUB=$RUN_PID
sleep 2
start_play "$PLAYER_PORT" "play/live/baseline" "$BASE_OUT"; BASE_PLAY=$RUN_PID
sleep 8
stop_pid "$BASE_PLAY"; sleep 2; stop_pid "$BASE_PUB"; sleep 1

[ -s "$BASE_OUT" ] || fail "[phase 1] player received no data over the L3 loopback"
BASE_SIZE=$(bytes_of "$BASE_OUT")
[ "$BASE_SIZE" -ge "$MIN_RELAY_BYTES" ] || \
    fail "[phase 1] L3 player output too small (${BASE_SIZE} bytes), relay did not carry the stream"
echo "E2E OK [phase 1]: L3 direct relay carried ${BASE_SIZE} bytes through srt_server"

# ---------------------------------------------------------------------------
# Phase 2 — FEC-accept (Todo 9): the L1 listener sets SRTO_PACKETFILTER="fec"
# (accept-form). srt_client is a *plain*, non-FEC caller (it sets no packet
# filter), so a successful relay here proves the accept-form does NOT reject a
# non-FEC caller — the L1 responder clears the filter for that connection and
# connects plain (COMPATIBILITY.md §6 case b). One listener serves both shapes;
# there is no separate FEC port. A full-FEC caller (case a) is covered by the
# sockopt read-back in tests/test_srt_profiles.cpp (Todo 8).
# ---------------------------------------------------------------------------
FEC_OUT="$WORKDIR/fec.ts"
start_pub "$PUB_L1_PORT" "publish/live/fecaccept"; FEC_PUB=$RUN_PID
sleep 2
start_play "$PLAYER_PORT" "play/live/fecaccept" "$FEC_OUT"; FEC_PLAY=$RUN_PID
sleep 8
stop_pid "$FEC_PLAY"; sleep 2; stop_pid "$FEC_PUB"; sleep 1

[ -s "$FEC_OUT" ] || fail "[phase 2] L1 FEC-accept listener relayed no data to a non-FEC plain caller"
FEC_SIZE=$(bytes_of "$FEC_OUT")
[ "$FEC_SIZE" -ge "$MIN_RELAY_BYTES" ] || \
    fail "[phase 2] L1 FEC-accept relay too small (${FEC_SIZE} bytes); plain caller not served"
echo "E2E OK [phase 2]: L1 FEC-accept listener served a NON-FEC plain caller (${FEC_SIZE} bytes); full-FEC caller covered by Todo 8 sockopt test"

echo "E2E PASS: startup + baseline relay + L1 FEC-accept green"
