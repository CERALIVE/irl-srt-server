<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

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

