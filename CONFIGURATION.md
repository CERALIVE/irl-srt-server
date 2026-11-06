# Configuration reference

This document catalogs the configuration directives that the IRL fork of `srt-live-server` adds on top of the upstream SLS project, plus the base directives whose behavior changed in this fork. The canonical example with inline comments is `src/sls.conf`.

For directives inherited unchanged from upstream SLS see the upstream wiki at `https://github.com/rstular/srt-live-server/wiki/Directives`. Feature deep dives live in [`BITRATE_LIMITING.md`](BITRATE_LIMITING.md) and [`PLAYER_KEY_IMPLEMENTATION.md`](PLAYER_KEY_IMPLEMENTATION.md). This file links to those rather than restating them.

## Conventions

- Directives live in one of three scopes: `srt { }` (global), `srt { server { } }` (per server), or `srt { server { app { } } }` (per app).
- Boolean values accept `true` / `false` or `1` / `0`. Strings may be optionally wrapped in single or double quotes; the quotes are stripped at parse time.
- Time values are in the unit the directive name implies (`_ms` suffix for milliseconds, `_timeout` is usually seconds unless otherwise noted, see the per directive entry).
- `-1` is the conventional "unlimited" sentinel for counted limits.

## Global scope (`srt { ... }`)

### Server runtime

| Directive | Type | Default | Notes |
|---|---|---|---|
| `worker_threads` | int | `1` | Worker threads handling SRT connections. Bump for many concurrent streams (more than 5). |
| `worker_connections` | int | `300` | Max concurrent SRT connections per worker. |
| `worker_reserved_connections` | int | `0` (none) | Worker slots held back from unreserved players. A player is reserved when its `on_event_url` authorization answers 200 with `{"reserved": true}`; publishers and listeners may always use the reserved slots. Unreserved players are dispatched only while a worker has more than this many free slots, so ordinary readers cannot fill the capacity hosted or internal readers need. Must be below `worker_connections` (otherwise ignored with a warning). Only meaningful with `on_event_url`: without it no player is reserved. |
| `pidfile` | path | `/tmp/sls/sls_server.pid` | PID file path; created on start, removed on stop. |
| `user` | string | (unset) | Drop privileges to this user after binding. Only applies when started as root, otherwise ignored with a warning. |
| `group` | string | user's primary group | Drop privileges to this group. Pairs with `user`. |

### HTTP stats and control API

| Directive | Type | Default | Notes |
|---|---|---|---|
| `http_port` | int | `8181` | Port for the HTTP stats / control API. Exposes `/stats`, `/healthz`, and the disconnect endpoints. |
| `cors_header` | string | `*` | Value sent in `Access-Control-Allow-Origin`. Restrict to a specific domain for production. |
| `api_keys` | string list | (unset) | Comma separated API keys. Clients pass one in `Authorization: <key>` to use the control endpoints without the `?publisher=` query. Compared in constant time. |
| `stat_post_url` | url | (unset) | If set, SLS POSTs the stats JSON to this URL on the interval below. |
| `stat_post_interval` | int (seconds) | `1` | Interval between auto posted stats payloads. |

#### Control endpoints

- `GET /stats` lists each player of a publisher with a `sessionId`: a positive integer, unique for the life of the process (it survives `SIGHUP` reloads and two sessions sharing one peer `ip:port` get different ids). An authorized response also carries a top level `logging` object, so the log pipeline can be monitored even when log lines are dropped: `droppedMessages` and `securityDroppedMessages` (lines lost to queue overrun), `suppressedRefusals` (refusal lines over the shared budget), `sinkStalled` (a log write has been blocked for at least 1 s right now), `sinkStallEvents`, and `securityEvents` with exact `authRefusals`, `admissionRefusals`, `kicks` and `adminDisconnects` counts.
- `POST /disconnect-player` kicks exactly one player. Parameters (query or form): `stream`, the publisher map key as listed in `/stats`, and `session`, the player's `sessionId` (also sent to `on_event_url` as `session_id`). Both must match the same live player. Requires `Authorization: <api key>`. Answers 200 when the kick is queued (the player's own worker closes it within a few tens of milliseconds and `on_close` follows), 400 for a missing, repeated or malformed parameter (`session` must be 1 to 20 decimal digits, not 0), 401 without a valid key, 404 when no live player matches. It reaches players of managers still draining after a reload. Responses never echo the stream key. It is a separate route from `/disconnect`, so a request that drops a parameter can never kick a publisher.
- `POST /disconnect?stream=` (publisher kick) is unchanged, except that a publisher still served by a manager draining after a reload can now be kicked too.

### Logging

The fork ships a redesigned logging system designed to keep log volume sane on IRL setups where players and OBS reconnect frequently. The per category levels and the rate limiter together let an operator silence noisy subsystems without losing real errors.

| Directive | Type | Default | Notes |
|---|---|---|---|
| `log_file` | path | `logs/srt_server.log` | Log file path. |
| `log_level` | enum | `info` | Global log level (`trace`, `debug`, `info`, `warn`, `error`). |
| `log_format` | enum | `text` | `text` or `json`. JSON is useful for log aggregators. |
| `log_rate_limit_enabled` | bool | `1` | Enable per event rate limiting. |
| `log_rate_limit_window` | int (seconds) | `60` | Time window for the rate limit threshold. |
| `log_rate_limit_threshold` | int | `5` | Log every Nth occurrence of a repeated event. |
| `log_summary_enabled` | bool | `1` | Emit periodic operational summary (active publishers / players, connect counts, errors). |
| `log_summary_interval` | int (seconds) | `60` | Summary cadence. |
| `log_session_ids` | bool | `1` | Tag log lines with a short connection id like `[connection:a3f2]` for grep based correlation. |
| `log_level_connection` | enum | inherits `log_level` | Publisher / player connect and disconnect events. |
| `log_level_listener` | enum | inherits `log_level` | Listener lifecycle, accepts. |
| `log_level_stream` | enum | inherits `log_level` | Stream lifecycle (publishers). |
| `log_level_data` | enum | inherits `log_level` | Data path / packet trace. Very verbose. |
| `log_level_relay` | enum | inherits `log_level` | Puller and pusher operations. |
| `log_level_http` | enum | inherits `log_level` | HTTP API and webhooks. |
| `log_level_auth` | enum | inherits `log_level` | Authentication, player keys. |
| `log_level_system` | enum | inherits `log_level` | Startup, shutdown, config reload. |

#### Redaction and non blocking output

Every log line passes through one redacting sink before it reaches stdout or `log_file`, so credentials never leave the process, whichever call site printed them: values of `k`, `key`, `player_key` and `token` query parameters (also URL encoded), the value after the `cl_live_`, `cl_out_`, `hob_`, `pvw_`, `sbr_`, `opush_` and `ipush_` prefixes, the `play/r/<credential>` reader slot, the `/admission/<secret>` and `/sls/player-key/<secret>` path segments, quoted values after a word ending in `key`, and, as literals, the configured `on_event_url`, `player_key_auth_url` and `stat_post_url` paths, `api_keys` and `srt_passphrase`. Relay ids, app and stream names and peer addresses are kept.

Logging is asynchronous: lines are queued for background threads, and when a queue is full the oldest line is dropped and counted (`/stats` `logging.droppedMessages`). A stdout pipe nobody reads, a slow FIFO consumer or a hung disk therefore costs log lines, never media worker time. Security events (authorization and admission refusals, kicks, admin disconnects) use their own queue and are also counted exactly. Unauthenticated refusal lines share one budget (burst 50, then 20 lines/s); the excess is counted and summarised every 10 s. `SIGPIPE` is ignored, so a closed stdout does not stop the server. The operational queue holds 8192 lines; the `SLS_LOG_QUEUE_SIZE` environment variable (16 to 1048576) overrides it.

## Per server scope (`srt { server { ... } } }`)

### Listener ports

`listen_player`, `listen_publisher`, and `listen_publisher_srtla` each accept either a single port, a comma separated list, an inclusive range (`a-b`), or any mix. One SRT listener is created per port. SRTLA patches are enabled automatically on `listen_publisher_srtla` and disabled on `listen_publisher`.

| Directive | Type | Default | Notes |
|---|---|---|---|
| `listen_player` | port list | (required) | Player facing port(s). |
| `listen_publisher` | port list | (required for direct SRT publishers) | Direct SRT publisher port(s) (OBS, FFmpeg without SRTLA). |
| `listen_publisher_srtla` | port list | (required for SRTLA publishers) | SRTLA / bonded cellular publisher port(s) (used together with `srtla_rec`). |

### Latency and connection tuning

| Directive | Type | Default | Notes |
|---|---|---|---|
| `latency_min` | int (ms) | (per role default) | Minimum SRT latency for both publisher and player listeners. Floor enforced and warned when configured below the SRT default. |
| `latency_max` | int (ms) | (per role default) | Maximum latency for accepted connections. |
| `backlog` | int | `100` | Listen backlog for simultaneous connection attempts. |
| `idle_streams_timeout` | int (seconds) | `30` | Close streams idle for this long. `-1` disables. |
| `player_idle_streams_timeout` | int (seconds) | `0` (inherit `idle_streams_timeout`) | Idle timeout applied to player roles only. Set it higher than `idle_streams_timeout` so viewers ride out a publisher outage without being disconnected: when the publisher returns, surviving players re anchor at the live write head (with fresh PAT and PMT) instead of forcing every viewer client to reconnect. Publishers keep the shared `idle_streams_timeout`, so a dead publisher is still reaped promptly. `0` inherits `idle_streams_timeout`. `-1` disables the player idle reap entirely (viewers then only leave when their own socket dies or they disconnect). |
| `peer_idle_timeout` | int (ms) | `0` (libsrt default) | `SRTO_PEERIDLETIMEO` applied to every accepted socket. Bounds how long a peer may go silent (no data, no keepalive) before the link is declared broken. The belabox SRT fork raises the libsrt default so bonded cellular gaps do not kill healthy publishers; set a tighter value (for example `4000`) only when publishers reach SLS over a stable hop (a local `srtla_rec` terminating the bond). Avoid on direct bonded SRTLA listeners. |
| `publisher_first_data_grace` | int (ms) | `0` (built in `3000`) | First data probation for an accepted publisher. A connection that completes the SRT handshake, registers as a publisher, then never delivers a media packet (almost always a player or preview pointed at the ingest port, a valid stream key still authenticates) is reaped by `(negotiated latency + this grace)` instead of squatting the key for the full `idle_streams_timeout`. The latency term is required because SRT's TSBPD holds the first packet for the whole receive latency window, so a legitimate high latency encoder (Moblin around `3000`, Belabox `2000` to `3000`) surfaces its first byte only after that window. The grace covers RTT, encoder warmup, and TSBPD jitter on top of it. `0` applies the built in `3000`. `-1` disables probation. Works with the publisher takeover guard, which refuses a new connection for a key whose incumbent is actively delivering rather than evicting the live broadcaster. |

### Listener wide SRT encryption

These run during the SRT crypto handshake on every accepted connection (publishers and players). This is orthogonal to `player_key_auth_url`, which runs after the handshake on streamid lookup.

| Directive | Type | Default | Notes |
|---|---|---|---|
| `srt_passphrase` | string (10 to 79 bytes) | (unset, no encryption) | libsrt enforces this passphrase during the crypto handshake. Validated at listener start. |
| `srt_pbkeylen` | int (0, 16, 24, 32) | `0` (libsrt default, AES-128) | Key length in bytes. Validated at listener start. |

### Streamid routing

| Directive | Type | Default | Notes |
|---|---|---|---|
| `domain_player` | string | (required) | Domain segment used in the player streamid (`<domain_player>/<app>/<stream>`). |
| `domain_publisher` | string | (required) | Domain segment used in the publisher streamid. |
| `default_sid` | string | (unset) | Default streamid to use when the encoder omits one. |

The pair `domain_publisher` / `app_publisher` must not equal `domain_player` / `app_player` in the same server block.

### Webhooks

| Directive | Type | Default | Notes |
|---|---|---|---|
| `on_event_url` | url | (unset) | HTTP endpoint called on publisher and player connect / close. Callback shape: `?on_event=on_connect\|on_close&role_name=&srt_url=<stream_url>&remote_ip=&remote_port=`; player callbacks add `&session_id=<sessionId>`, the handle `/disconnect-player` takes. The publish auth response may include a `pushTargets` array that drives per publisher push destinations (see below). A player is held by its listener, unsent and outside any worker, until its `on_connect` call answers 200; the answer may carry `{"reserved": true}` (see `worker_reserved_connections`). A player refused at that point for the player limit gets `on_close`, because its `on_connect` was authorized. |
| `auth_reject_cache_ttl` | int (seconds) | `30` | Negative auth cache TTL. A publisher streamid whose `on_event_url` lookup returns non 200 is remembered for this many seconds; subsequent attempts with the same canonical streamid are rejected at the SRT handshake (no accept, no webhook). `0` falls back to the 30 second default. Mitigates streamid rotating DoS. |

### Pending players

With `on_event_url` set, an accepted player waits for its authorization. These caps bound the waiting players, since they never count against `max_players_per_stream`. A player over a cap is refused before any webhook call.

| Directive | Type | Default | Notes |
|---|---|---|---|
| `player_pending_max_per_stream` | int | `0` (built in `64`) | Players of one stream waiting for authorization. `-1` disables the cap. |
| `player_pending_max_per_ip` | int | `0` (no cap) | Players from one peer IP waiting for authorization, across all streams of the server block, plus that peer's player key validations in flight. Off by default because peers behind a shared address (a proxy, a NAT) would share it. |
| `player_pending_count_loopback` | bool | `0` | `0` exempts loopback peers (`127.0.0.0/8`, `::1`) from both caps, so on node readers such as previews are never refused by a flood; `1` counts them. |

### Player key authentication

When `player_key_auth_url` is set, player connections targeting the configured `domain_player` / `app_player` are validated against the API endpoint. The response maps the player key to the real stream id (and optionally to a per stream player limit). See [`PLAYER_KEY_IMPLEMENTATION.md`](PLAYER_KEY_IMPLEMENTATION.md) for the full protocol.

| Directive | Type | Default | Notes |
|---|---|---|---|
| `player_key_auth_url` | url | (unset, disabled) | API endpoint for validating player keys. `GET <url>?player_key=<key>`. Expected JSON response: `{"stream_id": "publish/live/actualstream", "max_players_per_stream": 10}` (the limit is optional). |
| `player_key_auth_timeout` | int (ms) | `2000` | HTTP timeout for the validation call. |
| `player_key_cache_duration` | int (ms) | `60000` | Duration to cache successful validations (and briefly cache rejections to prevent API abuse). |
| `player_key_rate_limit_requests` | int | `-1` (unlimited) | Max validation requests per peer IP per window. |
| `player_key_rate_limit_window` | int (ms) | `60000` | Rate limit window. |
| `player_key_min_length` | int | `8` | Minimum accepted player key length. |
| `player_key_max_length` | int | `64` | Maximum accepted player key length. Valid characters are printable ASCII (hex `0x20` to `0x7E`). |

## Per app scope (`srt { server { app { ... } } }`)

### Routing

| Directive | Type | Default | Notes |
|---|---|---|---|
| `app_player` | string | (required) | App segment used in the player streamid. |
| `app_publisher` | string | (required) | App segment used in the publisher streamid. |

### IP access control (allow / deny)

`allow` and `deny` directives gate connections per role.

| Directive | Type | Notes |
|---|---|---|
| `allow publish <ip\|all>` | rule | Whitelist for publishers. |
| `deny publish <ip>` | rule | Blacklist for publishers. |
| `allow play <ip\|all>` | rule | Whitelist for players. |
| `deny play <ip>` | rule | Blacklist for players. |

**Known IPv6 limitation.** The ACL storage type (`sls_ip_access_t`) holds only an IPv4 address. As a result, specific IPv6 allow or deny rules are not enforced today. IPv6 peers match only the wildcard entry (`all`) and otherwise hit the documented default (currently "accept by default", matching the IPv4 no match path); a one shot warning per call is logged so operators know that explicit IPv6 ACL entries are being ignored. Tracking item: add IPv6 ACL matching to `conf.cpp` and `SLSListenerHandler.cpp`. Until then, an explicit `deny play <ipv6>` rule will not take effect.

### Per stream limits

| Directive | Type | Default | Notes |
|---|---|---|---|
| `max_players_per_stream` | int | `-1` (unlimited) | Cap on simultaneous authorized players per stream. A player whose `on_event_url` authorization is still pending does not count (the pending caps above bound those); the cap is checked when the player connects and again, atomically, when its authorization resolves, and a player over the cap is refused then without receiving media. Previously pending players counted too, so a burst of unauthorized connections could fill the cap. Without `on_event_url` every player is authorized on connect and the behavior is unchanged. May be overridden per session by `max_players_per_stream` in a player key validation response. |

### In band timecode

Some encoders carry SMPTE timecode inside the video elementary stream: H.264 puts it in a `pic_timing` SEI (payload type 1, decodable only once the stream's SPS/VUI has been parsed, and only if that SPS sets `pic_struct_present_flag`), HEVC in a `time_code` SEI (payload type 136). With `timecode_sei true`, the publisher ingest path reads it and reports the last decoded value per stream under `timecode` in `/stats`:

```json
"timecode": {
  "valid": true,
  "timecode": "01:02:03:04",
  "dropFrame": false,
  "codec": "h264",
  "videoPid": 256,
  "pts": 90000,
  "updates": 149
}
```

`timecode` is `HH:MM:SS:FF`, with a `;` before the frames on a drop frame timecode. `pts` is the 90 kHz PTS of the access unit the timecode came from, so it can be lined up against the rest of the stream. `updates` counts timecodes decoded since the last `/stats` poll that passed `clear`. The whole object is absent unless the app opted in, and `valid` stays `false` on a stream whose encoder emits no timecode SEI — which is most of them, including the usual Belabox and moblin encoder paths. Turn it on when you know your encoder emits timecode.

| Directive | Type | Default | Notes |
|---|---|---|---|
| `timecode_sei` | bool | `false` | Read in band SMPTE timecode from publisher video SEI and report it in `/stats`. Costs one scan of each access unit's leading 8 KB, plus an 8 KB buffer per publishing stream. |

### Bitrate limiting

See [`BITRATE_LIMITING.md`](BITRATE_LIMITING.md) for the algorithm.

| Directive | Type | Default | Notes |
|---|---|---|---|
| `max_input_bitrate_kbps` | int | (unset, disabled) | Sustained publisher bitrate ceiling in kbps. |
| `max_input_bitrate_violation_timeout` | int (seconds) | `30` | Disconnect after this many seconds of sustained violation. |
| `max_input_bitrate_spike_tolerance` | int (percent) | `120` | Spike multiplier in percent (`120` means trigger at 1.2x of `max_input_bitrate_kbps`). |

### Push destinations

Push destinations let the publish auth webhook drive per publisher SRT pushers. When `push_destination_max > 0` and the publish auth response includes a JSON body `{ "pushTargets": [{ "url": "srt://..." }, ...] }`, SLS spawns one `CSLSPusher` caller per accepted entry. Each URL is validated again at use time. Aggregate egress per stream is bounded by `push_destination_max * max_input_bitrate_kbps`.

| Directive | Type | Default | Notes |
|---|---|---|---|
| `push_destination_max` | int | `0` (feature disabled) | Maximum number of push targets accepted per publisher. `0` disables the feature regardless of webhook output. |
| `push_destination_allow_internal` | bool | `false` | When `false`, RFC1918, loopback, and link local destinations are rejected. |
| `push_destination_allow_self` | bool | `false` | When `false`, the server's own bind addresses are rejected as destinations. |
| `push_destination_allow_schemes` | string list | `srt` | Allowed URL schemes for destinations. |
| `push_destination_max_url_len` | int (bytes) | `1024` | Maximum accepted destination URL length. |

### Relay (`relay { }` block)

The per app `relay { }` block configures static pull or push relays. The static relay code path remains in the codebase but is largely superseded for push by the dynamic `pushTargets` mechanism described above. The pull direction is still in active use.

| Block field | Type | Notes |
|---|---|---|
| `type` | enum | `pull` or `push`. |
| `mode` | enum | `pull`: `loop` or `hash`. `push`: `all` or `hash`. |
| `reconnect_interval` | int (seconds) | Reconnect delay on failure. |
| `idle_streams_timeout` | int (seconds) | Idle teardown timeout. `-1` disables. |
| `upstreams` | url list | Space separated list of upstream SRT URLs. Templates like `{stream_name}` are substituted. |

TODO: confirm whether the static `relay { type push; }` direction is still preferred over dynamic `pushTargets` for any operator workflow; cross reference the resolution from plan 007 once it lands.

## See also

- [`BITRATE_LIMITING.md`](BITRATE_LIMITING.md). Sustained vs spike tolerance, disconnect criteria.
- [`PLAYER_KEY_IMPLEMENTATION.md`](PLAYER_KEY_IMPLEMENTATION.md). Full player key auth protocol and response schema.
- [`CLAUDE.md`](CLAUDE.md). Orientation for contributors and agents (build layout, sanitizer guidance, commit conventions).
