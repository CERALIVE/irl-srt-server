# Todo 27: cutover doc, profile contract, transient remote, receiver PR

## status: pending-owner

The pre-declared Alternative B was taken. Independently verified at execution time:

- `gh api repos/CERALIVE/srt/git/refs/tags/srt-v1.5.7+ceralive.1` → 404 Not Found (tag absent).
- `gh pr view 24 -R CERALIVE/srt --json state,mergedAt` → `{"mergedAt":null,"state":"OPEN"}`.

So the libsrt release does not exist yet, and the merged SHA the Dockerfile must be
re-pinned to is unknowable. Per the plan, nothing was waited on and nothing was
triggered. All executor-side work is committed on `feat/bonded-path-convergence`
with the Dockerfile STILL at the branch-tip pin
`ARG SRT_COMMIT=ca14c8bd06c89d2fd7b69bb3d8eea48dd47c2e3e`.

## Clearing commands (run verbatim once the owner has merged, tagged, and dispatched)

Values substituted from `docs/evidence/bpc/srt-release.json` in the sender repo:
`srt_tag` = `srt-v1.5.7+ceralive.1`, `deb_assets` =
`["libsrt1.5-ceralive_1.5.7+ceralive.1_amd64.deb","libsrt1.5-ceralive_1.5.7+ceralive.1_arm64.deb"]`
(`merged_upstream: true`, so the primary tag applies, not the `srt-v1.5.6+ceralive.2` fallback).

Completion proof for the manual `publish-release.yml` dispatch (the GitHub release
object is created before that workflow's later steps run, so `gh release view`
alone proves nothing):

```
gh run list -R CERALIVE/srt --workflow publish-release.yml --branch srt-v1.5.7+ceralive.1 --status success --json conclusion | jq -e '.[0].conclusion=="success"'
```

Literal asset check (string equality via `index`, never `test()`):

```
gh release view srt-v1.5.7+ceralive.1 -R CERALIVE/srt --json assets | jq -e --argjson want '["libsrt1.5-ceralive_1.5.7+ceralive.1_amd64.deb","libsrt1.5-ceralive_1.5.7+ceralive.1_arm64.deb"]' '(.assets|map(.name)) as $have | all($want[]; . as $w | ($have|index($w)) != null)'
```

Both must exit 0. Then, and only then, the single remaining action:

1. `SHA=$(git ls-remote https://github.com/CERALIVE/srt.git refs/tags/srt-v1.5.7+ceralive.1^{} | cut -f1)` (fall back to the non-peeled ref if the tag is lightweight).
2. Set that SHA in `Dockerfile` `ARG SRT_COMMIT`, the three `.github/workflows/ci.yml` `SRT_COMMIT` values, and `scripts/check-srt-pin.sh` `EXPECTED_PIN`; move the branch tip `ca14c8bd06c89d2fd7b69bb3d8eea48dd47c2e3e` into `RETIRED_PINS`.
3. `bash scripts/check-srt-pin.sh && docker build .` must both exit 0.
4. Update the "Build pin" line in the prepared PR body and the pin sentences in `AGENTS.md` / `README.md` / `docs/bonded-policy-cutover.md`.
5. Commit, push, and open the ONE receiver PR from the prepared body (see below).

## What landed in this todo

| Item | Location |
|---|---|
| Cutover doc: converged policy table, preconditions, four-step runbook with `/stats` soak table, `SLS_BONDED_PROFILE_OVERRIDE` rollback, known limitations with measured numbers, 4003 removal timeline | `docs/bonded-policy-cutover.md` |
| Profile contract rewritten: one bonded policy + L3, structural-impossibility sentence, TTL* = 200 with the M1 / Todo 24 citation, controller explicitly not built, cutover link, transient-remote policy | `AGENTS.md` → SRT DEPENDENCY, RECEIVE PROFILES, COMMON TASKS, NOTES |
| `irlserver` remote removed from the repo (`git remote -v` shows only `origin`) | repo config, not a tracked file |
| Prepared PR body (policy table, runbook link, M1/M3/Todo 24/twin-port numbers, CI) | sender repo `docs/evidence/bpc/pr-bodies/irl-srt-server.md` (untracked there; that repo's own todo commits it) |

Not done, by design: no PR opened, no `ARG SRT_COMMIT` change, no L3 change.

## Gates

| Gate | Result |
|---|---|
| `docker build .` at the branch-tip pin | PASS, `EXIT=0`; `100% tests passed, 0 tests failed out of 10`, total test time 128.98 s; startup logged `SRT compat mode: reorderfreeze+periodicnakgate`. The netem loss leg self-skipped without NET_ADMIN as expected. Log: `test-results/task-27-attempt-1/docker-build.log` (local, gitignored). |
| `git remote -v \| grep -c irlserver` | 0 |
| `grep -c SLS_BONDED_PROFILE_OVERRIDE docs/bonded-policy-cutover.md` | ≥ 1 |

## Numbers cited (sources)

- M1: sender `docs/evidence/bpc/m1-ttl/spike.json` (TTL* 200, freeze penalty 644.64892578125 ms, owned passes 0/3/2 of 8, 24-Mbit best 200).
- M3: sender `docs/evidence/bpc/m3-interop/spike.json` (quadrants 25/6 vs 48/39 of 48; C rows belabox-c 0/3 @1.158, irlserver-rust-enhanced 1/3 @1.459, irlserver-rust-classic 3/3 @1.763, ours-3.3.0 0/3 @1.751; ours-new C 0/3 @1.016 and M1 0/3 @1.151).
- Todo 24: this repo `docs/evidence/bpc/task-24-lossmaxttl.md` (TTL200 retransmissions 40.591255 / 26.554053 / 51.584450 %, TTL500 0.505766 / 0.192057 / 0.313309 %).
- M4: sender `docs/evidence/bpc/m4/README.md` + `verdict.json` (ours-new SLS4003 M1/M4/M6 9/9 settled, `retransmit_unknown`, zero lineage passes).
- S-TWINPORT: sender `docs/evidence/bpc/twinport/spike.json` (`outcome: distinct`, `d_goodput_pct: null`, `d_loss_pp: null`).
