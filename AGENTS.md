# irl-srt-server (CERALIVE)

Agent guide for CERALIVE's hard fork of `irlserver/irl-srt-server` (SRT Live Server, SLS).
Read this before touching anything. Upstream's own orientation is `CLAUDE.md`; this file
covers only what CERALIVE adds on top.

## WHAT THIS FORK IS

- **Server source is byte-identical to upstream.** The hard-fork base is
  `a86dd8abd3659baea8ca8315bb4842410d4bc292` ("feat(core): list each stream's players in
  /stats and fix max_players"). `git diff --stat <base>..HEAD -- src/` must print nothing
  beyond the ledgered `port` rows (see below). No CERALIVE feature lives in `src/`.
- **What CERALIVE owns:** the libsrt pin (`Dockerfile`, `.github/workflows/ci.yml`), the CI/CD
  layer (`ci.yml`, `build-check.yml`, `publish-image.yml`), the repository contract scripts
  under `scripts/`, a small set of security-class `fix(auth)`/`fix(core)` ports, and these
  docs.
- **Version is upstream's.** `project(srt-live-server VERSION 3.1.0)` in `CMakeLists.txt` is
  not bumped by this fork. The image tag equals that PROJECT_VERSION; the first git tag will be
  `v3.1.0`. Semver, never CalVer.
- **Consumers:** `ceralive-platform` pulls `ghcr.io/ceralive/irl-srt-server:<PROJECT_VERSION>`
  by digest. The bonded uplink arrives through `srtla_rec` (CERALIVE/srtla) on
  `listen_publisher_srtla`; direct SRT publishers use `listen_publisher`.

## UPSTREAM RELATIONSHIP (bidirectional)

Two remotes: `origin` (`CERALIVE/irl-srt-server`, always present) and `irlserver`
(`https://github.com/irlserver/irl-srt-server.git`, **transient**: add, fetch, merge or
cherry-pick, remove before any push or PR). Never leave the upstream remote attached at PR
time; `git remote -v` must show only `origin`.

- **Pulling:** manual and deliberate, one dedicated PR per sync, full gate green before it
  lands. No bots, no scheduled sync. Because `src/` is upstream's, a sync is normally a clean
  fast-forward of `src/` plus a re-check of the CI layer.
- **Pushing back:** a defect found here that exists upstream is **offered upstream first**
  (issue or PR on `irlserver/irl-srt-server`), then carried locally as a ledgered `port` row
  until it lands there. Known candidate at the time of writing: upstream's shipped
  `src/sls.conf` is CRLF and upstream's own parser rejects CRLF, so the stock image cannot
  boot its default config. CI normalizes into a scratch copy; nobody has patched `src/` for it.
- **Ledger:** `docs/notes/upstream-hardfork-sls-ledger.md` classifies every legacy CERALIVE
  commit as `drop` / `port` / `already-upstream`. A `port` is a re-implementation against the
  upstream tree with its own test, never a `git cherry-pick` across the fork boundary.

## LIBSRT PIN CONTRACT

SLS names `SRTO_SRTLAPATCHES` unconditionally (`src/core/SLSSrt.cpp`). Stock Haivision libsrt
does not declare it, so the unmodified upstream source only compiles against a libsrt that
does. Upstream builds against `irlserver/srt` (branch `belabox`, an SRT 1.5.5-era fork).
CERALIVE builds against **`CERALIVE/srt`**, which is Haivision **1.5.7** plus the CERALIVE
socket options. The compat enumerator lives in the srt fork, not in SLS.

- **Current pin (RELEASE TAG):** `d487b13365205b6cd5da9d9b50868c323e255b7c`, which is the
  tag **`srt-v1.5.7+ceralive.2`** (Debian `libsrt1.5-ceralive 1.5.7+ceralive.2`). Resolve a
  tag to its **own** commit (`git rev-parse <tag>^{commit}`), NOT to the merge commit that
  preceded it — this tag sits one docs-correction commit after merge `26e78679`, and pinning
  the merge would silently build a different tree than the released package. The interim
  branch pin `51d500c4…` on `feat/srtla-options-1.5.7` is RETIRED and enumerated in
  `RETIRED_PINS`.
- **`scripts/check-srt-pin.sh` is the gate.** It asserts four build inputs agree on
  `EXPECTED_PIN`: `Dockerfile` `ARG SRT_COMMIT` plus exactly three `SRT_COMMIT:` env sites
  in `ci.yml` (build-and-test, fuzz, coverage). The `clang-tidy` job reads the commit out of
  the Dockerfile on purpose so it cannot drift and the count stays three. Retired SHAs go
  into `RETIRED_PINS`, so an old pin can never be re-introduced silently.
- **Bumping the pin touches five places:** `EXPECTED_PIN` and `RETIRED_PINS` in the script,
  `ARG SRT_COMMIT`, and the three `ci.yml` sites. Then run the script.
- **Same-libsrt invariant:** the server image and the device (`libsrt1.5-ceralive`) must
  run the same libsrt release. A pin bump here is paired with the device pin in the root
  `versions.yaml`.
- The `stock-libsrt-expected-incompatible` CI leg builds against apt libsrt and **passes only
  if the compile fails** on `SRTO_SRTLAPATCHES`. If it ever starts succeeding, a source-level
  fallback was smuggled in or a non-stock header leaked onto the include path.

## THE THREE CERALIVE SOCKET OPTIONS (`CERALIVE/srt` `srtcore/srt.h`)

| Number | Enumerator | Type | Meaning |
|---|---|---|---|
| `118` | `SRTO_SRTLAPATCHES` | bool | Compat enumerator with upstream `irlserver/srt`'s name and semantics. Setter: `bReorderFreeze = on; iPeriodicNakGate = on ? SRTLA_PATCHES_DEFAULT_NAKGATE : 0`. Getter: `bReorderFreeze && iPeriodicNakGate != 0`. Only 0 / non-zero are meaningful. URI key `srtlapatches`. |
| `119` | `SRTO_PERIODICNAKGATE` | int, tri-state | `0` off (Haivision behaviour: periodic NAK always sent). `1` filter: subtract still-reorderable fresh-loss ranges, send the periodic NAK only for genuine loss. `2` suppress: never send the periodic NAK (bit-exact upstream `SRTLAPATCHES` site 4). Any other value is `SRT_EINVPARAM`. URI key `periodicnakgate`. |
| `120` | `SRTO_REORDERFREEZE` | bool | Freeze `m_iReorderTolerance` at `iMaxReorderTolerance`: no decay on ordered/early runs. Pre-existing CERALIVE option. URI key `reorderfreeze`. |

Upstream `irlserver/srt` numbers its `SRTO_SRTLAPATCHES` **120**, the same number CERALIVE
already used for `REORDERFREEZE`. Numbers are ABI; never renumber any of the three.

**Setting `PERIODICNAKGATE` after `SRTLAPATCHES` overrides it** (last write wins). SLS only
sets `SRTLAPATCHES`, so the server's effective behaviour is whatever the compat default
resolves to.

### Equivalence: upstream `SRTLAPATCHES=1` vs CERALIVE `REORDERFREEZE=1 + PERIODICNAKGATE`

Verified by reading both trees (upstream `f2297192:srtcore/core.cpp` against CERALIVE's).

| # | Upstream gate site | CERALIVE | Verdict |
|---|---|---|---|
| 1 | `initial_loss_ttl = srtlaPatches ? iMaxReorderTolerance : m_iReorderTolerance` | `initial_loss_ttl = m_iReorderTolerance` | **Exactly equivalent.** Every write to `m_iReorderTolerance` was enumerated: init sets it to the max; the only decrements are the two freeze-gated sites; the only other write is an increase capped at the max. While `bReorderFreeze`, `m_iReorderTolerance == iMaxReorderTolerance`. |
| 2 | 50-consecutive-ordered decay gated `!srtlaPatches` | gated `!bReorderFreeze` | Equivalent. |
| 3 | 10-consecutive-early decay gated `!srtlaPatches` | gated `!bReorderFreeze` | Equivalent. |
| 4 | periodic NAK: `if (!srtlaPatches) sendCtrl(UMSG_LOSSREPORT)` (suppressed entirely) | `PERIODICNAKGATE`: `2` = suppressed (bit-exact), `1` = filtered (genuine loss still reported) | **Divergent by design; resolved by measurement, not on paper.** |

### The periodic-NAK default is a PLACEHOLDER (A/B PENDING)

`SRTLA_PATCHES_DEFAULT_NAKGATE` (`srtcore/socketconfig.h` in `CERALIVE/srt`) is currently
**`2`** (upstream-exact suppress) **until measured**. The D10 A/B (plan
`upstream-rebase-hard-fork`, todos 36-38) runs `1` vs `2` on the `srtla` repo's compat
harness (fixed netem loss + reorder cells, N=3, primary metrics viewer-observed loss and
goodput, secondary retransmit ratio; a tie resolves to `2`). **The result is not in yet.**
Do not write, in code or docs, that `2` is the chosen default. When the A/B lands, the winner
is pinned by number in the srt release notes and in the image description, and this section
is rewritten with the verdict.

## CI LANES

- **`ci.yml`** (push/PR): `repository contracts` (`check-action-refs.sh`,
  `check-tracked-workspace-evidence.sh`, `check-srt-pin.sh`, `test-image-publish-contracts.sh`);
  `build-and-test` matrix: `debug`, `asan-ubsan`, `tsan` (each builds the pinned CERALIVE/srt
  and runs the full ctest suite plus the publisher-authorization probation E2E) and the
  `stock-libsrt-expected-incompatible` negative leg; `clang-tidy` (baselined, srt headers from
  the Dockerfile pin); `clang-format` (changed lines); `fuzz` (four libFuzzer targets, 60 s
  each, pinned libsrt); `coverage` (gcovr, report-only).
- **`build-check.yml`** (push/PR): the production `Dockerfile` for `amd64` and `arm64` with
  binary verification against the pin, Trivy, CycloneDX SBOM, advisory CodeQL, static analysis.
- **`publish-image.yml`** (manual dispatch only): see `docs/IMAGE-RELEASE.md`.
- `netns`/privileged coverage does not exist here; nothing in this repo needs it.

Branch filters currently say `upstream/main`. After the canonical swap they say `main`.
Every workflow, script, and doc must stop naming the old canonical branch by then.

## BUILD AND TEST (local)

```bash
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSLS_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
bash scripts/check-srt-pin.sh
bash scripts/test-image-publish-contracts.sh
```

The host must have `CERALIVE/srt` at the pinned commit installed (`./configure && make &&
make install`, exactly as the `Dockerfile` does). A stock `libsrt-openssl-dev` on the include
path wins over `/usr/local` on some distros and turns every build into the negative leg;
`docker build .` is the safe isolation. Before scripting an edit, check line endings:
several `src/core/*.cpp` files and `src/sls.conf` are CRLF.

## RELEASE PROCEDURE

1. The commit is on canonical `main` (until the swap: `upstream/main`), CI green, the
   `check-srt-pin.sh` gate green, `CMakeLists.txt` PROJECT_VERSION is the intended tag.
2. Confirm `ghcr.io/ceralive/irl-srt-server:<PROJECT_VERSION>` is unused with
   `scripts/check-image-tags-unused.sh <image> <tag> <sha>`. This needs a credential with
   `read:packages`; the package is private and an anonymous or under-scoped token gets `403`,
   which the script correctly treats as blocking, not as "tag free".
3. Dispatch `publish-image.yml` from `main` with `release_tag=<PROJECT_VERSION>` and
   `expected_sha=<40-hex>`. `validate-image-release.sh` rejects anything that is not semver
   `MAJOR.MINOR.PATCH`, not equal to PROJECT_VERSION, not the checked-out SHA, or not an
   ancestor of `main`.
4. Both architecture release gates build the Dockerfile and boot the image, asserting
   `Initialized libsrt v1.5.7`, `srtla_patches=on`, and no `SRTO_SRTLAPATCHES failure`.
5. The publish job pushes `:<tag>` and `:sha-<sha>`, attaches provenance and SBOM, signs the
   digest keylessly, verifies the signature, and prints the receipt in the run summary.
6. Tag the commit `v<PROJECT_VERSION>` and write the release notes: **pin the exact libsrt
   release and the three option numbers with their effective values** (118 on, 119 = the
   compat default, 120 on).

Status at the time of writing: **no CERALIVE image has been published from this base yet.**
`3.1.0` is the target tag; the live "tag unused" pre-flight is still owed by the publish todo.

## ANTI-PATTERNS (things the legacy fork did that this base does NOT)

- **No receive profiles or modes.** No profile table, no per-listener mode selection, no
  L1/L2/L3 tiers. SRTLA behaviour is upstream's: one boolean, set on `listen_publisher_srtla`.
- **No `lossmaxttl` tuning.** Upstream's listener default (`200` packets, not ms) stands.
- **No audio-gap concealment.** Upstream removed it (`47e3ac4`); the server relays the TS
  opaquely and gap handling belongs to the playback side.
- **No per-source-IP connection limiter.** SLS sits behind a protected proxy, so every client
  shares the proxy's source address; a limiter would throttle legitimate traffic collectively.
- **No compat shim in `src/`.** `#ifdef SRTO_SRTLAPATCHES` fallbacks belong in the srt fork,
  never here; the negative CI leg exists to catch exactly that.
- **No CalVer, no `latest` tag, no moved tags.** Release tags equal PROJECT_VERSION and are
  immutable; rollback is a platform-side digest selection.
- **No auto-sync with upstream, no upstream remote left attached.**
- **No verbatim copy of legacy docs.** The legacy `AGENTS.md`, the audio-gap feature doc,
  `docs/evidence/`, and the legacy `docs/upstream-*.md` sync notes are not carried; they
  describe a fork that no longer exists.

## DOCS DISCIPLINE

Any behaviour, pin, or CI change updates this file and `README.md` in the same PR. The pin
SHA and the A/B status above are the two things most likely to go stale; check both first.
