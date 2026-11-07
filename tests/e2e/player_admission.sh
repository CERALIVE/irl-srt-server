#!/usr/bin/env bash
# Player admission end to end, against a real srt_server and webhook:
#   1. /disconnect-player kicks exactly one session, even when two readers
#      share one peer ip:port; the other keeps playing decodable video and the
#      publisher stays live. The hooks carry session_id and boot_epoch.
#   2. The /disconnect-player input and auth matrix.
#   3. max_players_per_stream counts authorized players only: pending readers
#      cannot crowd out an authorized one, the limit still caps authorized
#      players, and the per-stream pending cap bounds the pending ones.
#   4. worker_reserved_connections: a reserved reader is dispatched while an
#      unreserved one waits for capacity.
#   5. Hosted and preview readers, verified internal by the player-key
#      resolver, are admitted while public pending readers fill the cap.
#   6. After a restart reuses a session id, a kick carrying the earlier
#      boot's epoch does not reach the new session.
#   7. A resolver-reserved key's internal grant is held by one peer at a
#      time, and one peer holds a bounded number of grants.
#   8. A reload keeps a verified internal reader's worker reservation, and a
#      public reader left behind full slots still gets its on_close.
E2E_NAME=PLAYER-ADMISSION
source "$(dirname "$0")/lib_e2e.sh"

CONF="$E2E_DIR/sls-player-admission.conf"
RESERVE_CONF="$E2E_DIR/sls-dispatch-reserve.conf"
INTERNAL_CONF="$E2E_DIR/sls-internal-pending.conf"
RELOAD_RESERVE_CONF="$E2E_DIR/sls-reload-reserve.conf"
HTTP="http://127.0.0.1:8286"
API_KEY="player-admission-test-key"
PUB_PORT=5701
PLAY_PORT=5700
HOOK_PORT=8296
STREAM_KEY="publish/live/kick"
AUTH=(-H "Authorization: $API_KEY")

make_ts "$WORKDIR/in.ts" 90
# Ports the test client binds to pick the webhook's verdict (hook_server.py).
start_hooks "$HOOK_PORT" --slow 47100-47199 --reserved 47200-47299 --slow-ms 3000
start_sls "$CONF"
start_publisher "publish/live/kick" "$WORKDIR/in.ts" main
wait_until 15 publisher_live "$STREAM_KEY" || fail "publisher never went live"

# --- 1. two readers on ONE local UDP port; kick the second by session id ---
mkfifo "$WORKDIR/pair.in"
exec 7<>"$WORKDIR/pair.in"
start_reader pair --streamid play/live/kick --sockets 2 --bind-port 47011 --wait-stdin --duration-ms 60000 \
    <"$WORKDIR/pair.in"
first_hook='.on_event == "on_connect" and .role_name == "player" and .remote_port == "47011"'
wait_until 10 hook_count_at_least "$first_hook" 1 || fail "first reader's on_connect hook never arrived"
echo next >&7
wait_until 10 hook_count_at_least "$first_hook" 2 || fail "second reader's on_connect hook never arrived"
s0="$(hooks "$first_hook" | sed -n 1p | jq -r .session_id)"
s1="$(hooks "$first_hook" | sed -n 2p | jq -r .session_id)"
[[ "$s0" =~ ^[0-9]+$ && "$s1" =~ ^[0-9]+$ && "$s0" != "$s1" ]] || fail "hooks lack distinct session ids: '$s0' '$s1'"
[[ "$(hooks "$first_hook" | jq -r .remote_ip | sort -u | wc -l)" == 1 ]] || fail "readers did not share one peer ip"
[[ -z "$(hooks '.role_name == "publisher" and has("session_id")')" ]] || fail "publisher hooks must not carry session_id"
epoch="$(hooks "$first_hook" | sed -n 1p | jq -r .boot_epoch)"
[[ "$epoch" =~ ^[0-9a-f]{16}$ ]] || fail "player hooks lack a boot_epoch: '$epoch'"
[[ "$(hooks "$first_hook" | jq -r .boot_epoch | sort -u)" == "$epoch" ]] || fail "one boot sent two boot epochs"
[[ "$(stats | jq -r .bootEpoch)" == "$epoch" ]] || fail "/stats bootEpoch is not the hooks' boot_epoch $epoch"
EPOCH=(--data-urlencode "boot_epoch=$epoch")
wait_until 10 reader_at_least pair 0 65536 || fail "first reader received no media"
wait_until 10 reader_at_least pair 1 65536 || fail "second reader received no media"
listed="$(stats | jq -c --arg k "$STREAM_KEY" '[.publishers[$k].players[].sessionId] | sort')"
[[ "$listed" == "$(jq -cn --argjson a "$s0" --argjson b "$s1" '[$a, $b] | sort')" ]] ||
    fail "/stats sessionIds $listed do not match the hooks ($s0, $s1)"
pass "two readers on one peer ip:port have distinct session ids in hooks and /stats ($s0, $s1)"

before_kick="$(reader_total pair 0)"
expect_status 200 "kick second reader" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=$STREAM_KEY" --data-urlencode "session=$s1" "${EPOCH[@]}"
wait_until 5 reader_closed pair 1 || fail "kicked reader (session $s1) was not disconnected"
reader_closed pair 0 && fail "the other reader (session $s0) was disconnected by a kick for $s1"
wait_until 10 reader_at_least pair 0 $((before_kick + 262144)) ||
    fail "the other reader stopped receiving after the kick"
reader_closed pair 0 && fail "the other reader closed after the kick"
wait_until 5 hook_count_at_least ".on_event == \"on_close\" and .session_id == \"$s1\"" 1 ||
    fail "no on_close hook with session_id=$s1"
[[ "$(hook_count ".on_event == \"on_close\" and .session_id == \"$s0\"")" == 0 ]] || fail "on_close for the survivor"
publisher_live "$STREAM_KEY" || fail "publisher went down after a player kick"
frames="$(decodable_frames "$WORKDIR/pair.0.ts" "$before_kick")"
(( ${frames:-0} >= 25 )) || fail "survivor's post-kick media decoded only ${frames:-0} frames"
pass "kick by session id closed only session $s1; session $s0 kept playing ($frames frames decoded after the kick)"

# --- 2. input and auth matrix (the survivor must stay connected throughout) ---
P="/disconnect-player"
expect_status 401 "no API key" POST "$P" --data-urlencode "stream=$STREAM_KEY" --data-urlencode "session=$s0" "${EPOCH[@]}"
expect_status 401 "wrong API key" POST "$P" -H "Authorization: wrong" --data-urlencode "stream=$STREAM_KEY" \
    --data-urlencode "session=$s0" "${EPOCH[@]}"
expect_status 404 "GET is not routed" GET "$P?stream=x&session=$s0" "${AUTH[@]}"
expect_status 400 "missing session" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" "${EPOCH[@]}"
expect_status 400 "missing stream" POST "$P" "${AUTH[@]}" --data-urlencode "session=$s0" "${EPOCH[@]}"
# (cpp-httplib folds byte-identical repeats into one parameter, so the
# ambiguous case is two different values.)
expect_status 400 "two session values" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" \
    --data-urlencode "session=$s1" --data-urlencode "session=$s0" "${EPOCH[@]}"
expect_status 400 "missing boot_epoch" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" \
    --data-urlencode "session=$s0"
for bad in "" 0123456789ABCDEF 0123456789abcde 0123456789abcdef0 0123456789abcde-; do
    expect_status 400 "boot_epoch '$bad'" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" \
        --data-urlencode "session=$s0" --data-urlencode "boot_epoch=$bad"
done
other_epoch=0000000000000000
[[ "$epoch" != "$other_epoch" ]] || other_epoch=1111111111111111
expect_status 400 "two boot_epoch values" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" \
    --data-urlencode "session=$s0" "${EPOCH[@]}" --data-urlencode "boot_epoch=$other_epoch"
expect_status 404 "right session, another boot's epoch" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" \
    --data-urlencode "session=$s0" --data-urlencode "boot_epoch=$other_epoch"
expect_status 400 "two stream values" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" \
    --data-urlencode "stream=publish/live/other" --data-urlencode "session=$s0" "${EPOCH[@]}"
for bad in abc 0 -1 " $s0" "${s0}x" 99999999999999999999 184467440737095516160; do
    expect_status 400 "session '$bad'" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" \
        --data-urlencode "session=$bad" "${EPOCH[@]}"
done
expect_status 400 "control byte in stream" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$(printf 'a\001b')" \
    --data-urlencode "session=$s0" "${EPOCH[@]}"
expect_status 404 "right session, other stream" POST "$P" "${AUTH[@]}" --data-urlencode "stream=publish/live/other" \
    --data-urlencode "session=$s0" "${EPOCH[@]}"
expect_status 404 "right session, stream prefix" POST "$P" "${AUTH[@]}" --data-urlencode "stream=publish/live" \
    --data-urlencode "session=$s0" "${EPOCH[@]}"
expect_status 404 "already kicked session" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" \
    --data-urlencode "session=$s1" "${EPOCH[@]}"
expect_status 404 "unknown session" POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" \
    --data-urlencode "session=$((s0 + 100000))" "${EPOCH[@]}"
expect_status 400 "publisher /disconnect still needs a stream" POST "/disconnect" "${AUTH[@]}"
body="$(api POST "$P" "${AUTH[@]}" --data-urlencode "stream=$STREAM_KEY" --data-urlencode "session=$((s0 + 100000))" "${EPOCH[@]}")"
[[ "$body" != *"$STREAM_KEY"* ]] || fail "a response echoed the stream key"
reader_closed pair 0 && fail "the survivor was disconnected by a refused request"
pass "auth, method, parameter and identity checks refuse every malformed or mismatched kick"

# --- 3. authorized-only player limit and the pending cap ---
# max_players_per_stream is 2 and session $s0 holds one slot. Slow readers
# (their webhook answers after 3 s, inside SLS's 4 s authorization deadline)
# stay pending meanwhile; the per-stream pending cap is 4.
slow_hook() { echo ".on_event == \"on_connect\" and (.remote_port | test(\"^($1)\$\"))"; }
for port in 47101 47102 47103; do
    start_reader "slow$port" --streamid play/live/kick --bind-port "$port" --duration-ms 20000
done
wait_until 10 hook_count_at_least "$(slow_hook '4710[1-3]')" 3 || fail "the slow readers did not reach the webhook"

# Before this change the limit counted pending players too (1 + 3 >= 2) and
# refused the next reader; now three pending readers leave the slot open.
start_reader fast47201 --streamid play/live/kick --bind-port 47201 --duration-ms 30000
wait_until 5 reader_at_least fast47201 0 16384 || fail "an authorized reader was crowded out by pending readers"
pass "three pending readers did not take the slot an authorized reader needed"

# Their authorizations resolve with the stream full: each is refused at
# authorization, is never sent media, and is released with on_close.
for port in 47101 47102 47103; do
    wait_until 10 reader_closed "slow$port" 0 || fail "slow reader $port was not refused once authorized"
    (( $(reader_total "slow$port" 0) == 0 )) || fail "over-limit reader $port received media"
done
wait_until 5 hook_count_at_least '.on_event == "on_close" and (.remote_port | test("^4710[1-3]$"))' 3 ||
    fail "refused authorized readers were not released with on_close"
grep -q "refused authorized player" "$SERVER_LOG" || fail "no authorization-time refusal logged"
start_reader fast47202 --streamid play/live/kick --bind-port 47202 --duration-ms 10000
wait_until 5 reader_closed fast47202 0 || fail "a third authorized reader was admitted past the limit"
(( $(reader_total fast47202 0) == 0 )) || fail "the over-limit reader received media"
pass "the limit still caps authorized players: over-limit readers are refused and never sent media"

# Free a slot, then fill the pending cap: a fifth pending reader is refused
# before any webhook call, and of the four pending only one wins the slot.
fast_session="$(hooks '.on_event == "on_connect" and .remote_port == "47201"' | jq -r .session_id)"
expect_status 200 "kick the hosted reader" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=$STREAM_KEY" --data-urlencode "session=$fast_session" "${EPOCH[@]}"
wait_until 5 reader_closed fast47201 0 || fail "kicked reader $fast_session stayed connected"
for port in 47111 47112 47113 47114; do
    start_reader "slow$port" --streamid play/live/kick --bind-port "$port" --duration-ms 20000
done
wait_until 10 hook_count_at_least "$(slow_hook '4711[1-4]')" 4 || fail "four pending readers did not reach the webhook"
start_reader slow47115 --streamid play/live/kick --bind-port 47115 --duration-ms 20000
wait_until 5 reader_closed slow47115 0 || fail "a fifth pending reader was not refused by the pending cap"
[[ "$(hook_count '.on_event == "on_connect" and .remote_port == "47115"')" == 0 ]] || fail "the pending-capped reader reached the webhook"
grep -q "pending players per stream at cap" "$SERVER_LOG" || fail "no pending-cap refusal logged"
pass "the per-stream pending cap refused a fifth pending reader before any webhook call"

settled() { reader_closed "$1" 0 || reader_at_least "$1" 0 16384; }
served=0
for port in 47111 47112 47113 47114; do
    wait_until 10 settled "slow$port" || fail "pending reader $port was neither admitted nor refused"
done
for port in 47111 47112 47113 47114; do
    if reader_at_least "slow$port" 0 16384; then
        reader_closed "slow$port" 0 && fail "the admitted reader $port was disconnected"
        served=$((served + 1))
    else
        (( $(reader_total "slow$port" 0) == 0 )) || fail "refused reader $port received media"
    fi
done
(( served == 1 )) || fail "$served of four concurrent authorizations took the one free slot"
players_json="$(stats | jq -c --arg k "$STREAM_KEY" '.publishers[$k].players')"
[[ "$(jq length <<<"$players_json")" == 2 ]] || fail "/stats does not show exactly two authorized players: $players_json"
refusals="$(stats | jq '.logging.securityEvents.admissionRefusals')"
(( refusals >= 8 )) || fail "admission refusals not counted (got $refusals)"
pass "exactly one of four racing authorizations took the free slot; refusals counted ($refusals)"

# A reader kicked while its authorization is pending is closed once the
# webhook answers, never sent media, and released with on_close (the webhook
# had authorized it).
for port in 47111 47112 47113 47114; do
    if reader_at_least "slow$port" 0 16384; then winner="$port"; fi
done
winner_session="$(hooks "$(slow_hook "$winner")" | jq -r .session_id)"
expect_status 200 "kick the admitted reader" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=$STREAM_KEY" --data-urlencode "session=$winner_session" "${EPOCH[@]}"
wait_until 5 reader_closed "slow$winner" 0 || fail "admitted reader $winner_session stayed connected"
start_reader slow47121 --streamid play/live/kick --bind-port 47121 --duration-ms 20000
wait_until 5 hook_count_at_least "$(slow_hook 47121)" 1 || fail "the pending reader never reached the webhook"
pending_session="$(hooks "$(slow_hook 47121)" | jq -r .session_id)"
expect_status 200 "kick a pending reader" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=$STREAM_KEY" --data-urlencode "session=$pending_session" "${EPOCH[@]}"
wait_until 10 reader_closed slow47121 0 || fail "the reader kicked while pending was not closed"
(( $(reader_total slow47121 0) == 0 )) || fail "the reader kicked while pending received media"
wait_until 5 hook_count_at_least ".on_event == \"on_close\" and .session_id == \"$pending_session\"" 1 ||
    fail "no on_close for the reader kicked while pending"
pass "a reader kicked while pending was closed after its verdict, unsent, with on_close"

stop_children
stop_sls
pass "server shut down cleanly with no sanitizer finding"

# --- 4. dispatch reservation ---
stop_children
: >"$HOOK_LOG"
SERVER_LOG="$WORKDIR/reserve-server.log"
PLAY_PORT=5702
PUB_PORT=5703
start_sls "$RESERVE_CONF"
# A fresh copy: srt_client keeps a side index next to its input file.
cp "$WORKDIR/in.ts" "$WORKDIR/in-reserve.ts"
start_publisher "publish/live/kick" "$WORKDIR/in-reserve.ts" reserve
wait_until 15 publisher_live "$STREAM_KEY" || fail "publisher never went live (reserve phase)"
# Two listeners + the publisher + this reader fill the open band (6 - 2).
start_reader open47001 --streamid play/live/kick --bind-port 47001 --duration-ms 30000
wait_until 10 reader_at_least open47001 0 65536 || fail "first unreserved reader not served"
start_reader wait47002 --streamid play/live/kick --bind-port 47002 --duration-ms 30000
wait_until 10 hook_count_at_least '.on_event == "on_connect" and .remote_port == "47002"' 1 ||
    fail "second unreserved reader never authorized"
start_reader res47203 --streamid play/live/kick --bind-port 47203 --duration-ms 30000
wait_until 10 reader_at_least res47203 0 65536 || fail "the reserved reader was not dispatched into the reserved band"
(( $(reader_total wait47002 0) == 0 )) || fail "an unreserved reader was dispatched into the reserved band"
pass "the reserved reader took a reserved slot while the unreserved one waited"
stop_children
stop_sls
pass "server shut down cleanly with no sanitizer finding (reserve phase)"

# --- 5. internal readers keep their pending capacity ---
# The resolver marks the hosted (hob_) and preview (pvw_) keys reserved; the
# public key is not. Four public readers held on their hook fill the
# per-stream pending cap (4), so a fifth public reader is refused before any
# webhook call, and the hosted and preview readers must still reach their
# webhook while all four stay held. Every HTTP pool gets 4 threads, as on a
# 4-core host, so the held public authorizations fill theirs.
kill "$HOOK_PID" 2>/dev/null || true
wait "$HOOK_PID" 2>/dev/null || true
: >"$HOOK_LOG"
start_hooks "$HOOK_PORT" --hold 47131-47135 --player-key "pubE2eKey0=$STREAM_KEY" \
    --reserved-player-key "hob_e2eHosted0=$STREAM_KEY@127.0.0.1" --reserved-player-key "pvw_e2ePreview0=$STREAM_KEY@127.0.0.1"
PLAY_PORT=5704
PUB_PORT=5705
SERVER_LOG="$WORKDIR/internal-server.log"
export SLS_HTTP_THREADS=4
start_sls "$INTERNAL_CONF"
unset SLS_HTTP_THREADS
cp "$WORKDIR/in.ts" "$WORKDIR/in-internal.ts"
start_publisher "publish/live/kick" "$WORKDIR/in-internal.ts" internal
wait_until 15 publisher_live "$STREAM_KEY" || fail "publisher never went live (internal phase)"
# The first public reader resolves the public key; the other three hit the cache.
start_reader pub47131 --streamid play/live/pubE2eKey0 --bind-port 47131 --duration-ms 20000
wait_until 10 hook_count_at_least "$(slow_hook 47131)" 1 || fail "the first public reader did not reach the webhook"
for port in 47132 47133 47134; do
    start_reader "pub$port" --streamid play/live/pubE2eKey0 --bind-port "$port" --duration-ms 20000
done
wait_until 10 hook_count_at_least "$(slow_hook '4713[1-4]')" 4 || fail "the public readers did not reach the webhook"
# A refusal is counted the moment it happens; the refused reader itself may
# notice the close only seconds later, after the holds have ended.
admission_refusals() { stats | jq '.logging.securityEvents.admissionRefusals'; }
refusals_above() { (( $(admission_refusals) > $1 )); }
refused="$(admission_refusals)"
start_reader pub47135 --streamid play/live/pubE2eKey0 --bind-port 47135 --duration-ms 20000
wait_until 5 refusals_above "$refused" || fail "a fifth public reader was not refused by the pending cap"
[[ "$(hook_count '.on_event == "on_connect" and .remote_port == "47135"')" == 0 ]] || fail "the capped public reader reached the webhook"
start_reader hosted --streamid play/live/hob_e2eHosted0 --bind-port 47301 --duration-ms 20000
wait_until 5 hook_count_at_least "$(slow_hook 47301)" 1 || fail "the hosted reader never reached its webhook"
start_reader preview --streamid play/live/pvw_e2ePreview0 --bind-port 47302 --duration-ms 20000
wait_until 5 hook_count_at_least "$(slow_hook 47302)" 1 || fail "the preview reader never reached its webhook"
# The hook server still held all four public authorizations when each arrived.
for port in 47301 47302; do
    held="$(hooks "$(slow_hook "$port")" | jq -s '.[0].held')"
    (( held >= 4 )) || fail "reader $port reached its webhook with only $held public authorizations held"
done
release_hooks "$HOOK_PORT"
wait_until 10 reader_closed pub47135 0 || fail "the refused public reader stayed connected"
wait_until 10 reader_at_least hosted 0 65536 || fail "the hosted reader was not admitted"
wait_until 10 reader_at_least preview 0 65536 || fail "the preview reader was not admitted"
for port in 47131 47132 47133 47134; do
    wait_until 10 reader_at_least "pub$port" 0 16384 || fail "public reader $port was not admitted after its verdict"
done
pass "hosted and preview readers reached their webhook while four public authorizations were held"
stop_children
stop_sls
pass "server shut down cleanly with no sanitizer finding (internal phase)"

# --- 6. a kick from an earlier boot never reaches a session of this one ---
# Each boot numbers its players from 1, so the first reader of each boot gets
# the same session id; only the boot epoch tells the two apart.
boot_hook() { echo ".on_event == \"on_connect\" and .remote_port == \"$1\""; }
SERVER_LOG="$WORKDIR/boot1-server.log"
PLAY_PORT=5700
PUB_PORT=5701
start_sls "$CONF"
cp "$WORKDIR/in.ts" "$WORKDIR/in-boot1.ts"
start_publisher "publish/live/kick" "$WORKDIR/in-boot1.ts" boot1
wait_until 15 publisher_live "$STREAM_KEY" || fail "publisher never went live (first boot)"
start_reader boot1 --streamid play/live/kick --bind-port 47401 --duration-ms 30000
wait_until 10 reader_at_least boot1 0 65536 || fail "the first boot's reader received no media"
old_session="$(hooks "$(boot_hook 47401)" | jq -r .session_id)"
old_epoch="$(hooks "$(boot_hook 47401)" | jq -r .boot_epoch)"
stop_children
stop_sls
SERVER_LOG="$WORKDIR/boot2-server.log"
start_sls "$CONF"
cp "$WORKDIR/in.ts" "$WORKDIR/in-boot2.ts"
start_publisher "publish/live/kick" "$WORKDIR/in-boot2.ts" boot2
wait_until 15 publisher_live "$STREAM_KEY" || fail "publisher never went live (second boot)"
start_reader boot2 --streamid play/live/kick --bind-port 47402 --duration-ms 30000
wait_until 10 reader_at_least boot2 0 65536 || fail "the second boot's reader received no media"
new_session="$(hooks "$(boot_hook 47402)" | jq -r .session_id)"
new_epoch="$(hooks "$(boot_hook 47402)" | jq -r .boot_epoch)"
[[ "$new_session" == "$old_session" ]] || fail "the restart did not reuse session id $old_session (got $new_session)"
[[ "$old_epoch" =~ ^[0-9a-f]{16}$ && "$new_epoch" =~ ^[0-9a-f]{16}$ && "$old_epoch" != "$new_epoch" ]] ||
    fail "the two boots do not have distinct boot epochs: '$old_epoch' '$new_epoch'"
[[ "$(stats | jq -r .bootEpoch)" == "$new_epoch" ]] || fail "/stats bootEpoch is not the new boot's $new_epoch"
expect_status 404 "the first boot's kick for session $old_session" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=$STREAM_KEY" --data-urlencode "session=$old_session" --data-urlencode "boot_epoch=$old_epoch"
mark="$(reader_total boot2 0)"
wait_until 10 reader_at_least boot2 0 $((mark + 131072)) || fail "the new session stopped receiving after an old-boot kick"
reader_closed boot2 0 && fail "an old-boot kick closed the new session $new_session"
[[ "$(hook_count ".on_event == \"on_close\" and .boot_epoch == \"$new_epoch\"")" == 0 ]] ||
    fail "an old-boot kick released a new-boot session"
expect_status 200 "this boot's kick for session $new_session" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=$STREAM_KEY" --data-urlencode "session=$new_session" --data-urlencode "boot_epoch=$new_epoch"
wait_until 5 reader_closed boot2 0 || fail "this boot's kick did not close session $new_session"
pass "a kick carrying the earlier boot's epoch missed the reused session $new_session; this boot's kick closed it"
stop_children
stop_sls
pass "server shut down cleanly with no sanitizer finding (restart phase)"

restart_hooks() {
    kill "$HOOK_PID" 2>/dev/null || true
    wait "$HOOK_PID" 2>/dev/null || true
    : >"$HOOK_LOG"
    start_hooks "$HOOK_PORT" "$@"
}

# --- 7. a reserved key's internal grant is bound to its peer and capped ---
# Four public readers held on their hook fill the per-stream pending cap (4),
# so a reader denied an internal grant is refused before any webhook call.
# Grants are held too, so they stay pending until release. 127.0.0.2 stands in
# for another peer.
grant_keys=()
# Keys 1-5 are bound to 127.0.0.2, 0, 6 and 8 to 127.0.0.1, 7 to 127.0.0.3.
for k in 0 1 2 3 4 5 6 7 8; do
    bound=127.0.0.1
    (( k >= 1 && k <= 5 )) && bound=127.0.0.2
    (( k == 7 )) && bound=127.0.0.3
    grant_keys+=(--reserved-player-key "hob_e2eGrant$k=$STREAM_KEY@$bound")
done
restart_hooks --hold 47140-47176 --player-key "pubE2eKey0=$STREAM_KEY" "${grant_keys[@]}"
SERVER_LOG="$WORKDIR/grant-server.log"
PLAY_PORT=5704
PUB_PORT=5705
export SLS_HTTP_THREADS=8
start_sls "$INTERNAL_CONF"
unset SLS_HTTP_THREADS
cp "$WORKDIR/in.ts" "$WORKDIR/in-grant.ts"
start_publisher "publish/live/kick" "$WORKDIR/in-grant.ts" grant
wait_until 15 publisher_live "$STREAM_KEY" || fail "publisher never went live (grant phase)"
fill_public_cap() {
    local base="$1" p
    start_reader "pub$base" --streamid play/live/pubE2eKey0 --bind-port "$base" --duration-ms 20000
    wait_until 10 hook_count_at_least "$(slow_hook "$base")" 1 || fail "public reader $base did not reach the webhook"
    for p in $((base + 1)) $((base + 2)) $((base + 3)); do
        start_reader "pub$p" --streamid play/live/pubE2eKey0 --bind-port "$p" --duration-ms 20000
    done
    wait_until 10 hook_count_at_least "$(slow_hook "$base|$((base + 1))|$((base + 2))|$((base + 3))")" 4 ||
        fail "the public readers from port $base did not reach the webhook"
}

fill_public_cap 47140
# The key's first presenter holds its grant while its verdict is pending, so
# a copy of the key from another peer is public and the full cap refuses it.
start_reader grantA --streamid play/live/hob_e2eGrant0 --bind-port 47150 --duration-ms 20000
wait_until 5 hook_count_at_least "$(slow_hook 47150)" 1 || fail "the reserved key's first presenter was refused"
refused="$(admission_refusals)"
start_reader grantB --streamid play/live/hob_e2eGrant0 --bind-addr 127.0.0.2 --bind-port 47151 --duration-ms 20000
wait_until 5 refusals_above "$refused" || fail "a copied reserved key from another peer took internal capacity"
[[ "$(hook_count '.on_event == "on_connect" and .remote_port == "47151"')" == 0 ]] || fail "the copied reserved key reached the webhook"
# The key's own peer cannot take a second grant while the first is pending.
refused="$(admission_refusals)"
start_reader grantC --streamid play/live/hob_e2eGrant0 --bind-port 47152 --duration-ms 20000
wait_until 5 refusals_above "$refused" || fail "a second reader of one reserved key took a second internal grant"
[[ "$(hook_count '.on_event == "on_connect" and .remote_port == "47152"')" == 0 ]] || fail "the reserved key's second reader reached the webhook"
# A key the resolver did not verify for this peer (no internal_class) is public.
refused="$(admission_refusals)"
start_reader grantU --streamid play/live/hob_e2eGrant7 --bind-port 47153 --duration-ms 20000
wait_until 5 refusals_above "$refused" || fail "a key not verified for this peer took internal capacity"
[[ "$(hook_count '.on_event == "on_connect" and .remote_port == "47153"')" == 0 ]] || fail "the unverified key reached the webhook"
pass "a copied key from another peer, a second grant for one key and an unverified key all stayed public"
# Lookups are cached per key and peer: a wrong peer presenting a key first
# caches "public" for that peer only, and the bound peer, inside the cache
# window, still gets its internal grant.
refused="$(admission_refusals)"
start_reader grantW --streamid play/live/hob_e2eGrant8 --bind-addr 127.0.0.2 --bind-port 47154 --duration-ms 20000
wait_until 5 refusals_above "$refused" || fail "a wrong peer's reader of a bound key took internal capacity"
start_reader grantR --streamid play/live/hob_e2eGrant8 --bind-port 47155 --duration-ms 20000
wait_until 5 hook_count_at_least "$(slow_hook 47155)" 1 ||
    fail "the bound peer was refused after a wrong peer's lookup of its key was cached"
pass "a wrong peer's lookup of a bound key did not downgrade the bound peer"
release_hooks "$HOOK_PORT"
wait_until 10 reader_closed grantB 0 || fail "the copied reserved key's reader stayed connected"

for p in 47140 47141 47142 47143; do
    wait_until 10 reader_at_least "pub$p" 0 16384 || fail "public reader $p was not admitted after its verdict"
done
wait_until 10 reader_at_least grantA 0 16384 || fail "the reserved key's first presenter was not admitted"
wait_until 10 reader_at_least grantR 0 16384 || fail "the bound peer of the wrong-peer-first key was not admitted"
fill_public_cap 47160
for k in 1 2 3 4; do
    start_reader "peer$k" --streamid "play/live/hob_e2eGrant$k" --bind-addr 127.0.0.2 --bind-port $((47170 + k)) \
        --duration-ms 20000
done
wait_until 5 hook_count_at_least "$(slow_hook '4717[1-4]')" 4 || fail "one peer was refused grants below the per-peer cap"
refused="$(admission_refusals)"
start_reader peer5 --streamid play/live/hob_e2eGrant5 --bind-addr 127.0.0.2 --bind-port 47175 --duration-ms 20000
wait_until 5 refusals_above "$refused" || fail "one peer took a reserved grant past the per-peer cap"
[[ "$(hook_count '.on_event == "on_connect" and .remote_port == "47175"')" == 0 ]] || fail "the reserved key past the per-peer cap reached the webhook"
start_reader peer6 --streamid play/live/hob_e2eGrant6 --bind-port 47176 --duration-ms 20000
wait_until 5 hook_count_at_least "$(slow_hook 47176)" 1 || fail "another peer was refused a grant because one peer was capped"
held="$(hooks "$(slow_hook 47176)" | jq -s '.[0].held')"
(( held >= 8 )) || fail "the other peer's grant arrived with only $held authorizations held, not the full cap and four grants"
release_hooks "$HOOK_PORT"
wait_until 10 reader_closed peer5 0 || fail "the reader past the per-peer cap stayed connected"
pass "one peer held at most four reserved grants, and another peer still got one"
stop_children
stop_sls
pass "server shut down cleanly with no sanitizer finding (grant phase)"

# --- 8. a reload keeps a verified internal reader's worker reservation ---
# One worker, 6 slots, 2 reserved. Before the reload it holds the two
# listeners, the publisher, an unreserved reader and two reserved ones; the
# reload drops the listeners, so the 4 roles left fill the open band.
restart_hooks --slow 47180-47189 --slow-ms 3000 --reserved 47190-47199 --player-key "pubE2eKey0=$STREAM_KEY" \
    --reserved-player-key "hob_e2eReload0=$STREAM_KEY@127.0.0.1"
SERVER_LOG="$WORKDIR/reload-reserve-server.log"
PLAY_PORT=5706
PUB_PORT=5707
start_sls "$RELOAD_RESERVE_CONF"
cp "$WORKDIR/in.ts" "$WORKDIR/in-reload.ts"
start_publisher "publish/live/kick" "$WORKDIR/in-reload.ts" reload
wait_until 15 publisher_live "$STREAM_KEY" || fail "publisher never went live (reload phase)"
for reader in "resU 47210" "resR1 47190" "resR2 47191"; do
    set -- $reader
    start_reader "$1" --streamid play/live/pubE2eKey0 --bind-port "$2" --duration-ms 60000
    wait_until 10 reader_at_least "$1" 0 65536 || fail "reader $1 was not dispatched before the reload"
done
start_reader resH --streamid play/live/hob_e2eReload0 --bind-port 47180 --duration-ms 60000
start_reader resP --streamid play/live/pubE2eKey0 --bind-port 47181 --duration-ms 60000
wait_until 5 hook_count_at_least "$(slow_hook '4718[01]')" 2 || fail "the readers to hold across the reload were refused"
# The same hosted key copied to 127.0.0.2: public, so it may not take the
# reserved band after the reload either.
start_reader resC --streamid play/live/hob_e2eReload0 --bind-addr 127.0.0.2 --bind-port 47182 --duration-ms 60000
wait_until 5 hook_count_at_least "$(slow_hook 47182)" 1 || fail "the copied key's reader never reached the webhook"
c_session="$(hooks "$(slow_hook 47182)" | jq -r .session_id)"
h_session="$(hooks "$(slow_hook 47180)" | jq -r .session_id)"
p_session="$(hooks "$(slow_hook 47181)" | jq -r .session_id)"
before="$(grep -c 'Reloaded successfully.' "$SERVER_LOG" || true)"
kill -HUP "$SERVER_PID"
wait_until 10 bash -c "(( \$(grep -c 'Reloaded successfully.' '$SERVER_LOG') > $before ))" || fail "the reload did not complete"
wait_until 15 reader_at_least resH 0 65536 || fail "the internal reader lost its worker reservation across the reload"
# The public one may not take the reserved band. It still gets its verdict,
# is released with on_close, and is never sent media.
close_hook() { echo ".on_event == \"on_close\" and .session_id == \"$1\""; }
wait_until 15 hook_count_at_least "$(close_hook "$p_session")" 1 || fail "the public reader left in the backlog got no on_close"
(( $(reader_total resP 0) == 0 )) || fail "the public reader took a reserved slot"
wait_until 15 hook_count_at_least "$(close_hook "$c_session")" 1 || fail "the copied key's reader got no on_close"
(( $(reader_total resC 0) == 0 )) || fail "the copied key's reader took a reserved slot"
stop_children
wait_until 10 hook_count_at_least "$(close_hook "$h_session")" 1 || fail "the internal reader got no on_close"
for s in "$h_session" "$p_session" "$c_session"; do
    [[ "$(hook_count "$(close_hook "$s")")" == 1 ]] || fail "session $s did not get exactly one on_close"
done
pass "after a reload the internal reader kept its reservation and played; the public one was released with one on_close"
stop_sls
pass "server shut down cleanly with no sanitizer finding (reload phase)"

echo "PLAYER-ADMISSION PASS"
