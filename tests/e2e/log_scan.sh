#!/usr/bin/env bash
# No credential reaches SLS's own output, at any log level.
#
# For each level from trace to error, SLS runs a scenario full of planted
# credentials: a publisher key, reader keys of every class, refused and junk
# handshakes, secret webhook path segments, the API key, legacy token= and
# URL-encoded forms, a player kick and a publisher disconnect. Everything SLS
# writes (stdout, stderr and log_file) is then searched for every planted
# value; one hit fails. The webhook's own record proves each value was really
# in play, and at the verbose levels the non-secret fields must survive.
E2E_NAME=LOG-SCAN
source "$(dirname "$0")/lib_e2e.sh"

HTTP="http://127.0.0.1:8289"
API_KEY="SCANapiKey0003"
PUB_PORT=5731
PLAY_PORT=5730
HOOK_PORT=8299
PUB_SID="publish/r/relay1?k=cl_live_SCANpub0001"
SECRETS=(SCANsecretPath01 SCANsecretPath02 SCANapiKey0003 SCANpub0001 SCANpub0002 SCANout0003 SCANbad0004
    SCANhob0005 SCANpvw0006 SCANraw0007xyz SCANjunk0008 SCANamp0009 SCANtok0010 SCANenc0011)

urlenc() { jq -rn --arg v "$1" '$v | @uri'; }

make_ts "$WORKDIR/in.ts" 60
start_hooks "$HOOK_PORT" --deny-substring relay2 \
    --player-key "cl_out_SCANout0003=$PUB_SID" --player-key "hob_SCANhob0005=$PUB_SID" \
    --player-key "pvw_SCANpvw0006=$PUB_SID" --player-key "SCANraw0007xyz=$PUB_SID"

junk() { "$SLS_E2E_READER" --port "$1" --streamid "$2" --flood 1 --sockets 1 >/dev/null 2>&1 || true; }
# held PORT STREAMID: a handshake that stays connected. Use it for every probe
# whose webhook call is asserted: a connect-and-close probe can be gone before
# SLS reads its peer address, and SLS then refuses it before any webhook call.
held() {
    "$SLS_E2E_READER" --port "$1" --streamid "$2" --duration-ms 5000 >/dev/null 2>&1 &
    CHILD_PIDS+=("$!")
}

for level in trace debug info warning error; do
    : >"$HOOK_LOG"
    SERVER_LOG="$WORKDIR/server-$level.log"
    FILE_LOG="$WORKDIR/file-$level.log"
    CONF="$WORKDIR/scan-$level.conf"
    sed -e "s|@LOG_LEVEL@|$level|" -e "s|@LOG_FILE@|$FILE_LOG|" "$E2E_DIR/sls-log-scan.conf.in" >"$CONF"
    start_sls "$CONF"

    cp "$WORKDIR/in.ts" "$WORKDIR/in-$level.ts"
    "$SRT_CLIENT" -r "srt://127.0.0.1:${PUB_PORT}?streamid=$(urlenc "$PUB_SID")" -i "$WORKDIR/in-$level.ts" \
        >"$WORKDIR/publisher-$level.log" 2>&1 &
    CHILD_PIDS+=("$!")
    wait_until 15 publisher_live "publish/r/relay1?k=cl_live_SCANpub0001" || fail "[$level] publisher not live"

    # Readers of every key class. The first attempt of an uncached key is held
    # while the key webhook runs, then admitted.
    for key in cl_out_SCANout0003 hob_SCANhob0005 pvw_SCANpvw0006 SCANraw0007xyz; do
        start_reader "r-$level-$key" --streamid "play/r/$key" --duration-ms 30000
    done
    for key in cl_out_SCANout0003 hob_SCANhob0005 pvw_SCANpvw0006 SCANraw0007xyz; do
        wait_until 15 reader_at_least "r-$level-$key" 0 16384 || fail "[$level] reader $key received no media"
    done

    # Refusals: an unknown reader key (twice: held, then cached negative), a
    # publisher the webhook refuses, and junk handshakes.
    held "$PLAY_PORT" "play/r/cl_out_SCANbad0004"
    wait_until 5 hook_count_at_least '.player_key == "cl_out_SCANbad0004"' 1 || fail "[$level] bad key never checked"
    junk "$PLAY_PORT" "play/r/cl_out_SCANbad0004"
    held "$PUB_PORT" "publish/r/relay2?k=cl_live_SCANpub0002"
    junk "$PLAY_PORT" "#!::r=live/x?k=SCANjunk0008"
    junk "$PUB_PORT" "publish/r/x&k=SCANamp0009"
    held "$PUB_PORT" "publish/r/x?token=SCANtok0010"
    junk "$PLAY_PORT" "publish%2Fr%2Fx%3Fk%3DSCANenc0011"
    wait_until 5 hook_count_at_least '.on_event == "on_connect" and (.srt_url | contains("SCANpub0002"))' 1 ||
        fail "[$level] refused publisher never reached the webhook"
    wait_until 5 hook_count_at_least '.on_event == "on_connect" and (.srt_url | contains("SCANtok0010"))' 1 ||
        fail "[$level] the token= publisher never reached the webhook"

    # Control-plane actions that log the stream key: a kick and a disconnect.
    session="$(hooks '.on_event == "on_connect" and .role_name == "player"' | sed -n 1p | jq -r .session_id)"
    expect_status 200 "[$level] kick" POST "/disconnect-player" -H "Authorization: $API_KEY" \
        --data-urlencode "stream=$PUB_SID" --data-urlencode "session=$session"
    expect_status 401 "[$level] wrong key" GET "/stats" -H "Authorization: SCANapiKey0003-wrong"
    expect_status 200 "[$level] disconnect publisher" POST "/disconnect" -H "Authorization: $API_KEY" \
        --data-urlencode "stream=$PUB_SID"
    wait_until 10 hook_count_at_least '.on_event == "on_close" and .role_name == "publisher"' 1 ||
        fail "[$level] the disconnected publisher never closed"
    stop_children
    stop_sls

    # Positive control: the planted values were really in play.
    for secret in SCANsecretPath01 SCANsecretPath02 SCANpub0001 SCANpub0002 SCANout0003 SCANbad0004 SCANhob0005 \
        SCANpvw0006 SCANraw0007xyz SCANtok0010; do
        grep -q "$secret" "$HOOK_LOG" || fail "[$level] $secret never reached the webhook; the scan proves nothing"
    done

    for secret in "${SECRETS[@]}"; do
        hits="$(cat "$SERVER_LOG" "$FILE_LOG" | grep -c -F "$secret" || true)"
        if (( hits > 0 )); then
            grep -F "$secret" "$SERVER_LOG" "$FILE_LOG" | head -5 >&2
            fail "[$level] $secret appears $hits time(s) in SLS output"
        fi
    done
    lines="$(cat "$SERVER_LOG" "$FILE_LOG" | wc -l)"
    if [[ "$level" == trace || "$level" == debug || "$level" == info ]]; then
        grep -q "relay1" "$SERVER_LOG" || fail "[$level] the relay id did not survive redaction"
        grep -q "127.0.0.1" "$SERVER_LOG" || fail "[$level] peer addresses did not survive redaction"
        grep -q "<redacted>" "$SERVER_LOG" || fail "[$level] nothing was redacted; the scenario logged no credentials"
    fi
    [[ -s "$FILE_LOG" ]] || [[ "$level" == error ]] || fail "[$level] log_file stayed empty"
    pass "[$level] 0 of ${#SECRETS[@]} planted credentials in $lines lines of SLS output"
done
echo "LOG-SCAN PASS"
