<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

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

The canonical and GitHub default branch is `main`; both push and PR filters name
`main`. The pre-swap canonical history is preserved at `legacy` (`ae229f9`), never
force-pushed or deleted. Publication fetches and validates `origin/main`, and the
Cosign certificate identity names `refs/heads/main`.

