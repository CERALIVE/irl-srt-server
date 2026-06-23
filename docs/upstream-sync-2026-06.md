# Upstream catch-up triage — June 2026

Read-only analysis. This document classifies every commit upstream
`irlserver/irl-srt-server` is ahead of our `master`, so the merge (a separate
task) can be executed deliberately. **This is a plan, not the merge.**

## Range

- **Our fork:** `CERALIVE/irl-srt-server` branch `master`, HEAD `78107a6`.
- **Upstream tip:** `a1dd80c` (`irlserver/main`, "Merge pull request #14 from irlserver/advisor/execute-all").
- **Merge-base:** `682ac28`.
- **Commits to absorb:** `git rev-list --count 682ac28..a1dd80c` = **52**.

Classification key:

- **ADAPT** — take the fix/feature; fold it around our divergences. A security,
  performance, correctness, portability, or feature commit is always ADAPT.
- **DISCARD** — upstream-only noise (scratch design docs, a competing agent-routing
  file, a changelog backfill, or a topology-only merge bubble). No real fix is ever
  DISCARDed; every DISCARD is justified inline.

## Our divergences to preserve (never regress these)

1. **ADR-002 `SLS_HAVE_SRTO_SRTLAPATCHES`** CMake probe + stock-libsrt path in
   `src/core/SLSSrt.cpp` (`SRTO_NAKREPORT=0` + `SRTO_LOSSMAXTTL=30` fallback).
2. **"SRT compat mode" logging** (`srtlapatches` vs `standard-options`) in
   `CSLSSrt::libsrt_setup`.
3. **Multi-listen-port** parsing/feature and its config validator
   (`src/core/conf.cpp` / `conf.hpp`, `src/tests/test_conf_validation.cpp`).
4. **Docker-based build-check** (`.github/workflows/build-check.yml`) and the
   `Dockerfile` pinned to `irlserver/srt@belabox`.
5. **Explicit `util.hpp` / `strlcpy` include** (our `src/core/util.hpp` already
   declares `strlcpy`).
6. **`AGENTS.md`** as our agent-routing surface (we do NOT carry upstream `CLAUDE.md`).
7. **Our `ChangeLog.md`** (v1.5.x lineage) and **`src/CMakeLists.txt`** (ours-only).
8. Local-scratch `.gitignore` entry.

---

## Commit classification (52)

Listed in `git log` order (upstream tip → merge-base). Hashes are upstream SHAs,
all reachable in local objects.

| # | SHA | Subject | Class | Reason |
|---|-----|---------|-------|--------|
| 1 | `a1dd80c` | Merge PR #14 advisor/execute-all | **DISCARD** | Merge bubble (2 parents); topology only — its content is classified via the constituent commits below. A true-merge replays diffs, not this bubble. |
| 2 | `87d86f4` | fix(core): project strlcpy in SLSListenerConfig for glibc portability | **ADAPT** | Portability fix. **Conflict file.** Reconcile with our existing `util.hpp`/`strlcpy` divergence (keep ours; fold the project-`strlcpy` call site in `SLSListenerConfig.cpp`, plus upstream's `#include <string>` + `url_encode(const std::string&)` qualification). |
| 3 | `bff489a` | docs(plans): add 000 macos plan, sync final status index | **DISCARD** | Upstream `plans/` scratch design docs (root `plans/`, absent in our fork). Not code. |
| 4 | `47e03fb` | docs(plans): add architecture refactor design plans 008-012 | **DISCARD** | Upstream `plans/` scratch design docs; no behavior. |
| 5 | `a7c358e` | docs: add claude.md | **DISCARD** | Upstream `CLAUDE.md` agent-routing file. Our routing surface is `AGENTS.md`; importing a second one is noise/conflict. |
| 6 | `96a49e8` | docs: backfill changelog with fork history to 3.1.0 | **DISCARD** | **Keep ours.** Our `ChangeLog.md` carries the v1.5.x lineage; reject the upstream 3.1.0 backfill hunk — do not import upstream's release history. |
| 7 | `8fd807c` | docs: add configuration.md cataloging irl directives | **DISCARD** | Upstream-only doc tree. References companion docs absent from our fork (`AUDIO_GAP_FILLING.md`, `BITRATE_LIMITING.md`, `PLAYER_KEY_IMPLEMENTATION.md`) and does **not** reflect our ADR-002 stock-libsrt path. Importing it yields dangling links and directives we document differently. Decision made explicitly per task: does not match our fork's directive set incl. ADR-002 → DISCARD. |
| 8 | `67eef74` | docs: fix srt dependency and build prerequisites in readme | **ADAPT** | `README.md` is a **conflict file**. Fold upstream's prerequisite corrections around our ADR-002 / stock-vs-patched-libsrt + Docker narrative — keep our SRT-dependency section. |
| 9 | `2f82e23` | chore(build): add clang-format, clang-tidy and -Wextra -Wshadow | **ADAPT** | Tooling/build hardening. Touches **conflict files** `.clang-format` + `CMakeLists.txt`: reconcile `.clang-format`, fold clang-tidy/`-Wextra`/`-Wshadow` into our CMake on top of the ADR-002 probe + Docker build-check. |
| 10 | `2dc0f38` | refactor(core): drop dead sls_format and av_* helpers in common.cpp | **ADAPT** | Dead-code removal; safe, no divergence overlap. |
| 11 | `7d0f32f` | chore(deps): pin cpp-httplib and json submodules to release tags | **ADAPT** | Submodule pin hygiene; reconcile `.gitmodules`/submodule refs (we carry `lib/json`). |
| 12 | `051153e` | build(docker): pin srt fork and alpine base to fixed refs | **ADAPT** | `Dockerfile` is a **conflict file**. Keep our `irlserver/srt@belabox` pin; fold upstream's Alpine-base/ref pinning hygiene. |
| 13 | `a9faf6a` | perf(core): compile out per-packet spdlog::trace on data path | **ADAPT** | Performance — hot data path. |
| 14 | `b5d6dfc` | perf(core): downgrade is_exist lock and drop per-packet string alloc | **ADAPT** | Performance — lock contention + per-packet alloc. |
| 15 | `212db67` | fix(core): correct ts file replay loop wrap and null guard | **ADAPT** | Correctness + null-guard fix. |
| 16 | `cc97465` | test(core): make auth-reject-cache timing tests deterministic | **ADAPT** | Test reliability; pairs with the hardening commits and our test discipline. |
| 17 | `fa42ecb` | fix(core): close socket and free addrinfo on libsrt_setup errors | **ADAPT** | Resource-leak fix. Touches `SLSSrt.cpp` (**conflict file**) — keep ADR-002 + compat-mode logging, fold the leak fix. |
| 18 | `8e2b378` | fix(core): do not silently accept ipv6 peers in ip-acl checks | **ADAPT** | **Security** — IP-ACL bypass fix. Never discard. |
| 19 | `f626d76` | fix(core): kick publisher via atomic flag in disconnect_stream | **ADAPT** | Race-condition fix. |
| 20 | `9dd9a5f` | fix(sls): use erase-returns-next in reload-manager sweep | **ADAPT** | Iterator-invalidation correctness fix. |
| 21 | `6947bfb` | fix(core): race-free m_nDataCount, write-locked setSize, log fmt | **ADAPT** | Data-race fix. |
| 22 | `9091f66` | fix(core): stop leaking stat_info_t on each accepted role | **ADAPT** | Memory-leak fix. |
| 23 | `c960285` | fix(core): reject url-significant characters in streamid components | **ADAPT** | **Security** — complements our streamid validation; reconcile with our validator. |
| 24 | `0b78f0f` | fix(core): key auth-reject cache on canonical streamid | **ADAPT** | Security/correctness — cache-key fix. |
| 25 | `4a8fb10` | fix(core): null-check strdup(sid) on the accept path | **ADAPT** | Null-deref fix on accept path. |
| 26 | `f80b32b` | fix(sls): constant-time api-key comparison for control endpoints | **ADAPT** | **Security** — timing-attack mitigation. |
| 27 | `6de2c7c` | fix(core): negative-cache malformed player-key webhook responses | **ADAPT** | Robustness/DoS hardening. |
| 28 | `8a69b18` | fix(core): clamp sps/pps copy to ts_info buffer size | **ADAPT** | **Security** — buffer-overflow clamp. |
| 29 | `5afacf3` | ci(build): add github actions build + ctest + sanitizer matrix | **ADAPT** | CI. **Do not drop our Docker `build-check.yml`** — fold upstream's sanitizer/ctest matrix alongside it. |
| 30 | `8b930b4` | test(core): add doctest harness and first unit tests | **ADAPT** | Test harness; must coexist with our `src/tests/test_conf_validation.cpp`. |
| 31 | `854dd99` | build(sls): portable link libs for macos | **ADAPT** | Portability. Touches CMake link libs; reconcile against our ours-only `src/CMakeLists.txt`. |
| 32 | `8e2f012` | fix(sls): qualify socket bind() to avoid std::bind clash on macos | **ADAPT** | Portability/correctness. |
| 33 | `6dc80c0` | fix(sls): portable pthread_t formatting in logs | **ADAPT** | Portability. |
| 34 | `305090e` | fix(sls): portable epoll wakeup via self-pipe on non-linux | **ADAPT** | Portability — non-Linux event loop. |
| 35 | `baaa875` | docs(plans): add improve audit plans | **DISCARD** | Upstream `plans/` scratch docs; no behavior. |
| 36 | `78d67c0` | Merge PR #13 fix/worker-egress-and-latency | **DISCARD** | Merge bubble (2 parents); topology only — content classified via constituents below. |
| 37 | `755c4bc` | fix(sls): improve latency handling based on listener role in SLSListener | **ADAPT** | Latency/correctness. Touches `SLSListener` — preserve our multi-listen-port handling while folding the role-aware latency logic. |
| 38 | `976c3d9` | feat(sls): deferred accept for async player-key validation | **ADAPT** | Feature — async player-key accept. |
| 39 | `5af0c62` | chore: ignore build-wsl/ build dir | **ADAPT** | `.gitignore` is a **conflict file**. Fold the `build-wsl/` ignore; keep our local-scratch entry. |
| 40 | `ec1aaa8` | feat(sls): pre-accept player gate + handoff backlog ceiling | **ADAPT** | Feature — pre-accept gate + backlog ceiling (DoS-relevant). |
| 41 | `b5b7e5e` | fix(sls): non-blocking player-key validation (reject-and-retry) | **ADAPT** | Feature/correctness — non-blocking validation. |
| 42 | `90d8195` | fix(sls): bound and time-gate auth/rate-limit/player-key caches | **ADAPT** | **Security/DoS** — bounded, time-gated caches. |
| 43 | `148f1db` | Harden SLS auth and HTTPS callbacks | **ADAPT** | **Security** hardening (single-parent real commit, not a merge — verified). |
| 44 | `2b4425b` | fix(sls): only negative-cache streamid on explicit auth reject | **ADAPT** | Correctness — cache-poisoning avoidance. |
| 45 | `efe2147` | feat(sls): handshake-time rejection for streamid dos | **ADAPT** | **Security** — handshake-time DoS rejection. |
| 46 | `d848191` | fix(sls): trim whitespace/newlines from streamid components | **ADAPT** | Input-validation fix; reconcile with our streamid validator. |
| 47 | `db8ec6d` | fix(sls): check latency setsockopt returns in libsrt_setup | **ADAPT** | Error-check fix. Touches `libsrt_setup` / `SLSSrt.cpp` (**conflict file**) — keep ADR-002 path + compat-mode logging. |
| 48 | `35967db` | update submodules | **ADAPT** | Submodule-pointer bump (spdlog/json). Reconcile to keep submodules current; verify no regression of our pins at merge time. |
| 49 | `5a2abe1` | fix(sls): make publisher receive-latency floor explicit + warn below floor | **ADAPT** | Correctness/feature — explicit latency floor + warning. |
| 50 | `076a44b` | refactor(sls): event-driven egress, drop permanent SRT_EPOLL_OUT arm | **ADAPT** | Performance/refactor — event-driven egress. |
| 51 | `30076a9` | fix(sls): throttle worker housekeeping to POLLING_TIME cadence | **ADAPT** | Performance — housekeeping cadence. |
| 52 | `e65d513` | feat(sls): publisher takeover on reconnect + optional peer-idle timeout | **ADAPT** | Feature — publisher takeover + peer-idle timeout. |

**Tally:** 44 ADAPT, 8 DISCARD = 52. No commit unclassified.

DISCARD set (8): `a1dd80c`, `78d67c0` (merge bubbles), `bff489a`, `47e03fb`,
`baaa875` (upstream `plans/` scratch), `a7c358e` (`CLAUDE.md`), `96a49e8`
(changelog backfill — keep ours), `8fd807c` (`CONFIGURATION.md` — upstream-only doc
tree, no ADR-002).

---

## Conflict files — resolution intent (8)

The actual overlap was re-derived, not assumed:

```
comm -12 <(git diff 682ac28 HEAD --name-only | sort) \
         <(git diff 682ac28 a1dd80c --name-only | sort)
```

yields exactly these 8 files. **Re-derive this at merge time** — the list is a
guide, not a contract.

| File | Resolution intent |
|------|-------------------|
| `.clang-format` | **Reconcile both.** Take upstream's style additions; keep any of our local format choices. One unified `.clang-format`. |
| `CMakeLists.txt` | **Keep our ADR-002 probe (`SLS_HAVE_SRTO_SRTLAPATCHES`) + Docker build-check wiring; fold upstream's clang-tidy + `-Wextra` + `-Wshadow`** (`2f82e23`). Do not let upstream's CMake clobber the probe. |
| `Dockerfile` | **Adapt.** Keep our `irlserver/srt@belabox` pin and build-check assumptions; fold upstream's pinned Alpine base / fixed refs (`051153e`). |
| `.gitignore` | **Adapt.** Fold upstream's `build-wsl/` / `build-dir` ignores (`5af0c62`); keep our local-scratch entry. |
| `README.md` | **Adapt.** Keep our SRT-dependency / ADR-002 / stock-vs-patched + Docker sections; fold upstream's build-prerequisite corrections (`67eef74`). |
| `src/core/SLSListenerConfig.cpp` | **Reconcile our `util.hpp` / `strlcpy` include with upstream's project-`strlcpy` (`87d86f4`).** Keep our declaration; adopt the project-`strlcpy` call site + `url_encode(const std::string&)` qualification. |
| `src/core/SLSSrt.cpp` | **Keep ADR-002 path + "SRT compat mode" logging; fold upstream correctness fixes** (`fa42ecb` addrinfo/socket cleanup, `db8ec6d` setsockopt return checks). The stock-libsrt branch and compat-mode log lines are non-negotiable. |
| `src/core/TCPRole.cpp` | **Adapt.** Fold upstream's correctness/portability changes; no known divergence in this file, so a straight reconcile of overlapping hunks. |

**Not in the overlap (verify at merge time):**

- `src/CMakeLists.txt` — **ours-only.** It is in our `682ac28..HEAD` diff but NOT
  in `682ac28..a1dd80c`, so it is not a conflict file. Upstream's macOS link-lib /
  portability commits (`854dd99`, `5afacf3`) may still touch CMake elsewhere —
  re-run the `comm -12` derivation at merge time to confirm it stays ours-only.
- Our other ours-only files (`AGENTS.md`, `.github/dependabot.yml`,
  `.github/workflows/build-check.yml`, `src/core/conf.cpp`, `src/core/conf.hpp`,
  `src/tests/test_conf_validation.cpp`, `tests/e2e/*`) are untouched upstream and
  carry through cleanly.

---

## Execution note

This document is the triage only. The merge itself (add the `irlserver` remote
transiently, `git merge` per the root policy, resolve the 8 conflict files per the
table above, remove the remote before pushing) is a separate task. At that point,
**re-derive the conflict set** with the `comm -12` command above — upstream or our
branch may have moved.
