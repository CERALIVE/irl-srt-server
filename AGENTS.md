# irl-srt-server

Fork of [irlserver/irl-srt-server](https://github.com/irlserver/irl-srt-server). C++17/CMake. Receives the bonded SRT stream from the device side and re-serves it for downstream consumers.

Parent manifest: [`../AGENTS.md`](../AGENTS.md)

---

## ROLE IN THE GROUP

```
srtla (device, bond) ──▶ irl-srt-server ──▶ ceralive-platform (ingest)
```

- Sits at the cloud edge, accepting the SRTLA-bonded stream from the device.
- Re-serves the stream over SRT so ceralive-platform can pull it.
- No encoding, no transcoding — pure SRT relay with MPEG-TS payload.

---

## STACK

| Layer | Detail |
|-------|--------|
| Language | C++17 |
| Build | CMake 3.5+, outputs to `build/bin/` |
| SRT transport | System-installed libsrt (`-lsrt`); builds against **either** the BELABOX-patched [`irlserver/srt`](https://github.com/irlserver/srt) `belabox` branch (defines `SRTO_SRTLAPATCHES`) **or** stock [Haivision/srt](https://github.com/Haivision/srt). A CMake probe selects the path automatically; the patched fork is now OPTIONAL. No srt submodule |
| Submodules | `lib/spdlog` (irlserver/spdlog 1.9.2), `lib/json` (nlohmann/json) |
| Config | `sls.conf` — domain/app/stream routing, publisher vs player separation |

---

## SRT DEPENDENCY

`irl-srt-server` has no `srt` submodule. `.gitmodules` contains only `lib/spdlog` and `lib/json`. `src/CMakeLists.txt` links with `-lsrt` directly, so system-installed libsrt must be present before building.

**The canonical build uses `CERALIVE/srt` @ `reorderfreeze-1.5.5` (SHA `66b3609`).** The Dockerfile clones `https://github.com/CERALIVE/srt.git` and checks out that branch. This is a clean reset to Haivision v1.5.5 (`1e4c908`) plus the single sanctioned `SRTO_REORDERFREEZE` patch — it sheds the old BELABOX-merge hunks (unconditional decay-freeze, periodic-NAK-off, `iMaxReorderTolerance` TTL override) and re-introduces only the opt-in decay freeze.

**Three-way compat probe.** `CMakeLists.txt` runs two `check_cxx_source_compiles` probes and defines macros that gate the libsrt_setup branch at compile time:

| Macro defined | libsrt in use | Behavior |
|---------------|---------------|----------|
| `SLS_HAVE_SRTO_REORDERFREEZE` | `CERALIVE/srt` @ `reorderfreeze-1.5.5` (canonical) | Sets `SRTO_REORDERFREEZE` per-profile; NAK set independently per profile |
| `SLS_HAVE_SRTO_SRTLAPATCHES` | `irlserver/srt` `belabox` (legacy) | Sets `SRTO_SRTLAPATCHES` (fuses NAK-off); per-profile NAK is best-effort |
| neither | Stock Haivision / distro libsrt | Sets `SRTO_NAKREPORT=0` + `SRTO_LOSSMAXTTL=30` on SRTLA listeners |

The startup log emits two lines per listener — one for the compat mode and one for the profile:

- `SRT compat mode: reorderfreeze (CERALIVE/srt, reorderfreeze + nakreport=1).`
- `SRT compat mode: srtlapatches (patched libsrt).`
- `SRT compat mode: standard-options (stock libsrt, nakreport=0, lossmaxttl=30).`

Grep `SRT compat mode` to confirm which libsrt a deployment is running. The mode is fixed at build time; rebuild against the other libsrt to change it.

The stock-libsrt substitution is authorized by ADR-002 ("SRT patch necessity"), whose pre-registered A/B/C reorder-stress evaluation found the standard options a SAFE substitute for the custom patch (identical goodput, zero disconnects, retransmit amplification within the 1.5× tolerance).

The canonical, reproducible build is the [`Dockerfile`](Dockerfile) — Alpine + `CERALIVE/srt@reorderfreeze-1.5.5` + submodules — and CI (`.github/workflows/build-check.yml`) runs `docker build` so the build check never drifts from the production image.

## RECEIVE PROFILES (L1 / L2 / L3)

`irl-srt-server` exposes three static listener profiles. Each listener is tagged at creation in `SLSManager` and the tag drives `CSLSSrt::libsrt_setup` via a `SrtProfileSpec` table in `SLSSrt.cpp`.

| Profile | `sls.conf` directive | Serves | Freeze | NAK | LOSSMAXTTL | RCVLATENCY floor |
|---------|---------------------|--------|--------|-----|------------|-----------------|
| **L1** `L1FreezeNak` | `listen_publisher_srtla` | Balanced / Low-Latency / Resilient / Low-Latency+FEC | yes | on | 30 | 100 ms |
| **L2** `L2Classic` | `listen_publisher_srtla_classic` | Classic | yes | off | 30 | 100 ms |
| **L3** `L3Direct` | `listen_publisher` / player / fallback | OBS / external direct-SRT | no | default | 200 | none |

`LOSSMAXTTL=30` is the validated cap from the A/B profile matrix (Todo 6): the smallest value that passes all six quality clauses across all four non-FEC profiles.

**FEC on L1.** `L1FreezeNak` sets `SRTO_PACKETFILTER="fec"` (accept-form, pre-bind, inherited by accepted sockets). A non-FEC caller is NOT rejected — the SRT responder branch clears the filter per-connection and connects plain (COMPATIBILITY.md §6 case b). A FEC caller negotiates the merged config (case a). There is no separate FEC listener port; L1 serves both.

**Startup log per listener.** `libsrt_setup` emits:
```
SRT profile: L1-freeze-nak (freeze=1, nakreport=1, lossmaxttl=30)
SRT profile: L2-classic (freeze=1, nakreport=0, lossmaxttl=30)
SRT profile: L3-direct (freeze=0, nakreport=default, lossmaxttl=200)
```

**Unit tests.** `tests/test_srt_profiles.cpp` (doctest, gated by `-DSLS_BUILD_TESTS=ON`) binds real listeners and reads sockopts back. The Dockerfile now passes `-DSLS_BUILD_TESTS=ON` so the profile assertions run in the canonical CI gate (`ctest 2/2`).

**`listen_publisher_srtla_classic` directive.** This is a new `sls.conf` directive (L2). It creates an SRTLA-mode listener (same `set_srtla_mode(true)` as L1) but tagged `L2Classic` — NAK-off, freeze-on, LOSSMAXTTL=30. Use it for the Classic profile port alongside the existing `listen_publisher_srtla` (L1) port.

---

## COMMON TASKS

| I need to… | Do this |
|------------|---------|
| Build the server + client | [BUILD](#build) — `git submodule update --init` then `cmake … && make -j` |
| Reproduce the canonical/CI build | `docker build .` — the [`Dockerfile`](Dockerfile) is the source of truth (Alpine + `irlserver/srt@belabox`); CI runs the same on amd64 + arm64 |
| Run / smoke-test the suite | [TEST](#test) — config-validator unit tests + the `srt_client` loopback push/play |
| Change which libsrt is used (patched vs stock) | Rebuild against the other libsrt; the `SLS_HAVE_SRTO_SRTLAPATCHES` CMake probe selects the path. See [SRT DEPENDENCY](#srt-dependency) |
| Confirm which compat mode a running binary took | Grep the journal/stdout for `SRT compat mode` (`srtlapatches` vs `standard-options`) — it is **not** a readable build flag |
| Edit stream routing / listener ports | `sls.conf` — see [WHERE TO LOOK](#where-to-look) and STREAM ID FORMAT |
| Sync upstream fixes | See NOTES — add the `irlserver` remote, merge `irlserver/main` into `master` |

---

## BUILD

```bash
git submodule update --init        # pulls spdlog + json only
mkdir build && cd build
cmake ../ -DCMAKE_BUILD_TYPE=Release
make -j
# binaries: build/bin/srt_server, build/bin/srt_client
```

For a `Debug` build, pass `-DCMAKE_BUILD_TYPE=Debug` instead.

---

## TEST

There is no GoogleTest/ctest harness in this fork; verification is the **CI Docker
build** plus the config-parser unit tests and a runtime loopback smoke test.

- **CI gate (canonical):** `.github/workflows/build-check.yml` runs `docker build`
  on amd64 + arm64, so the build can never drift from the production image. A green
  `docker build` is the required pre-merge gate.
- **Config-validator tests:** the port-list parser (single / comma-list / ascending
  `a-b` range, `1..65535` bound) and the `streamid` safety check (rejects path
  separators, control bytes, bare `.`/`..`) are covered — a malformed `sls.conf`
  fails at parse time with a line number, not later at bind. See the `core:` /
  `test:` commits on the `chore/prod-readiness` line.
- **Loopback smoke test:** run the server, then push a TS file and play it back with
  the bundled client to prove an end-to-end SRT path:

  ```bash
  ./build/bin/srt_server -c ../sls.conf
  ./build/bin/srt_client -r 'srt://127.0.0.1:8080?streamid=uplive.sls/live/test' -i in.ts
  ./build/bin/srt_client -r 'srt://127.0.0.1:8080?streamid=live.sls/live/test'   -o out.ts
  ```

- **Verify the active SRT compat mode** at startup: grep the log for `SRT compat
  mode:` (`srtlapatches` = patched libsrt; `standard-options` = stock libsrt with
  `nakreport=0`, `lossmaxttl=30`).

Place any local test artifacts in a repo-local, gitignored `test-results/` — never
a path that escapes this checkout (Rule D).

---

## WHERE TO LOOK

| Task | Location |
|------|----------|
| Server entry point | `src/srt-live-server.cpp` |
| Client test tool | `src/srt-live-client.cpp` |
| Stream routing config | `sls.conf` |
| Build wiring | `CMakeLists.txt`, `src/CMakeLists.txt` |
| Logging | via `lib/spdlog` |

---

## STREAM ID FORMAT

SLS uses RTMP-style stream IDs in the SRT `streamid` parameter:

- Publisher: `srt://host:8080?streamid=uplive.sls/live/<name>`
- Player: `srt://host:8080?streamid=live.sls/live/<name>`

Publisher and player domain/app combos must differ in `sls.conf`.

---

## NOTES

- Only MPEG-TS format is supported.
- Remote: `origin https://github.com/CERALIVE/irl-srt-server`
- Upstream catch-up: add `irlserver https://github.com/irlserver/irl-srt-server` and merge `irlserver/main` (default branch) into `master`. **Current sync point: `a1dd80c`** (irlserver/main tip, "Merge PR #14 advisor/execute-all"; merge-base advanced from `682ac28` to `a1dd80c`, absorbing 44 of 52 commits). Merge commit: `6386faa`. Regression fix on top: `89d0f8b` (re-arms `SRT_EPOLL_OUT` on `srt_client` push socket, broken by `076a44b` event-driven egress). Our `master` carries `.clang-format`, `AGENTS.md`, a local-scratch gitignore entry, the explicit `util.hpp`/`strlcpy` include, the Docker-based build-check, the ADR-002 `SLS_HAVE_SRTO_SRTLAPATCHES` CMake probe, "SRT compat mode" logging, and the multi-listen-port config validator. See `docs/upstream-currency-2026-06.md` for the full improvement report and `docs/upstream-sync-2026-06.md` for the T4 triage classification.
- CI: `.github/workflows/build-check.yml` runs `docker build` on amd64 + arm64.
- Not part of the device image — cloud deployment only.
- Decision records: ADR-002 ("SRT patch necessity") is prose in this file and `README.md` — no file. ADR-003 ("reject service migration, harden in place") is [`docs/adr/ADR-003-service-migration.md`](docs/adr/ADR-003-service-migration.md).
