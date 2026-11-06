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
E2E_NAME=PLAYER-ADMISSION
source "$(dirname "$0")/lib_e2e.sh"

CONF="$E2E_DIR/sls-player-admission.conf"
RESERVE_CONF="$E2E_DIR/sls-dispatch-reserve.conf"
INTERNAL_CONF="$E2E_DIR/sls-internal-pending.conf"
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
[[ "$(hook_count '.remote_port == "47115"')" == 0 ]] || fail "the pending-capped reader reached the webhook"
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
# public key is not. Four slow public readers fill the per-stream pending cap
# (4), so a fifth public reader is refused before any webhook call, while the
# hosted and preview readers are admitted during that window.
kill "$HOOK_PID" 2>/dev/null || true
wait "$HOOK_PID" 2>/dev/null || true
: >"$HOOK_LOG"
start_hooks "$HOOK_PORT" --slow 47100-47199 --slow-ms 3500 --player-key "pubE2eKey0=$STREAM_KEY" \
    --reserved-player-key "hob_e2eHosted0=$STREAM_KEY" --reserved-player-key "pvw_e2ePreview0=$STREAM_KEY"
SERVER_LOG="$WORKDIR/internal-server.log"
start_sls "$INTERNAL_CONF"
cp "$WORKDIR/in.ts" "$WORKDIR/in-internal.ts"
start_publisher "publish/live/kick" "$WORKDIR/in-internal.ts" internal
wait_until 15 publisher_live "$STREAM_KEY" || fail "publisher never went live (internal phase)"
# Key lookups run one at a time: the first public reader resolves the public
# key, the other three hit the cache, and the hosted key resolves before the
# preview key.
start_reader pub47131 --streamid play/live/pubE2eKey0 --bind-port 47131 --duration-ms 20000
wait_until 10 hook_count_at_least "$(slow_hook 47131)" 1 || fail "the first public reader did not reach the webhook"
for port in 47132 47133 47134; do
    start_reader "pub$port" --streamid play/live/pubE2eKey0 --bind-port "$port" --duration-ms 20000
done
wait_until 10 hook_count_at_least "$(slow_hook '4713[1-4]')" 4 || fail "the public readers did not reach the webhook"
start_reader pub47135 --streamid play/live/pubE2eKey0 --bind-port 47135 --duration-ms 20000
start_reader hosted --streamid play/live/hob_e2eHosted0 --bind-port 47301 --duration-ms 20000
wait_until 5 hook_count_at_least '.player_key == "hob_e2eHosted0" and .status == 200' 1 || fail "the hosted key was not resolved"
start_reader preview --streamid play/live/pvw_e2ePreview0 --bind-port 47302 --duration-ms 20000
wait_until 5 reader_closed pub47135 0 || fail "a fifth public reader was not refused by the pending cap"
[[ "$(hook_count '.remote_port == "47135"')" == 0 ]] || fail "the capped public reader reached the webhook"
wait_until 10 reader_at_least hosted 0 65536 || fail "the hosted reader was refused while public readers filled the cap"
wait_until 10 reader_at_least preview 0 65536 || fail "the preview reader was refused while public readers filled the cap"
# Both passed the pending caps while all four public readers were pending:
# their on_connect left SLS before the first public verdict came back.
flood_end="$(hooks "$(slow_hook '4713[1-4]')" | jq -s 'map(.t) | min + 3.5')"
for port in 47301 47302; do
    t="$(hooks ".on_event == \"on_connect\" and .remote_port == \"$port\"" | jq -s '.[0].t')"
    jq -en --argjson t "$t" --argjson end "$flood_end" '$t < $end' >/dev/null ||
        fail "reader $port reached its webhook only after the public flood resolved"
done
pass "hosted and preview readers were admitted while public pending readers filled the cap"
stop_children
stop_sls
pass "server shut down cleanly with no sanitizer finding (internal phase)"

# --- 6. a kick from an earlier boot never reaches a session of this one ---
# Each boot numbers its players from 1, so the first reader of each boot gets
# the same session id; only the boot epoch tells the two apart.
boot_hook() { echo ".on_event == \"on_connect\" and .remote_port == \"$1\""; }
SERVER_LOG="$WORKDIR/boot1-server.log"
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

echo "PLAYER-ADMISSION PASS"
