#!/bin/sh
# SRT loopback end-to-end check, run as a HARD gate inside `docker build`.
#
# It proves the freshly built srt_server actually relays an MPEG-TS stream over
# real libsrt on 127.0.0.1: a publisher pushes a generated TS file in and a
# player pulls it back out, and we assert the player received a non-trivial
# amount of data. ffmpeg is used only to synthesise the TS payload (SLS relays
# TS opaquely, so the codec is irrelevant); the SRT transport on both legs is
# the project's own srt_client built against the same libsrt as the server.
#
# Any failure exits non-zero, which fails the Docker build. There is no skip
# path: missing binaries, a bind failure, or zero bytes delivered all abort.
set -eu

SRT_SERVER="${SRT_SERVER:-srt_server}"
SRT_CLIENT="${SRT_CLIENT:-srt_client}"
CONF="${SLS_LOOPBACK_CONF:-/etc/sls-loopback.conf}"
PLAYER_PORT=4000
PUBLISHER_PORT=4001
STREAM="live/e2e"

WORKDIR="$(mktemp -d)"
IN_TS="$WORKDIR/in.ts"
OUT_TS="$WORKDIR/out.ts"
SERVER_LOG="$WORKDIR/server.log"
SERVER_PID=""
PUB_PID=""
PLAY_PID=""

cleanup() {
    [ -n "$PLAY_PID" ] && kill "$PLAY_PID" 2>/dev/null || true
    [ -n "$PUB_PID" ] && kill "$PUB_PID" 2>/dev/null || true
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

fail() {
    echo "E2E FAIL: $1" >&2
    exit 1
}

command -v "$SRT_SERVER" >/dev/null 2>&1 || fail "srt_server not found on PATH"
command -v "$SRT_CLIENT" >/dev/null 2>&1 || fail "srt_client not found on PATH"
command -v ffmpeg >/dev/null 2>&1 || fail "ffmpeg not found on PATH"
[ -f "$CONF" ] || fail "loopback config not found at $CONF"

# Synthesise a finite MPEG-TS file. 20s of low-bitrate mpeg2video gives a slow CI
# runner ample time to connect both legs and still drain real media. mpeg2video +
# mpegts are ffmpeg built-ins, so this needs no external codec library.
ffmpeg -nostdin -loglevel error \
    -f lavfi -i "testsrc=duration=20:size=320x240:rate=25" \
    -c:v mpeg2video -b:v 2M -f mpegts "$IN_TS"
[ -s "$IN_TS" ] || fail "ffmpeg produced an empty input TS file"

"$SRT_SERVER" -c "$CONF" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
# Let the listeners bind before any client connects.
sleep 3
kill -0 "$SERVER_PID" 2>/dev/null || fail "srt_server exited during startup"

# Each listener logs exactly one "SRT profile:" line at startup. The loopback
# config declares all three profiles (L3 direct, L1 SRTLA, L2 Classic), so all
# three distinct profile tags must appear — proof the static profile->listener
# table routed each port to its profile.
for tag in L3-direct L1-freeze-nak L2-classic; do
    grep -q "SRT profile: ${tag}" "$SERVER_LOG" || \
        fail "startup log missing 'SRT profile: ${tag}' line"
done
echo "E2E OK: startup log shows all three SRT profiles (L1/L2/L3)"

# Publisher pushes the TS file in over SRT (paced by the file's PTS, ~20s).
"$SRT_CLIENT" -r "srt://127.0.0.1:${PUBLISHER_PORT}?streamid=publish/${STREAM}" -i "$IN_TS" &
PUB_PID=$!
# Give the publisher a moment to register the stream with the server.
sleep 2

# Player pulls the live stream back out into a file.
"$SRT_CLIENT" -r "srt://127.0.0.1:${PLAYER_PORT}?streamid=play/${STREAM}" -o "$OUT_TS" &
PLAY_PID=$!

# Let media flow through the relay.
sleep 10

# Stop the player first so it flushes its output, then the publisher.
kill -INT "$PLAY_PID" 2>/dev/null || true
PLAY_PID=""
sleep 2
kill -INT "$PUB_PID" 2>/dev/null || true
PUB_PID=""

[ -s "$OUT_TS" ] || fail "player received no data over the SRT loopback"
SIZE="$(wc -c < "$OUT_TS")"
# A handful of 188-byte TS packets is the floor for "real media moved"; require
# well above that so a stray single packet cannot pass the gate.
[ "$SIZE" -ge 4096 ] || fail "player output too small (${SIZE} bytes), relay did not carry the stream"

echo "E2E PASS: SRT loopback relayed ${SIZE} bytes through srt_server on 127.0.0.1"
