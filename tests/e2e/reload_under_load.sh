#!/usr/bin/env bash
# SIGHUP reloads while media flows and the control plane is hammered.
#
# Three reloads run while /stats and /disconnect-player requests are issued
# continuously and readers join each generation. A reload retires the running
# manager (it keeps serving the readers it has) and starts a new one, so this
# exercises the HTTP side reaching managers that are retiring, being drained
# and destroyed. Built with -DSLS_TSAN=ON (or SLS_SANITIZE) it is the
# reload-lifetime race test: any finding fails stop_sls.
#
# Checked: /stats lists the readers of every live generation with their
# session ids; a reader still waiting for its on_event_url verdict when SIGHUP
# lands survives the reload, is admitted once the verdict arrives, plays, and
# gets exactly one on_close; session ids stay unique across generations; a
# reader from before
# every reload can still be kicked by session, exactly, while its neighbour on
# the same peer port keeps playing; drained managers are destroyed under load;
# the control plane never fails a request other than the documented 500 while
# no generation is current; clean shutdown.
E2E_NAME=RELOAD-UNDER-LOAD
source "$(dirname "$0")/lib_e2e.sh"

CONF="$E2E_DIR/sls-reload.conf"
HTTP="http://127.0.0.1:8287"
API_KEY="reload-test-key"
PUB_PORT=5711
PLAY_PORT=5710
HOOK_PORT=8297
AUTH=(-H "Authorization: $API_KEY")
LOAD_FLAG="$WORKDIR/load.run"

make_ts "$WORKDIR/in.ts" 120
# Port 47320's on_connect is answered after 3 s, so that reader is still
# pending when the first reload lands.
start_hooks "$HOOK_PORT" --slow 47320-47320 --slow-ms 3000
start_sls "$CONF"

reloads_done() { grep -c "Reloaded successfully." "$SERVER_LOG" || true; }
player_hook() { echo ".on_event == \"on_connect\" and .role_name == \"player\" and .remote_port == \"$1\""; }
session_of() { hooks "$(player_hook "$1")" | sed -n "${2:-1}p" | jq -r .session_id; }

# Generation 0: a publisher and two readers sharing one UDP port.
cp "$WORKDIR/in.ts" "$WORKDIR/gen0.ts"
start_publisher "publish/live/gen0" "$WORKDIR/gen0.ts" gen0
wait_until 15 publisher_live "publish/live/gen0" || fail "gen0 publisher never went live"
mkfifo "$WORKDIR/gen0pair.in"
exec 7<>"$WORKDIR/gen0pair.in"
start_reader gen0pair --streamid play/live/gen0 --sockets 2 --bind-port 47301 --wait-stdin --duration-ms 120000 \
    <"$WORKDIR/gen0pair.in"
wait_until 10 hook_count_at_least "$(player_hook 47301)" 1 || fail "gen0 reader 0 not authorized"
echo next >&7
wait_until 10 hook_count_at_least "$(player_hook 47301)" 2 || fail "gen0 reader 1 not authorized"
wait_until 10 reader_at_least gen0pair 0 65536 || fail "gen0 reader 0 received no media"
wait_until 10 reader_at_least gen0pair 1 65536 || fail "gen0 reader 1 received no media"
g0a="$(session_of 47301 1)"
g0b="$(session_of 47301 2)"

# A reader whose verdict is outstanding across the first reload.
start_reader pend --streamid play/live/gen0 --bind-port 47320 --duration-ms 120000
wait_until 10 hook_count_at_least "$(player_hook 47320)" 1 || fail "the slow reader's on_connect never arrived"
pend_session="$(session_of 47320)"

# Control-plane load for the rest of the run: four /stats pollers and two
# kick loops sending well-formed kicks for sessions that do not exist (404)
# and malformed ones (400). Every status code is recorded.
touch "$LOAD_FLAG"
LOAD_PIDS=()
for i in 1 2 3 4; do
    (
        while [[ -e "$LOAD_FLAG" ]]; do
            curl -s -o /dev/null -w '%{http_code}\n' "${AUTH[@]}" "$HTTP/stats" || echo "curl-error"
        done
    ) >"$WORKDIR/load-stats-$i.codes" 2>&1 &
    LOAD_PIDS+=("$!")
done
for i in 1 2; do
    (
        n=0
        while [[ -e "$LOAD_FLAG" ]]; do
            n=$((n + 1))
            curl -s -o /dev/null -w '%{http_code}\n' -X POST "${AUTH[@]}" "$HTTP/disconnect-player" \
                --data-urlencode "stream=publish/live/gen0" --data-urlencode "session=$((9000000 + n))" ||
                echo "curl-error"
            curl -s -o /dev/null -w '%{http_code}\n' -X POST "${AUTH[@]}" "$HTTP/disconnect-player" \
                --data-urlencode "stream=publish/live/gen0" --data-urlencode "session=x$n" || echo "curl-error"
        done
    ) >"$WORKDIR/load-kick-$i.codes" 2>&1 &
    LOAD_PIDS+=("$!")
done

# Three reloads; after each, a new publisher and reader join the new generation.
for gen in 1 2 3; do
    before="$(reloads_done)"
    kill -HUP "$SERVER_PID"
    wait_until 20 bash -c "(( \$(grep -c 'Reloaded successfully.' '$SERVER_LOG') > $before ))" ||
        fail "reload $gen did not complete"
    cp "$WORKDIR/in.ts" "$WORKDIR/gen$gen.ts"
    start_publisher "publish/live/gen$gen" "$WORKDIR/gen$gen.ts" "gen$gen"
    wait_until 15 publisher_live "publish/live/gen$gen" || fail "gen$gen publisher never went live"
    start_reader "gen${gen}r" --streamid "play/live/gen$gen" --bind-port $((47310 + gen)) --duration-ms 120000
    wait_until 10 reader_at_least "gen${gen}r" 0 65536 || fail "gen$gen reader received no media"
done
pass "three SIGHUP reloads completed under /stats and /disconnect-player load"

grep -q "player session=$pend_session stream=publish/live/gen0 handed to the workers for the reload" "$SERVER_LOG" ||
    fail "the first reload did not land while session $pend_session was pending"
wait_until 15 reader_at_least pend 0 65536 || fail "the reader pending across the reload never received media"
grep -q "player session=$pend_session stream=publish/live/gen0 authorized after a reload, admitted" "$SERVER_LOG" ||
    fail "session $pend_session was not admitted after the reload"
pass "a reader pending on its hook across a reload was admitted after it and played (session $pend_session)"

# gen0's readers lived through every reload in a retiring manager.
mark0="$(reader_total gen0pair 0)"
wait_until 10 reader_at_least gen0pair 0 $((mark0 + 131072)) || fail "a pre-reload reader stopped receiving"
g1="$(session_of 47311)"
g2="$(session_of 47312)"
g3="$(session_of 47313)"
ids="$(printf '%s\n' "$g0a" "$g0b" "$g1" "$g2" "$g3")"
[[ "$(sort -n <<<"$ids" | uniq | wc -l)" == 5 ]] || fail "session ids repeat across generations: $(tr '\n' ' ' <<<"$ids")"
[[ "$(sort -n <<<"$ids")" == "$ids" ]] || fail "session ids are not increasing across generations"
pass "session ids are unique and increasing across four generations ($(tr '\n' ' ' <<<"$ids"))"

# /stats lists the readers of every live generation, not only the current
# one, each under its stream with its session id.
all_stats="$(stats)"
for entry in "gen0:$g0a" "gen0:$g0b" "gen0:$pend_session" "gen1:$g1" "gen2:$g2" "gen3:$g3"; do
    jq -e --arg k "publish/live/${entry%%:*}" --argjson s "${entry#*:}" \
        '[.publishers[$k].players[]?.sessionId] | index($s) != null' <<<"$all_stats" >/dev/null ||
        fail "/stats does not list session ${entry#*:} of publish/live/${entry%%:*} after the reloads"
done
one_stats="$(curl -fsS -G "${AUTH[@]}" "$HTTP/stats" --data-urlencode "publisher=publish/live/gen0")"
[[ "$(jq -c '[.publishers["publish/live/gen0"].players[].sessionId] | sort' <<<"$one_stats")" == \
    "$(jq -cn --argjson a "$g0a" --argjson b "$g0b" --argjson c "$pend_session" '[$a, $b, $c] | sort')" ]] ||
    fail "/stats?publisher= does not list the gen0 readers: $one_stats"
pass "/stats lists the readers of all four generations with their session ids"

# Kick one reader in each kind of generation: the oldest retiring one (the
# second of a shared-port pair), a middle retiring one, and the current one.
expect_status 200 "kick pre-reload session $g0b" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=publish/live/gen0" --data-urlencode "session=$g0b"
wait_until 5 reader_closed gen0pair 1 || fail "pre-reload session $g0b was not disconnected"
mark0="$(reader_total gen0pair 0)"
wait_until 10 reader_at_least gen0pair 0 $((mark0 + 131072)) || fail "its neighbour $g0a stopped receiving"
reader_closed gen0pair 0 && fail "kicking $g0b also closed $g0a"
expect_status 404 "session from gen1 with gen0's stream" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=publish/live/gen0" --data-urlencode "session=$g1"
expect_status 200 "kick gen1 session $g1" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=publish/live/gen1" --data-urlencode "session=$g1"
wait_until 5 reader_closed gen1r 0 || fail "retiring-generation session $g1 was not disconnected"
expect_status 200 "kick current session $g3" POST "/disconnect-player" "${AUTH[@]}" \
    --data-urlencode "stream=publish/live/gen3" --data-urlencode "session=$g3"
wait_until 5 reader_closed gen3r 0 || fail "current-generation session $g3 was not disconnected"
reader_closed gen2r 0 && fail "an untargeted reader in gen2 was disconnected"
pass "kicks reached the oldest retiring, a middle retiring and the current generation, each exactly"

# Let the retired generations drain while the load continues: their
# managers are stopped and destroyed with HTTP requests in flight.
stop_children
wait_until 30 bash -c "(( \$(grep -c 'Checking reloaded manager, deleting manager' '$SERVER_LOG') >= 3 ))" ||
    fail "the three retired managers were not destroyed after draining"
pass "three drained managers were stopped and destroyed while the control plane was under load"

pend_hook() { echo ".role_name == \"player\" and .on_event == \"$1\" and .session_id == \"$pend_session\""; }
wait_until 10 hook_count_at_least "$(pend_hook on_close)" 1 || fail "session $pend_session got no on_close"
connects="$(hooks "$(pend_hook on_connect)" | wc -l)"
closes="$(hooks "$(pend_hook on_close)" | wc -l)"
(( connects == 1 && closes == 1 )) ||
    fail "session $pend_session hook lifecycle unbalanced: $connects on_connect, $closes on_close"
pass "the reader pending across the reload got exactly one on_connect and one on_close"

rm -f "$LOAD_FLAG"
for pid in "${LOAD_PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
codes="$(cat "$WORKDIR"/load-*.codes | sort | uniq -c | tr -s ' ' | tr '\n' ' ')"
total="$(cat "$WORKDIR"/load-*.codes | wc -l)"
(( total > 100 )) || fail "control-plane load issued only $total requests"
if grep -qvE '^(200|400|404|500)$' "$WORKDIR"/load-*.codes; then
    fail "control plane returned an unexpected result under reload: $codes"
fi
(( $(cat "$WORKDIR"/load-stats-*.codes | grep -c '^200$') > 0 )) || fail "no /stats succeeded"
pass "$total control-plane requests during reloads, results: $codes"

stop_sls
pass "server shut down cleanly with no sanitizer finding"
echo "RELOAD-UNDER-LOAD PASS"
