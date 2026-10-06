# Agent contract index

Every original AGENTS.md block is preserved below. Read the relevant contract before editing.

| Original heading | Contract | Governed paths / tasks |
|---|---|---|
| Overview | [overview.md](overview.md) | `irlserver/irl-srt-server`, `CLAUDE.md` |
| WHAT THIS FORK IS | [what-this-fork-is.md](what-this-fork-is.md) | `git diff --stat <base>..HEAD -- src/`, `src/`, `.github/workflows/ci.yml`, `ci.yml`, `build-check.yml`, `publish-image.yml`, `scripts/`, `/` |
| UPSTREAM RELATIONSHIP (bidirectional) | [upstream-relationship-bidirectional.md](upstream-relationship-bidirectional.md) | `CERALIVE/irl-srt-server`, `https://github.com/irlserver/irl-srt-server.git`, `src/`, `irlserver/irl-srt-server`, `src/sls.conf`, `docs/notes/upstream-hardfork-sls-ledger.md`, ` / ` |
| LIBSRT PIN CONTRACT | [libsrt-pin-contract.md](libsrt-pin-contract.md) | `src/core/SLSSrt.cpp`, `irlserver/srt`, `CERALIVE/srt`, `feat/srtla-options-1.5.7`, `scripts/check-srt-pin.sh`, `ci.yml` |
| THE THREE CERALIVE SOCKET OPTIONS (`CERALIVE/srt` `srtcore/srt.h`) | [the-three-ceralive-socket-options-ceralive-srt-srtcore-sr.md](the-three-ceralive-socket-options-ceralive-srt-srtcore-sr.md) | `CERALIVE/srt`, `srtcore/srt.h`, `irlserver/srt`, `. Only 0 / non-zero are meaningful. URI key `, `: no decay on ordered/early runs. Pre-existing CERALIVE option. URI key `, `f2297192:srtcore/core.cpp`, `srtcore/socketconfig.h` |
| CI LANES | [ci-lanes.md](ci-lanes.md) | `ci.yml`, `check-action-refs.sh`, `check-tracked-workspace-evidence.sh`, `check-srt-pin.sh`, `test-image-publish-contracts.sh`, `build-check.yml`, `publish-image.yml`, `docs/IMAGE-RELEASE.md` |
| BUILD AND TEST (local) | [build-and-test-local.md](build-and-test-local.md) | `CERALIVE/srt`, `/usr/local`, `src/core/*.cpp`, `src/sls.conf` |
| RELEASE PROCEDURE | [release-procedure.md](release-procedure.md) | `check-srt-pin.sh`, `ghcr.io/ceralive/irl-srt-server:<PROJECT_VERSION>`, `scripts/check-image-tags-unused.sh <image> <tag> <sha>`, `publish-image.yml`, `validate-image-release.sh` |
| ANTI-PATTERNS (things the legacy fork did that this base does NOT) | [anti-patterns-things-the-legacy-fork-did-that-this-base-d.md](anti-patterns-things-the-legacy-fork-did-that-this-base-d.md) | `src/`, `AGENTS.md`, `docs/evidence/`, `docs/upstream-*.md` |
| DOCS DISCIPLINE | [docs-discipline.md](docs-discipline.md) | `README.md` |
