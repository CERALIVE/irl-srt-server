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
