# Release Notes

## Unreleased

### Security

- **HTTP control plane defaults to loopback (BREAKING).** The stats / disconnect
  API now binds to `127.0.0.1` by default instead of all interfaces. Add
  `http_bind_addr 0.0.0.0;` (IPv4) or `http_bind_addr ::;` (IPv4+IPv6) to the
  `srt { ... }` block to restore remote access, and pair it with `api_keys`.
- **`/stats` requires an API key for every read.** The `?publisher=<name>` query
  was previously unauthenticated and leaked per-stream statistics; it is now
  gated behind `api_keys` like the list-all and `reset` paths. Configure
  `api_keys` and send `Authorization: <key>` to keep scraping `/stats`.
- **CORS is off by default.** `cors_header` now defaults to empty (no
  `Access-Control-Allow-Origin` header). Set `cors_header <origin>;` to opt in.

### v3.1.0 (in progress)

**SRTLA / bonded cellular**

- Listener ports select CERALIVE receive profiles: `listen_publisher_srtla` is L1 (freeze, NAK on), `listen_publisher_srtla_classic` is L2 (freeze, NAK off), and direct/player listeners use L3. L1/L2 retain `lossmaxttl=40`. Multiple ports per role use comma lists and inclusive ranges (`listen_player 4000,4010,5000-5005`).

**Player key authentication**

- Per stream player keys validated against an external HTTP endpoint with rate limiting, length and character validation, success and failure caching, configurable cache duration and timeouts.
- Per session `max_players_per_stream` override returned in the validation response.
- Non blocking player key validation: deferred accept lets the worker keep serving traffic while the HTTP lookup is in flight. Reject and retry path for transient validation failures.

**Push destinations from publish auth webhook**

- The publish auth webhook may return a `pushTargets` array. SLS spawns one dynamic `CSLSPusher` per accepted entry, bounded by the new app level knobs (`push_destination_max`, `push_destination_allow_internal`, `push_destination_allow_self`, `push_destination_allow_schemes`, `push_destination_max_url_len`).
- URL validation includes DNS resolution and self bind detection.

**Bitrate limiting**

- `max_input_bitrate_kbps` enforces a publisher ceiling with configurable spike tolerance (`max_input_bitrate_spike_tolerance`) and sustained violation timeout (`max_input_bitrate_violation_timeout`).
- Publishers that sustain violations past the timeout are disconnected.

**Audio gap filling (CERALIVE retained)**

- CERALIVE retains `audio_gap_fill`, transport-level silent frame insertion, and the per-publisher `audioGapFill` stats object. Upstream's removal is not adopted by this integration.

**In band timecode**

- New `timecode_sei` app directive reads SMPTE timecode out of the publisher's video elementary stream (H.264 `pic_timing` SEI, HEVC `time_code` SEI) and reports the last decoded value per stream under `timecode` in `/stats`, alongside its 90 kHz PTS. Off by default; parses the leading 8 KB of each access unit when on.

**HTTP stats and control API**

- New `/healthz` endpoint for Kubernetes probes.
- New disconnect stream endpoint with API key authorization.
- `api_keys` directive supports comma separated keys with constant time comparison.
- Atomic stats counters and a read locked `put()` so `/stats` no longer stalls the data path.
- Replaced the blocking HTTP client with an `AsyncHttpClient` (thread pool backed) for stats posting, player key validation, and `on_event_url` callbacks.
- New per stream `/stats` diagnostics for the jump/replay class of viewer issue: `maxReaderBacklogBytes` and `maxReaderBacklogMs` (furthest any viewer fell behind the ring write head, the size of a potential catch up burst) alongside the existing `ringOverruns`.
- Upstream removes unused synthesized PAT/PMT/SPS-PPS bootstrap injection machinery. CERALIVE still parses transport streams where required for retained audio gap filling and optional timecode extraction.
- Fixed `sendBackpressure`, which was read off the publisher role (which never runs the egress write path) and so was structurally always zero. It is now aggregated across a stream's viewers via the shared ring, so it reflects real viewer backpressure.
- Teardown log lines (`check_invalid_sock`, `get_state`) now carry `stream=<name>` for grep based correlation across many concurrent streams, and a flight recorder line at publisher teardown records the session's peak backlog, overruns, and viewer backpressure before the ring is freed (survives publisher reconnect).

**Logging redesign**

- Per category log levels (`log_level_connection`, `log_level_listener`, `log_level_stream`, `log_level_data`, `log_level_relay`, `log_level_http`, `log_level_auth`, `log_level_system`).
- Rate limited repetitive events (`log_rate_limit_*`).
- Periodic operational summary (`log_summary_*`).
- Session id tagging on log lines for grep based correlation (`log_session_ids`).
- Optional JSON file sink (`log_format json`) for log aggregators.

**Security and hardening**

- Listener wide SRT encryption via `srt_passphrase` and `srt_pbkeylen`, validated at listener start.
- Handshake time rejection for streamid based DoS (`auth_reject_cache_ttl` negative cache on the canonical streamid).
- Streamid sanitization: reject unsafe characters in host, app, and stream components; trim whitespace; reject URL significant characters.
- Bounded and time gated auth, rate limit, and player key caches.
- Constant time API key comparison on control endpoints.
- Drop privileges after binding via `user` and `group` directives.
- Numeric config values validated through `strtol` / `strtod`.
- Async signal safe signal handlers; SIGTERM handling.
- `peer_idle_timeout` (`SRTO_PEERIDLETIMEO`) per accepted socket.

**Stability and performance**

- `player_idle_streams_timeout` lets players survive publisher outages independently of the publisher idle timeout (`0` inherits, `-1` disables player idle reaping). `publisher_first_data_grace` bounds first-data probation by negotiated receive latency plus a grace interval.
- Publisher takeover on reconnect.
- Event driven worker (`eventfd`, no polling `msleep` on Linux), portable `epoll` wakeup via self pipe on non Linux.
- Event driven egress, drop the permanent `SRT_EPOLL_OUT` arm.
- Player latency clamped via `SRTO_PEERLATENCY`; bumped UDP buffers; explicit `SRTO_TLPKTDROP`.
- Disconnect viewers stuck in continuous backpressure; handle `EASYNCSND` backpressure without disconnecting healthy viewers.
- Race free `m_nDataCount`, write locked `setSize` on the publisher ring; size publisher ring per bitrate with reader overrun detection.
- Egress tuned to stop live viewers replaying stale video. The publisher ring is sized at 1x the latency window instead of 2x, and `MAX_EGRESS_BATCHES` is reduced from 8 to 2, so a viewer that falls behind is skipped forward per packet by SRT `TLPKTDROP` at the socket rather than catching up in a large burst of old frames (which a viewer perceives as a jump back then skip forward). The ring is treated as a small hand off buffer, not a jitter buffer; the SRT socket holds each viewer's latency window.
- Compile out per packet `SPDLOG_TRACE` / `SPDLOG_DEBUG` on the data path; drop per packet string allocations.
- Race and leak fixes on the accept path (close socket and free `addrinfo` on `libsrt_setup` errors; stop leaking `stat_info_t` per accepted role; null check `strdup(sid)`; kick publisher via atomic flag in `disconnect_stream`).
- Removed HLS recording.

**Portability**

- Build restored on macOS (portable `pthread_t` formatting, portable link libs, qualified `socket::bind`).

**Tooling**

- doctest based unit test harness wired into CTest, retaining CERALIVE profile/listener and audio gap tests alongside new idle, timecode, and log-rate-limiter tests. The CI matrix retains canonical debug/ASan+UBSan/TSan and stock-libsrt test legs plus a compile-only legacy BELABOX leg. Four libFuzzer targets include the timecode scanner; stats snapshot and reconnect-replay e2e gates run on the canonical debug leg.
- `clang-format` and `clang-tidy` configs; `-Wextra -Wshadow` warnings enabled.
- Pinned vendored submodules (cpp-httplib `v0.48.0`, json `v3.12.0`, spdlog tracks the `irlserver/spdlog` fork). The canonical Dockerfile retains `CERALIVE/srt` at `b06fdb6` (`1.5.6+ceralive.1`), with `SLS_BUILD_TESTS=ON`, CTest and SRT loopback gates.

**IP-ACL**

- IPv6 peers no longer silently bypass IPv4 allow / deny rules. They are matched only against the wildcard entry and otherwise hit the documented default with a one shot warning per call. Specific IPv6 ACL entries are still not supported.

### v3.0.0

- Switched to git submodules for vendored libraries (`spdlog`, `cpp-httplib`, `nlohmann/json`, `thread-pool`, `CxxUrl`).

### v2.5.0

- Per stream player limit (`max_players_per_stream`) with per session override returned by the player key validation API.
- Initial player key validation against an external API endpoint.
- Initial bitrate limiting feature for SRT stream input with configurable violation timeout.
- Disconnect stream endpoint, gated by API key authorization.
- SIGTERM signal handling.
- Refactored `SLSListener` into modular components (`SLSListenerCore`, `SLSListenerAuth`, `SLSListenerConfig`, `SLSListenerHandler`).
- Separate publisher and player listen ports (`listen_publisher`, `listen_player`).
- Dynamic latency, additional listener and worker logging, configuration file search in standard paths.

### v2.4.1

- API key support for control endpoints.

### v2.4.0

- Enabled `SRTO_SRTLAPATCHES` for SRTLA / bonded cellular compatibility.

### v2.3.2

- Stats: always emit the publishers array when status is ok; initialize the SRT stats object before requesting stats.

### v2.3.1

- Increased `POLLING_TIME` to reduce CPU usage.

### v2.3.0

- Switched the SRT dependency to the `irlserver/srt` fork (belabox patches).
- Lowered SRT flight credit constant.

### v2.2.0 (security update)

- Initial IPv6 support on listener sockets.
- SRT socket option tweaks.
- Small JSON API fixes; lower `TS_UDP_LEN` log severity to trace.

## Upstream history (`rstular/srt-live-server`)

## v1.5.1

- Fixed JSON status callback - information is now being sent via HTTP to the endpoint specified in the configuration file.
- Use proper JSON encoding library.

## v1.5.0

- Added `allow` and `deny` directives, to enable simple access control (see [Directives](https://github.com/rstular/srt-live-server/wiki/Directives) for more info).
- Added `pidfile` directive (see [Directives](https://github.com/rstular/srt-live-server/wiki/Directives) for more info).
- Bug fixes.

## v1.4.9

- Compatibility with Raspberry Pi.

## v1.4.8

- Compatibility with `srt v1.4.1`, add the set latency method before setup method.

## v1.4.7

- update the PID file path from `/opt/soft/sls/` to `/tmp/sls` to avoid the root authority in some case.

## v1.4.6

- update the PID file path from `~/` to `/opt/soft/sls/`

## v1.4.5

- add HLS record feature.

## v1.4.4

- OBS streaming compatible, OBS support the srt protocol which is later than v25.0. (https://obsproject.com/forum/threads/obs-studio-25-0-release-candidate.116067/)

## v1.4.3

- change the TCP epoll mode to select mode for compatibility with MacOS.
- modify the HTTP check repeat bug for reopen.

## v1.4.2

- add remote_ip and remote_port to on_event_url which can be as the unique identification for player or publisher.

## v1.4.1

- add publisher feather to slc(srt-live-client) tool, which can push ts file with srt according dts.
- modify the HTTP bug when host is not available.

## v1.4

- add HTTP statistic info.
- add HTTP event notification, on_connect, on_close.
- add player feature to slc(srt-live-client) tool for pressure test.

## v1.3

- support reload.
- add idle_streams_timeout feature for relay.
- change license type from gpl to mit.

## v1.2.1

- support hostname:port/app in upstreams of pull and push.

## v1.2

- update the memory mode, in v1.1 which is publisher copy data to eacc player, in v1.2 each publisher put data to a array and all players read data from this array.
- update the relation of the publisher and player, the player is not a member of publisher. the only relation of them is array data.
- add push and pull features, support all and hash mode for push, support loop and hash for pull. in cluster mode, you can push a stream to a hash node, and pull this stream from the same hash node.
