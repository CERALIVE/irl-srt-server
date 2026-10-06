<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## WHAT THIS FORK IS

- **Server source is byte-identical to upstream.** The hard-fork base is
  `a86dd8abd3659baea8ca8315bb4842410d4bc292` ("feat(core): list each stream's players in
  /stats and fix max_players"); that is also the last-merged upstream SHA, and the next
  sync's merge base is computed from it. `git diff --stat <base>..HEAD -- src/` must print nothing
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

