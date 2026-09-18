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
| SRT transport | System-installed libsrt (`-lsrt`); canonical CERALIVE fork required for converged bonded listeners. Stock/BELABOX still compile and support L3. No srt submodule |
| Submodules | `lib/spdlog` (irlserver/spdlog fork), `lib/json` (nlohmann/json v3.12.0), `lib/thread-pool` (bshoshany v5.1.0), `lib/cpp-httplib` (yhirose v0.48.0 @ `9d159bb`), `lib/CxxUrl` (commit `e81b86e`) |
| Config | `sls.conf` — domain/app/stream routing, publisher vs player separation |

---

## SRT DEPENDENCY

`irl-srt-server` has no `srt` submodule. `.gitmodules` contains five submodules: `lib/spdlog`, `lib/json`, `lib/thread-pool`, `lib/cpp-httplib`, and `lib/CxxUrl`. `src/CMakeLists.txt` links with `-lsrt` directly, so system-installed libsrt must be present before building.

**Canonical build pin: `CERALIVE/srt` branch `feat/bonded-path-convergence`, SHA `ca14c8bd06c89d2fd7b69bb3d8eea48dd47c2e3e`.** This published branch contains the v1.5.7 merge, `SRTO_REORDERFREEZE`, socket teardown fixes, and the opt-in `SRTO_PERIODICNAKGATE` implementation. The planned release is `1.5.7+ceralive.1`; this pin does not claim that release/tag or the device-image cutover has happened. Docker and every CI `SRT_COMMIT` agree, enforced by `scripts/check-srt-pin.sh`.

**Compile and runtime capability gate.** `CMakeLists.txt` uses three `check_cxx_source_compiles` probes because these options are enum members, not preprocessor macros:

| Macro defined | libsrt in use | Behavior |
|---------------|---------------|----------|
| `SLS_HAVE_SRTO_REORDERFREEZE` | CERALIVE/srt | Sets freeze independently of NAK |
| `SLS_HAVE_SRTO_SRTLAPATCHES` | `irlserver/srt` `belabox` (legacy) | Sets `SRTO_SRTLAPATCHES` (fuses NAK-off); per-profile NAK is best-effort |
| `SLS_HAVE_SRTO_PERIODICNAKGATE` (0 or 1) | Gate-capable CERALIVE/srt | Converged bonded profiles additionally require successful runtime `srt_setsockflag` |
| no gate | Stock / older forks | Converged L1/L2 refuse startup; L3 remains unchanged |

The startup log reports the compiled compatibility path, successful gate activation, and effective profile:

- `SRT compat mode: reorderfreeze+periodicnakgate` (successful converged bonded setup).
- `SRT compat mode: srtlapatches (patched libsrt).`
- `SRT compat mode: standard-options (stock libsrt).` (direct or explicit legacy override only).

Grep `SRT compat mode` to confirm which libsrt a deployment is running. The mode is fixed at build time; rebuild against the other libsrt to change it.

The historical ADR-002 stock substitution is NOT a silent fallback for the new converged policy. Missing headers or rejected runtime gate logs ERROR: `bonded profile requires SRTO_PERIODICNAKGATE (libsrt >= 1.5.7+ceralive.1); refusing to start listener`. Setup returns `SLS_ERROR`; listener/manager failure propagates to a nonzero process exit after cleanup.

The canonical build is the [`Dockerfile`](Dockerfile) — Alpine + the pinned CERALIVE/srt branch + submodules. Initialized submodule content also permits builds from a worktree whose `.git` pointers are external; otherwise Docker initializes submodules normally. Local build directories are excluded from the context.

## RECEIVE PROFILES (L1 / L2 / L3)

`irl-srt-server` exposes three static listener profiles. Each listener is tagged at creation in `SLSManager` and the tag drives `CSLSSrt::libsrt_setup` via a `SrtProfileSpec` table in `SLSSrt.cpp`.

| Profile | `sls.conf` directive | Serves | Freeze | NAK | LOSSMAXTTL | RCVLATENCY floor |
|---------|---------------------|--------|--------|-----|------------|-----------------|
| **L1** `L1FreezeNak` | `listen_publisher_srtla` | All bonded senders, optional FEC | yes | on + gate | 200 static fallback | 100 ms |
| **L2** `L2Classic` | `listen_publisher_srtla_classic` | Deprecated alias of L1, identical policy | yes | on + gate | 200 static fallback | 100 ms |
| **L3** `L3Direct` | `listen_publisher` / player / fallback | OBS / external direct-SRT | no | default | 200 | none |

`kBondedLossMaxTtl=200` is the measured M1 upstream-parity fallback, confirmed by
Todo 24's added released `ours-3.3.0` / C sweep (40/200/500, N=3 each). Original
TTL*=200; combined TTL*=200; controller=false in both decisions. No TTL passes
all owned cells, and the >=644.649 ms freeze penalty independently caps TTL at
200. The unchanged 24-Mbit diagnostic also prefers 200, so there is no live-change
spike, controller, or runtime config addition. This is NOT universal interop PASS:
released 3.3.0/C still fails at 200; its passing 500 arm cannot override the frozen
fallback/cap. The required M3 blocker re-evaluation is complete, but that performance
limitation remains for the owner-gated rollout. See
[`Todo 24 evidence`](docs/evidence/bpc/task-24-lossmaxttl.md).
L3's literal remains `false, false, false, 200, 0, false, false`.

Per-streamid negotiation is STRUCTURALLY IMPOSSIBLE: srtla_rec is libsrt-free, and the SRT handshake terminates at the encoder, so the receiver can NEVER learn the SRTLA sender's lineage.

**Rollback override:** `SLS_BONDED_PROFILE_OVERRIDE=converged|legacy-l1|legacy-l2`, read once by `libsrt_init` before listener creation and logged at INFO. Both aliases always switch together. `legacy-l1` selects freeze/NAK-on/TTL40/floor100/FEC/gate-off; `legacy-l2` selects freeze/NAK-off/TTL40/floor100/no-FEC/gate-off. L3 is never changed. Unknown values warn and retain converged (never downgrade). Reload does not reread the environment; restart to change it.

**FEC on both bonded aliases.** Both set `SRTO_PACKETFILTER="fec"` (accept-form, pre-bind, inherited by accepted sockets). A non-FEC caller connects plain; a FEC caller negotiates the merged config. No new per-connection callback or streamid policy is introduced.

**Startup log per listener.** `libsrt_setup` emits:
```
profile=L1-bonded freeze=1 nakreport=1 periodic_nak_gate=1 lossmaxttl=200 floor=100 fec_accept=1
profile=L2-bonded-alias freeze=1 nakreport=1 periodic_nak_gate=1 lossmaxttl=200 floor=100 fec_accept=1
profile=L3-direct freeze=0 nakreport=default periodic_nak_gate=0 lossmaxttl=200 floor=0 fec_accept=0
```

**Tests.** `tests/test_srt_profiles.cpp` binds ephemeral real sockets and reads options back, compares all policy fields (names identify aliases), freezes L3, and tests all overrides in fresh CTest processes. Linux linker wrapping fails only the gate syscall and proves `SLS_ERROR` plus ERROR logging and socket cleanup; L3 bypasses it. Without the enum, tests assert refusal, not downgrade. CTest registers the real `srt_loopback` when ffmpeg is available in a non-sanitizer build: stock builds assert both bonded startup failures before L3 relay/byte-integrity. Unsupported bonded cases print `SKIP: no SRTO_PERIODICNAKGATE`. CI installs e2e tools on debug AND stock; BELABOX remains compile-only. Docker runs this same CTest suite once.

**`listen_publisher_srtla_classic` directive.** Deprecated alias, including port 4003. The first alias setup logs one WARN per process: `listen_publisher_srtla_classic is a deprecated alias of listen_publisher_srtla (same policy); it will be removed in a future release`. Enum names remain stable for callers; they no longer imply different policies.

The profile suite also connects and accepts real loopback publisher sockets,
asserting inherited TTL and initial reorder tolerance against independent literals:
200 for converged/L3, 40 for explicit legacy bonded overrides. A temporary TTL40
mutation demonstrated that the new bonded assertions fail. No POST mutation is
needed on the static branch: libsrt inherits the listener's options at accept.

---

## COMMON TASKS

| I need to… | Do this |
|------------|---------|
| Build the server + client | [BUILD](#build) — `git submodule update --init` then `cmake … && make -j` |
| Reproduce the canonical/CI build | `docker build .` — Alpine + the pinned gate-capable CERALIVE/srt; CI runs the same on amd64 + arm64 |
| Publish a production image | Follow [`docs/IMAGE-RELEASE.md`](docs/IMAGE-RELEASE.md) — manual dispatch from `master` with an unused release tag + exact full SHA; the workflow gates both architectures and signs/verifies the manifest digest. It never deploys or changes platform variables |
| Run / smoke-test the suite | [TEST](#test) — config-validator unit tests + the `srt_client` loopback push/play |
| Change which libsrt is used | Rebuild against the other libsrt; all three compile probes select capabilities. Converged bonded setup also probes the runtime. See [SRT DEPENDENCY](#srt-dependency) |
| Confirm which compat mode a running binary took | Grep for `SRT compat mode`; converged bonded setup requires `reorderfreeze+periodicnakgate` |
| Edit stream routing / listener ports | `sls.conf` — see [WHERE TO LOOK](#where-to-look) and STREAM ID FORMAT |
| Sync upstream fixes | See NOTES — add the `irlserver` remote, merge `irlserver/main` into `master` |

---

## BUILD

```bash
git submodule update --init        # pulls all 5 vendored libs (spdlog, json, thread-pool, cpp-httplib, CxxUrl)
mkdir build && cd build
cmake ../ -DCMAKE_BUILD_TYPE=Release
make -j
# binaries: build/bin/srt_server, build/bin/srt_client
```

For a `Debug` build, pass `-DCMAKE_BUILD_TYPE=Debug` instead.

---

## IMAGE RELEASE

`.github/workflows/publish-image.yml` is the only production image publication
path. It accepts a required lowercase release tag and required 40-character
lowercase commit SHA through `workflow_dispatch`; it has no branch-push trigger.
The checkout must equal the requested SHA, the dispatch ref must be `master`,
and the commit must be on `origin/master`.

The release gate builds and tests the canonical Dockerfile natively on Linux
`amd64` and `arm64`. Only after both legs pass does the publish job create one
multi-architecture manifest under both the user-supplied tag and
`sha-<full-commit>`. Existing tags cause a hard failure and are never moved.
Docker Buildx emits provenance and SBOM attestations. Cosign signs the captured
manifest digest with GitHub OIDC and verifies the workflow identity before the
run succeeds.

The exact dispatch, receipt verification, platform handoff, and rollback
boundary are documented in [`docs/IMAGE-RELEASE.md`](docs/IMAGE-RELEASE.md).
`IRL_SRT_SERVER_RELEASE_TAG` is set in `ceralive-platform` only after a verified
publish and separate authorization; this workflow does not set it and performs
no deployment.

---

## TEST

The repository ships a doctest-based unit test suite wired into CTest, plus sanitizer builds, libFuzzer targets, and e2e scripts. The CI Docker build remains the canonical pre-merge gate.

- **CI gate (canonical):** `.github/workflows/build-check.yml` runs `docker build`
  on amd64 + arm64 against the pinned gate-capable CERALIVE/srt, so the build can
  never drift from the production image. A green `docker build` is the required
  pre-merge gate. It also asserts `SRT compat mode: reorderfreeze+periodicnakgate` in the startup
  log and runs Trivy CVE scanning + Syft SBOM generation.
- **CI quality gates** (`.github/workflows/ci.yml`): the repository-contract job plus
  the existing quality jobs run on every push/PR. The contract job rejects tracked
  references to workspace-local agent evidence; `.gitignore` remains the only
  permitted tracked mention of that local boundary. It also runs
  `scripts/test-image-publish-contracts.sh`. That suite resolves every external
  action used by the publish workflow through the GitHub commits API, with a
  bounded timeout, so missing or stale refs fail before a release dispatch.
  `sigstore/cosign-installer` is pinned to the immutable commit behind its current
  stable tag because upstream does not publish a floating major ref. The suite
  also executes malformed input, wrong/non-ancestor SHA, tag collision/repeat,
  and registry-response fixtures against the same guard scripts the workflow
  calls. The collision guard permits only status `1` paired with an exact
  verified absent-tag response: `manifest unknown`, `no such manifest`, or Buildx/GHCR's
  `ERROR: <requested-ref>: not found`; authorization, network, malformed,
  ambiguous, executable, and existing-digest responses remain blocking. It then
  mutation-tests the parsed workflow structure for the release-gate dependency,
  immutable full-SHA tag, both architectures, least-privilege/non-cancelling
  policy, and digest signing/verification:
  - `build-and-test` — canonical debug / ASan-UBSan / TSan plus stock full-test
    and BELABOX compile-only, with high-signal warning errors and full `ctest`.
  - `clang-tidy` — informational full baseline pass + a diff gate that fails only on
    NEW findings introduced on changed lines (decision D4: no blanket flip).
  - `clang-format` — style gate scoped to lines changed since the merge-base in `src/`.
  - `fuzz` — time-boxed libFuzzer smoke run (4 × 60 s) against the canonical SRT pin,
    failing on any crash; uploads `crash-*` / `oom-*` / `timeout-*` / `leak-*` as the
    `fuzz-findings` artifact.
- **Unit tests:** run with `-DSLS_BUILD_TESTS=ON`. Covers the config-validator (port-list
  parser, `streamid` safety gate), SRT profile sockopt assertions
  (`tests/test_srt_profiles.cpp`), and the listener-directive-to-profile
  mapping (`tests/test_listener_profile_map.cpp`, 12 assertions).

  ```bash
  cmake -S . -B build -DSLS_BUILD_TESTS=ON
  cmake --build build -j
  ctest --test-dir build --output-on-failure
  ```

- **Sanitizer builds:** two mutually exclusive flavors for catching memory and threading
  bugs in the ring buffer and cross-thread role/listener/manager state:

  ```bash
  # AddressSanitizer + UndefinedBehaviorSanitizer
  cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSLS_BUILD_TESTS=ON -DSLS_SANITIZE=ON
  cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

  # ThreadSanitizer
  cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSLS_BUILD_TESTS=ON -DSLS_TSAN=ON
  cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
  ```

- **Fuzz targets:** three libFuzzer targets in `tests/fuzz/` exercise the
  network- and operator-boundary input parsers under ASan + UBSan:

  | Target | Drives | Seed corpus |
  |--------|--------|-------------|
  | `fuzz_ts_parser` | MPEG-TS / PAT / PMT / PES parser | `tests/fuzz/corpus/ts/` |
  | `fuzz_streamid` | SRT `streamid` parse + safety gate | `tests/fuzz/corpus/streamid/` |
  | `fuzz_conf` | `sls.conf` port-list / tokenizer / value setters | `tests/fuzz/corpus/conf/` |

  `SLS_FUZZ` is a clang-only, dedicated build flavor (mutually exclusive with
  `SLS_SANITIZE` / `SLS_TSAN`). See README "Fuzzing the parsers" for the full build
  and run commands. The CI `fuzz` job mirrors the local 60 s smoke invocation exactly.

- **E2e scripts:**
  - `tests/e2e/srt_loopback.sh` — five-phase loopback: baseline (L3), FEC-accept (L1),
    byte-integrity, loss-matrix with NAK-on across L1/L2 (CTest/CI-wired). Requires `NET_ADMIN`
    for the netem loss leg (self-SKIPs when unavailable).
  - `tests/e2e/stats_snapshot.sh` — golden-snapshot the `/stats` + control API shape;
    fixtures in `tests/fixtures/`. Run with `--update` to re-bless after a legitimate
    shape change.

- **Loopback smoke test:** run the server, then push a TS file and play it back with
  the bundled client to prove an end-to-end SRT path:

  ```bash
  ./build/bin/srt_server -c ../sls.conf
  ./build/bin/srt_client -r 'srt://127.0.0.1:8080?streamid=uplive.sls/live/test' -i in.ts
  ./build/bin/srt_client -r 'srt://127.0.0.1:8080?streamid=live.sls/live/test'   -o out.ts
  ```

- **Verify the active SRT compat mode**: converged bonded startup must log
  `reorderfreeze+periodicnakgate`; `srtlapatches`/`standard-options` alone cannot serve it.

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

## RECEIVER CAPABILITY RECONCILIATION

Canonical decision record: [`docs/RECEIVER-RECONCILIATION.md`](../docs/RECEIVER-RECONCILIATION.md)

The historical 30-vs-40 calibration selected 40 by tie-break. It now applies to
the explicit rollback policies only. Converged bonded TTL is the static 200
upstream-parity fallback from M1 plus Todo 24's released-sender extension, not a
universal performance pass; see the residual C failure above.

Cross-ref: [`docs/RECEIVER-RECONCILIATION.md`](../docs/RECEIVER-RECONCILIATION.md),
[`srtla/docs/adr/ADR-002-srt-patch-necessity.md`](../srtla/docs/adr/ADR-002-srt-patch-necessity.md)

---

## NOTES

- Only MPEG-TS format is supported.
- Remote: `origin https://github.com/CERALIVE/irl-srt-server`
- Upstream catch-up: add `irlserver https://github.com/irlserver/irl-srt-server` and merge `irlserver/main` (default branch) into `master`. **Previous sync point: `ba2b04a`** ("fix(core): don't drop a publisher on an empty non-blocking read"). That reconciliation preserved the then-current profiles, 8 MiB receive bound, audio-gap API/stats, authenticated loopback control plane, and image-release gates. It adopted relay ownership, epoll-before-close teardown, non-fatal `EASYNCRCV`, publisher probation/takeover protection, viewer re-anchoring, reconnect tests, and timecode/continuity diagnostics. Earlier classification remains in `docs/upstream-currency-2026-06.md` and `docs/upstream-sync-2026-06.md`.
- Current upstream sync additionally includes `a86dd8a`: per-stream player registry/stats and the max-players fix. Merge resolution preserves CeraLive audio-gap stats, handoff-before-map teardown, and callbacks outside the socket lifetime lock.
- CI: `.github/workflows/build-check.yml` runs `docker build` on amd64 + arm64 (canonical gate-capable SRT pin). `.github/workflows/ci.yml` runs repository/release contracts; a five-leg build matrix (canonical debug/ASan-UBSan/TSan, stock full-test, BELABOX compile-only); clang-tidy; clang-format; four-target fuzzing; and coverage.
- Not part of the device image — cloud deployment only.
- Decision records: ADR-002 ("SRT patch necessity") is prose in this file and `README.md` — no file. ADR-003 ("reject service migration, harden in place") is [`docs/adr/ADR-003-service-migration.md`](docs/adr/ADR-003-service-migration.md).
