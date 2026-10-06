# irl-srt-server

Parent: [workspace rules](https://github.com/CERALIVE/ceralive/blob/master/AGENTS.md)

<!-- workspace-hard-rules:begin -->
## Workspace hard rules (identical in every CeraLive AGENTS.md)
- Commits and PRs carry the human author only: no Co-authored-by, no AI attribution.
- Start from the updated canonical branch; rebase to update; never `reset --hard` or discard others' work.
- One focused PR per repo, opened against CERALIVE/<repo>; the root policy PR merges first.
- A repo is self-contained: no path above its root; consume @ceralive packages from the registry, never link:/file:.
- Never delete, skip or weaken a test; every behavior change ships with a test.
- A user-visible change updates docs.ceralive.tv in English and Spanish (es-419), and any ceralive.tv claim it touches, in the same release.
- AGENTS.md holds rules and routing only, within budget; contracts and history live in docs/agents/.
- Full canon: https://github.com/CERALIVE/ceralive/blob/master/AGENTS.md
<!-- workspace-hard-rules:end -->

## ROLE

Cloud-edge SRT receiver and re-server downstream of SRTLA. Uses installed system libsrt from the pinned CERALIVE fork, not stock Haivision. Canonical branch: `main`.

## STRUCTURE

`src/`, `lib/`, `tests/`, `docs/`, `scripts/`, `tools/`, `plans/`, `.github/`; submodules listed in HARD RULES.

## COMMANDS

```bash
bash scripts/check-action-refs.sh
bash scripts/check-tracked-workspace-evidence.sh
bash scripts/check-srt-pin.sh
bash scripts/test-image-publish-contracts.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSLS_BUILD_TESTS=ON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_FLAGS="-Werror=return-type -Werror=format-security -Werror=nonnull -Werror=address -Werror=sizeof-pointer-memaccess"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## WHERE TO LOOK

| Task / code surface | Contract |
|---|---|
| Before changing anything else here, open docs/agents/README.md and read the contract for the subsystem you touch | [Contract index](docs/agents/README.md) |
| Overview | [overview.md](docs/agents/overview.md) |
| WHAT THIS FORK IS | [what-this-fork-is.md](docs/agents/what-this-fork-is.md) |
| UPSTREAM RELATIONSHIP (bidirectional) | [upstream-relationship-bidirectional.md](docs/agents/upstream-relationship-bidirectional.md) |
| LIBSRT PIN CONTRACT | [libsrt-pin-contract.md](docs/agents/libsrt-pin-contract.md) |
| THE THREE CERALIVE SOCKET OPTIONS (`CERALIVE/srt` `srtcore/srt.h`) | [the-three-ceralive-socket-options-ceralive-srt-srtcore-sr.md](docs/agents/the-three-ceralive-socket-options-ceralive-srt-srtcore-sr.md) |
| CI LANES | [ci-lanes.md](docs/agents/ci-lanes.md) |
| BUILD AND TEST (local) | [build-and-test-local.md](docs/agents/build-and-test-local.md) |
| RELEASE PROCEDURE | [release-procedure.md](docs/agents/release-procedure.md) |
| ANTI-PATTERNS (things the legacy fork did that this base does NOT) | [anti-patterns-things-the-legacy-fork-did-that-this-base-d.md](docs/agents/anti-patterns-things-the-legacy-fork-did-that-this-base-d.md) |
| DOCS DISCIPLINE | [docs-discipline.md](docs/agents/docs-discipline.md) |

## HARD RULES

- Use installed SYSTEM libsrt; no srt submodule. The production/CI install must use the pinned CERALIVE/srt release.
- Keep submodules: spdlog, json, thread-pool, cpp-httplib, CxxUrl.
- Canonical/default branch is `main`; preserve `legacy` history, never force-push or delete it.
- Server source stays upstream except ledgered, tested security ports; offer upstream defects upstream first.
- Keep all four libsrt pin inputs aligned; pair server and device libsrt release pins and reject retired SHAs.
- Socket option numbers 118, 119 and 120 are ABI; never renumber them or add a compat shim to SLS.
- Stock-libsrt negative CI passes only on SRTO_SRTLAPATCHES compile failure; never add a source fallback.
- Release tags are immutable SemVer equal to PROJECT_VERSION; no CalVer, latest, moved tags, or auto-sync.
- Publish only CI-green main ancestors at the exact validated SHA; verify both architecture gates and signed digest before tagging.
- Relay TS opaquely: no receive modes, lossmaxttl tuning, audio-gap concealment, or per-source-IP connection limiter.
- Remove transient upstream remotes before any push/PR; retain only origin.
