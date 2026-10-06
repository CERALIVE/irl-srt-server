<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

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

