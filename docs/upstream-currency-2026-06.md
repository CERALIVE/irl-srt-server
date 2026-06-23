# Upstream currency report — June 2026

Fork: `CERALIVE/irl-srt-server` branch `merge/upstream-2026-06`, HEAD `89d0f8b`.
Upstream tip absorbed: `a1dd80c` (`irlserver/main`, "Merge pull request #14 from irlserver/advisor/execute-all").
Prior merge-base: `682ac28`. New merge-base: `a1dd80c`.
Commits absorbed: 44 ADAPT out of 52 (8 DISCARD — see below).
Merge commit: `6386faa`. Regression fix on top: `89d0f8b`.

Triage source: `docs/upstream-sync-2026-06.md` (T4 classification).

---

## Improvements absorbed by category

### Security (8 commits)

| SHA | Subject | What it buys |
|-----|---------|--------------|
| `8e2b378` | fix(core): do not silently accept ipv6 peers in ip-acl checks | Closes an IP-ACL bypass: IPv6-mapped IPv4 addresses were passing ACL checks that should have blocked them. |
| `c960285` | fix(core): reject url-significant characters in streamid components | Rejects `/`, `\`, control bytes, and bare `.`/`..` in stream ID components at parse time, before any path resolution. Complements our existing streamid validator. |
| `0b78f0f` | fix(core): key auth-reject cache on canonical streamid | Auth-reject cache was keyed on the raw streamid string; a trivially mutated ID could bypass the cache. Now keyed on the canonical form. |
| `f80b32b` | fix(sls): constant-time api-key comparison for control endpoints | Replaces a naive string comparison on the HTTP control API key with a constant-time compare, closing a timing-attack channel. |
| `8a69b18` | fix(core): clamp sps/pps copy to ts_info buffer size | Bounds-checks the SPS/PPS copy into `ts_info` — previously could write past the buffer end on a malformed stream. |
| `90d8195` | fix(sls): bound and time-gate auth/rate-limit/player-key caches | All three caches now have a size ceiling and a TTL, preventing unbounded memory growth under a connection flood. |
| `efe2147` | feat(sls): handshake-time rejection for streamid dos | Rejects unknown or malformed stream IDs at the SRT handshake, before a full connection is established, cutting the cost of a stream-ID flood. |
| `148f1db` | Harden SLS auth and HTTPS callbacks | Tightens auth callback error paths and HTTPS webhook handling to avoid partial-auth states. |

### Performance (4 commits)

| SHA | Subject | What it buys |
|-----|---------|--------------|
| `a9faf6a` | perf(core): compile out per-packet spdlog::trace on data path | Removes a `spdlog::trace` call that was evaluated (and string-formatted) on every packet even when trace logging was disabled. Measurable reduction in data-path overhead at high bitrates. |
| `b5d6dfc` | perf(core): downgrade is_exist lock and drop per-packet string alloc | Downgrades a write lock to a read lock on the hot `is_exist` path and eliminates a per-packet `std::string` allocation. |
| `076a44b` | refactor(sls): event-driven egress, drop permanent SRT_EPOLL_OUT arm | Switches egress from a permanently armed `SRT_EPOLL_OUT` poll to an event-driven model: `SRT_EPOLL_OUT` is armed only when there is data to send, then disarmed. Eliminates busy-poll overhead when the send queue is empty. **Note: this commit introduced a regression on the `srt_client` push socket (see Regression section below).** |
| `30076a9` | fix(sls): throttle worker housekeeping to POLLING_TIME cadence | Worker housekeeping (stat collection, idle checks) was running on every poll iteration. Now gated to the `POLLING_TIME` cadence, reducing CPU burn on idle connections. |

### Portability (5 commits)

| SHA | Subject | What it buys |
|-----|---------|--------------|
| `854dd99` | build(sls): portable link libs for macos | Adds macOS-compatible link library flags so the server builds on Apple Silicon without manual CMake overrides. |
| `8e2f012` | fix(sls): qualify socket bind() to avoid std::bind clash on macos | `::bind()` was shadowed by `std::bind` on macOS, causing a compile error. Explicit `::bind()` qualification fixes it. |
| `6dc80c0` | fix(sls): portable pthread_t formatting in logs | `pthread_t` is not `unsigned long` on all platforms; uses `%p` cast to avoid UB and format-string warnings on non-Linux. |
| `305090e` | fix(sls): portable epoll wakeup via self-pipe on non-linux | `eventfd` is Linux-only. Replaces it with a self-pipe pair so the event loop wakeup works on macOS and other POSIX targets. |
| `87d86f4` | fix(core): use project strlcpy in SLSListenerConfig for glibc portability | `strlcpy` is absent from glibc. Uses the project's own `strlcpy` in `SLSListenerConfig.cpp`, reconciled with our existing `util.hpp` declaration. |

### Correctness and robustness (18 commits)

| SHA | Subject | What it buys |
|-----|---------|--------------|
| `212db67` | fix(core): correct ts file replay loop wrap and null guard | Off-by-one in the TS replay loop wrap and a missing null guard on the buffer pointer. |
| `fa42ecb` | fix(core): close socket and free addrinfo on libsrt_setup errors | Resource leak: on any error path in `libsrt_setup`, the SRT socket and `addrinfo` chain were not freed. |
| `f626d76` | fix(core): kick publisher via atomic flag in disconnect_stream | Race condition: `disconnect_stream` could race with the publisher thread. Now uses an atomic flag to signal the kick. |
| `9dd9a5f` | fix(sls): use erase-returns-next in reload-manager sweep | Iterator invalidation: the reload-manager sweep was calling `erase()` and then incrementing the invalidated iterator. |
| `6947bfb` | fix(core): race-free m_nDataCount, write-locked setSize, log fmt | Data race on `m_nDataCount` (now atomic) and missing write lock on `setSize`. |
| `9091f66` | fix(core): stop leaking stat_info_t on each accepted role | `stat_info_t` was heap-allocated on every accepted role and never freed. |
| `4a8fb10` | fix(core): null-check strdup(sid) on the accept path | `strdup` can return null under memory pressure; the accept path did not check. |
| `6de2c7c` | fix(core): negative-cache malformed player-key webhook responses | A malformed or unreachable webhook was not cached, so every subsequent connection attempt re-triggered the webhook call. Now negative-cached with a TTL. |
| `755c4bc` | fix(sls): improve latency handling based on listener role in SLSListener | Latency was applied uniformly regardless of listener role (publisher vs player). Now role-aware, preventing latency misconfiguration on mixed-role listeners. |
| `976c3d9` | feat(sls): deferred accept for async player-key validation | Player-key validation was blocking the accept loop. Now deferred: the connection is held in a pre-accept state while the webhook fires asynchronously. |
| `ec1aaa8` | feat(sls): pre-accept player gate + handoff backlog ceiling | Adds a pre-accept gate that limits the number of connections waiting for player-key validation, preventing backlog exhaustion. |
| `b5b7e5e` | fix(sls): non-blocking player-key validation (reject-and-retry) | Validation that previously blocked the worker thread now uses a reject-and-retry pattern so the worker stays responsive. |
| `2b4425b` | fix(sls): only negative-cache streamid on explicit auth reject | The negative cache was populated on any non-200 webhook response, including transient errors. Now only caches on an explicit auth reject (401/403). |
| `d848191` | fix(sls): trim whitespace/newlines from streamid components | Trailing whitespace or newlines in a stream ID component (e.g. from a misconfigured client) were passed through, causing lookup mismatches. |
| `db8ec6d` | fix(sls): check latency setsockopt returns in libsrt_setup | `srt_setsockopt` return values for latency options were ignored; errors now logged and surfaced. |
| `5a2abe1` | fix(sls): make publisher receive-latency floor explicit + warn below floor | Publisher receive latency had an implicit floor; values below it silently clamped. Now explicit with a warning log. |
| `e65d513` | feat(sls): publisher takeover on reconnect + optional peer-idle timeout | A reconnecting publisher can take over an active stream without dropping players. Adds an optional peer-idle timeout to evict stale connections. |
| `cc97465` | test(core): make auth-reject-cache timing tests deterministic | Timing-sensitive tests were flaky under load. Now use injected clocks for determinism. |

### Build, tooling, and CI (9 commits)

| SHA | Subject | What it buys |
|-----|---------|--------------|
| `2f82e23` | chore(build): add clang-format, clang-tidy and -Wextra -Wshadow | Adds `.clang-format`, clang-tidy integration, and `-Wextra -Wshadow` to the CMake build. Reconciled with our ADR-002 probe and Docker build-check. |
| `2dc0f38` | refactor(core): drop dead sls_format and av_* helpers in common.cpp | Removes unreachable `sls_format` and `av_*` helper functions from `common.cpp`. |
| `7d0f32f` | chore(deps): pin cpp-httplib and json submodules to release tags | `lib/cpp-httplib` pinned to `v0.48.0`, `lib/json` to `v3.12.0`. Reproducible builds. |
| `051153e` | build(docker): pin srt fork and alpine base to fixed refs | Alpine base image and the `irlserver/srt` belabox commit are now pinned in the `Dockerfile`. Reconciled to keep our belabox pin. |
| `5af0c62` | chore: ignore build-wsl/ build dir | Adds `build-wsl/` to `.gitignore`. Folded alongside our local-scratch entry. |
| `5afacf3` | ci(build): add github actions build + ctest + sanitizer matrix | Upstream's sanitizer/ctest CI matrix (ASan+UBSan, TSan). Folded alongside our Docker `build-check.yml` — both run. |
| `8b930b4` | test(core): add doctest harness and first unit tests | Doctest harness wired into CTest. Coexists with our `src/tests/test_conf_validation.cpp`. |
| `35967db` | update submodules | Bumps spdlog and json submodule pointers. |
| `67eef74` | docs: fix srt dependency and build prerequisites in readme | Corrects build prerequisite descriptions in `README.md`. Folded around our ADR-002 / stock-vs-patched-libsrt sections. |

---

## Regression found and fixed

`076a44b` (event-driven egress) introduced a regression: the `srt_client` push socket was not having `SRT_EPOLL_OUT` re-armed after the initial send, so the push path stalled after the first write. The server-side egress path was correct; only the client push socket was affected.

Fixed in `89d0f8b` (`fix(core): arm SRT_EPOLL_OUT on srt_client push socket`), which re-arms `SRT_EPOLL_OUT` on the push socket after each successful send, restoring the loopback push/play flow.

---

## Discards and rationale

8 commits were not absorbed. No real fix or feature was discarded.

| SHA | Subject | Why discarded |
|-----|---------|---------------|
| `a1dd80c` | Merge PR #14 advisor/execute-all | Merge bubble (two parents, topology only). Content classified via constituent commits. |
| `78d67c0` | Merge PR #13 fix/worker-egress-and-latency | Merge bubble (two parents, topology only). Content classified via constituent commits. |
| `bff489a` | docs(plans): add 000 macos plan, sync final status index | Upstream `plans/` scratch design docs. Not present in our fork tree; no behavior. |
| `47e03fb` | docs(plans): add architecture refactor design plans 008-012 | Upstream `plans/` scratch design docs. No behavior. |
| `baaa875` | docs(plans): add improve audit plans | Upstream `plans/` scratch design docs. No behavior. |
| `a7c358e` | docs: add claude.md | Upstream agent-routing file. Our routing surface is `AGENTS.md`; importing a second one creates a conflict with no benefit. |
| `96a49e8` | docs: backfill changelog with fork history to 3.1.0 | Our `ChangeLog.md` carries the v1.5.x lineage. The upstream 3.1.0 backfill is a different release history and would overwrite ours. |
| `8fd807c` | docs: add configuration.md cataloging irl directives | References companion docs absent from our fork (`AUDIO_GAP_FILLING.md`, `BITRATE_LIMITING.md`, `PLAYER_KEY_IMPLEMENTATION.md`) and does not reflect our ADR-002 stock-libsrt path. Importing it yields dangling links and directives we document differently. |

---

## Divergences preserved

These are CERALIVE-specific additions that were kept intact through the merge:

1. **ADR-002 `SLS_HAVE_SRTO_SRTLAPATCHES` CMake probe** in `CMakeLists.txt` and the stock-libsrt fallback path in `src/core/SLSSrt.cpp` (`SRTO_NAKREPORT=0` + `SRTO_LOSSMAXTTL=30`). The patched fork remains optional.
2. **"SRT compat mode" logging** in `CSLSSrt::libsrt_setup` (`srtlapatches` vs `standard-options`). Emitted per SRTLA listener at startup.
3. **Multi-listen-port** parsing and the config validator (`src/core/conf.cpp`, `conf.hpp`, `src/tests/test_conf_validation.cpp`). Upstream does not carry this feature.
4. **Docker-based build-check** (`.github/workflows/build-check.yml`) running `docker build` on amd64 + arm64. Upstream's sanitizer/ctest matrix runs alongside it, not instead of it.
5. **Explicit `util.hpp` / `strlcpy` include** in `src/core/util.hpp`. Upstream's `87d86f4` project-strlcpy call site was reconciled against our declaration.
6. **`AGENTS.md`** as the agent-routing surface. Upstream's `CLAUDE.md` (`a7c358e`) was discarded.
7. **`ChangeLog.md`** carrying the v1.5.x lineage. Upstream's 3.1.0 backfill (`96a49e8`) was discarded.

---

## Validation evidence

The following checks were run on `merge/upstream-2026-06` at HEAD `89d0f8b`. This is the T8 gate.

| Check | Result |
|-------|--------|
| `docker build .` (amd64) | Green. Image builds against `irlserver/srt@belabox`. |
| Stock-libsrt compile | Green. `SLS_HAVE_SRTO_SRTLAPATCHES=0` path compiles cleanly against Haivision/srt. |
| Config-validator (doctest / CTest) | Pass. Port-list parser and streamid safety checks pass. |
| `srt_client` loopback push/play | Pass. ~534 KB transferred end-to-end. |
| SRT compat mode log line | `SRT compat mode: srtlapatches (patched libsrt).` confirmed in startup log. |

**Scope of "no regression" claim:** the above five checks only. Bonded multi-link throughput, real-network latency, and SRTLA receiver interop under packet loss were not tested in this gate. Those scenarios require a live bonded setup and are deferred to hardware validation. Do not read the T8 gate result as coverage of bonded multi-link behavior.

---

## What is not yet tested (deferred)

- **Bonded multi-link throughput:** requires a live SRTLA sender + receiver pair across multiple network interfaces. Not run.
- **Real-network latency and jitter:** the loopback test uses localhost; no WAN or cellular path was exercised.
- **SRTLA receiver interop under packet loss:** the compat-mode ADR-002 evaluation covered reorder stress, but that was a prior standalone test, not re-run against this merge.
- **Publisher takeover under load** (`e65d513`): the feature was absorbed but not exercised with a live reconnect scenario.
- **Peer-idle timeout eviction** (`e65d513`): same — absorbed, not exercised.

These are required before a production deployment of this branch. They are not blockers for the PR gate, which is defined as T8 only.
