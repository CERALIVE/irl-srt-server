# Upstream hard-fork ledger — CERALIVE `irl-srt-server` commits vs upstream `a86dd8a`

Base of comparison: `irlserver/irl-srt-server` HEAD
`a86dd8abd3659baea8ca8315bb4842410d4bc292` (2026-09-13, "feat(core): list each
stream's players in /stats and fix max_players"), which is the hard-clone base for
the CERALIVE SLS rewrite (draft decision D1). SLS behaviour on the new base is
100% upstream (D20/D11): the server source stays byte-identical to upstream, so
only CI/CD and genuinely-not-upstream defect fixes are candidates for carrying
forward.

Two sections, one table each:

- **Section A** — the 19 post-ledger commits in
  `c4557ff07c4025c87166065d0150cc966b7ffbbd..origin/master` (`origin/master` =
  `ae229f96e38ff23ab880913c3c8d9c8e933e0f77`). These were not covered by the
  earlier explorer ledger.
- **Section B** — the earlier explorer ledger rows #1-#19, merged in verbatim
  from the plan draft's D20 decision.

Disposition vocabulary (exactly one per row): `drop`, `port`, `already-upstream`.

Classification rule (draft G2, applied mechanically, no judgment):

1. mechanism present in upstream `a86dd8a` ⇒ `already-upstream` (the hard clone
   already has it; nothing to carry)
2. `ci:` subject ⇒ `port`
3. `fix(auth)` / `fix(core)` subject whose mechanism is absent from `a86dd8a`
   ⇒ `port` (security class)
4. anything touching the SLS receive profiles, the L1-L3 mode table, or
   audio-gap concealment ⇒ `drop`
5. otherwise ⇒ `drop`

Carrying forward is always a **re-implementation** against the upstream tree
(draft D2) — never a `git cherry-pick` across the hard-fork boundary.

Upstream evidence column: an upstream `path:line` anchor when the mechanism is
present at `a86dd8a`, or the literal `none` when a direct search of `a86dd8a`
found no equivalent mechanism.

## Topology note

`origin/master` reaches `c4557ff` through the **second** parent of the PR-#22
merge: `sync/irlserver-ba2b04a` = `c4557ff` + the 18 non-merge commits below, and
`ae229f9` merged that branch into the then-master tip `02fd73e`. Tree-level, the
whole range is `34 files changed, 758 insertions(+), 249 deletions(-)`.
`c4557ff` itself is the fork's merge of upstream `ba2b04a`, and `a86dd8a` is
`ba2b04a` plus one upstream commit — so every Section-A commit is strictly
post-upstream-sync CERALIVE work, and an `already-upstream` verdict there could
only come from upstream having the same mechanism independently.

## Section A — the 19 post-ledger commits (`c4557ff..ae229f9`)

| # | SHA | Subject | Files | Upstream evidence @ `a86dd8a` | Category | Disposition |
|---|-----|---------|-------|-------------------------------|----------|-------------|
| A1 | `bb7f9d05c640e678fd32e5a7dbb1dc91f1492de6` | `fix(auth): advance silent publisher authorization on worker ticks` | `src/core/SLSPublisher.cpp`, `src/core/SLSPublisher.hpp`, `tests/e2e/publisher_auth_probation.sh`, `tests/e2e/sls-publisher-auth.conf` | none | fix(auth) | port |
| A2 | `783ba140c94106266456d06495200bd42d3c953c` | `ci: gate silent publisher authorization probation` | `.github/workflows/ci.yml` | none | ci | port |
| A3 | `e710925a204c12dc6340fee8ef07c44a9a441584` | `fix(auth): bound publisher authorization lifecycle` | `src/core/AsyncHttpClient.cpp`, `src/core/SLSListenerHandler.cpp`, `src/core/SLSPublisher.cpp`, `src/core/SLSRole.cpp`, `src/core/SLSRole.hpp`, `tests/e2e/publisher_auth_probation.sh` | none | fix(auth) | port |
| A4 | `66ac24b7b3193bfac7c8fc90020a30c95ab090a9` | `refactor(auth): add listener-owned publisher admission state` | `src/core/SLSListener.hpp`, `src/core/SLSListenerCore.cpp` | none | refactor(auth) | drop |
| A5 | `93359e24edee3633ff521b962901a0bd80e1cc8a` | `fix(auth): publish publishers only after authorization` | `src/core/SLSListenerHandler.cpp`, `tests/e2e/publisher_auth_probation.sh` | none | fix(auth) | port |
| A6 | `cbe93daee7c26c1f412c8e4ac395d6839d70f31c` | `fix(auth): serialize pending admission with teardown` | `src/core/SLSListenerHandler.cpp`, `tests/e2e/publisher_auth_probation.sh` | none | fix(auth) | port |
| A7 | `5efa27e950bd8b4a7519804cbe862ea5730481da` | `fix(core): close rts generation race` | `src/core/TSFileTimeReader.cpp` | none | fix(core) | port |
| A8 | `346346e382cc61adfb9c8dbf56b72b42f7a9f368` | `refactor(config): make parser paths tidy-clean` | `src/core/common.cpp`, `src/core/common.hpp`, `src/core/conf.cpp`, `src/core/conf.hpp` | `src/core/conf.cpp`, `src/core/common.cpp` (upstream owns these files in its own form) | refactor | drop |
| A9 | `3216cfe0e5f23f9bcfa614849978f60318b098fb` | `refactor(srt): make transport declarations tidy-clean` | `src/core/SLSSrt.cpp`, `src/core/SLSSrt.hpp` | `src/core/SLSSrt.hpp` (upstream owns this file in its own form) | refactor | drop |
| A10 | `93f0132b3e96245d109dcabd075d30aed4cdd7f6` | `refactor(auth): make authorization paths tidy-clean` | `src/core/AsyncHttpClient.cpp`, `src/core/SLSRole.cpp`, `src/core/SLSRole.hpp` | `src/core/SLSRole.cpp:920` (upstream owns `check_http_passed` and the surrounding role paths) | refactor | drop |
| A11 | `3a603f1ec16e45817d2a2392f665d3413fa16d2f` | `refactor(core): make stream diagnostics tidy-clean` | `src/core/SLSMapData.cpp`, `src/core/SLSPublisher.cpp`, `src/core/SLSRecycleArray.cpp` | `src/core/SLSMapData.cpp` (upstream owns these files in its own form) | refactor | drop |
| A12 | `a8ebe94d581e542cb6252287c7ed59c2ba41535b` | `refactor(relay): avoid relay config copies` | `src/core/SLSMapRelay.cpp`, `src/core/SLSMapRelay.hpp` | `src/core/SLSMapRelay.hpp` (upstream owns this file in its own form) | refactor | drop |
| A13 | `a95750029cdd84f9fed879a508cc04a11082ec5b` | `refactor(relay): make puller paths tidy-clean` | `src/core/SLSPuller.cpp`, `src/core/SLSPullerManager.cpp` | `src/core/SLSPullerManager.cpp` (upstream owns these files in its own form) | refactor | drop |
| A14 | `24413d414db60bff1c8aad84482154e959972c62` | `refactor(relay): make pusher paths tidy-clean` | `src/core/SLSPusherManager.cpp`, `src/core/SLSRelay.cpp`, `src/core/SLSRelayManager.cpp` | `src/core/SLSPusherManager.cpp` (upstream owns these files in its own form) | refactor | drop |
| A15 | `23a37b582b7cebf2cbdae91bfee4e903e5e8b66c` | `refactor(client): make client paths tidy-clean` | `src/core/SLSClient.cpp`, `src/core/SLSSyncClock.cpp`, `src/srt-live-client.cpp` | `src/core/SLSClient.cpp` (upstream owns these files in its own form) | refactor | drop |
| A16 | `3feb14eb218752dde24e1ed6df6d2bca2346fa2f` | `refactor(listener): make listener paths tidy-clean` | `src/core/SLSListenerConfig.cpp`, `src/core/SLSListenerHandler.cpp` | `src/core/SLSListenerHandler.cpp` (upstream owns these files in its own form) | refactor | drop |
| A17 | `a9056a5e5516cf8e6c5df44a36ee2893aa246c3e` | `refactor(stats): make publisher stats tidy-clean` | `src/core/SLSManager.cpp`, `src/core/SLSManager.hpp` | `src/core/SLSManager.hpp` (upstream owns this file in its own form) | refactor | drop |
| A18 | `a9e59c412655843b588e52a24cf1a47e6a41a3d8` | `style(core): format reconnect worker` | `src/core/SLSGroup.cpp` | `src/core/SLSGroup.cpp` (upstream owns this file in its own form) | style | drop |
| A19 | `ae229f96e38ff23ab880913c3c8d9c8e933e0f77` | `Merge pull request #22 from CERALIVE/sync/irlserver-ba2b04a` | (merge commit — no content of its own) | n/a (merge of A1-A18 into `02fd73e`) | merge | drop |

Row count: 19 (18 non-merge + 1 merge commit).

### Section A per-row rationale

- **A1 `bb7f9d0` — port.** Adds a `CSLSPublisher::on_worker_tick()` override that
  calls `check_http_passed()` on every worker pass, so a publisher that connects
  and then sends nothing still has its pending server-level authorization
  advanced. Upstream has the tick hook itself (`src/core/SLSRole.hpp:98` base
  no-op, dispatched at `src/core/SLSGroup.cpp:274`) and a `CSLSListener`
  override for **player**-key validation
  (`src/core/SLSListenerHandler.cpp:1172`), but there is no `CSLSPublisher`
  override and therefore no publisher-side equivalent. A direct search of
  `a86dd8a` for the publisher-side mechanism returns nothing.
- **A2 `783ba14` — port.** Adds a debug-matrix CI step running
  `tests/e2e/publisher_auth_probation.sh`. Upstream's `ci.yml` runs only
  `tests/e2e/stats_snapshot.sh` (:569) and `tests/e2e/reconnect_replay.sh`
  (:575); it has no probation gate and no such script. `ci:` ⇒ carry forward.
  Depends on the e2e script introduced by A1; the re-implementation lands the
  script and the gate together.
- **A3 `e710925` — port.** Introduces `m_http_auth_deadline_ms` plus
  `kHttpAuthorizationDeadlineMs` / `kHttpRequestTimeoutSeconds`, a `set_max_timeout`
  ceiling on the httplib client, fail-closed handling when a gate is configured
  but no request is in flight, and `on_connect()` returning `SLS_OK` when no
  webhook URL is configured. None of those symbols exist anywhere in `a86dd8a`.
- **A4 `66ac24b` — drop.** `PendingPublisherConnection`,
  `m_pending_publisher_connections`, `finish_publisher_accept`, and
  `drive_pending_publisher_connections` are all absent from `a86dd8a`, so this is
  not upstream — but the subject is `refactor(auth)`, which rule 3 does not
  admit, and rule 5 therefore applies. It is a pure structural prerequisite of A5
  and A6: no behaviour of its own. Because the plan carries A5/A6 forward as
  re-implementations rather than cherry-picks (D2), the listener-owned admission
  struct is authored as part of those two rows and never as its own commit. This
  row is `drop` as a *commit*, not as a *mechanism* — the follow-up task that
  lands A5/A6 must create the equivalent state or A5/A6 will not compile.
- **A5 `93359e2` — port.** Replaces publisher takeover-at-accept with deferred,
  listener-owned admission: an accepted publisher socket is held (capped at
  `MAX_PENDING_PUBLISHER_CONNECTIONS = 1024`, deduplicated per
  `key_stream_name`) and is only entered into `m_map_publisher` / handed to a
  role worker after authorization succeeds, so concurrent stats and takeover
  logic can never observe a partially admitted role. Absent from `a86dd8a`.
- **A6 `cbe93da` — port.** Takes the listener mutex at the top of
  `CSLSListener::on_worker_tick()` and returns early when the listener socket is
  gone or invalid, serializing pending admission against teardown. Upstream's
  `on_worker_tick` (`src/core/SLSListenerHandler.cpp:1172`) takes no such lock
  and has no such guard.
- **A7 `5efa27e` — port.** Replaces a `stat()`-then-`open(O_WRONLY|O_CREAT)`
  time-of-check/time-of-use window with a single atomic
  `open(O_WRONLY|O_CREAT|O_EXCL)` plus `EEXIST` handling, and corrects two
  `fd <= 0` checks to `fd < 0` (fd `0` is a legal descriptor). Upstream still
  has both the `stat()` pre-check (`src/core/TSFileTimeReader.cpp:198`) and the
  non-exclusive create (`src/core/TSFileTimeReader.cpp:212`), so the defect is
  live upstream and the fix is not there.
- **A8-A18 — drop.** Eleven `refactor(...)`/`style(...)` "tidy-clean" commits: no
  behaviour change, only clang-tidy-driven const/reference/loop/declaration
  cleanups and formatting on fork-local source. The hard clone takes upstream's
  copy of every one of these files byte-identical (D11/D20), so there is nothing
  for a cleanup of the fork's own divergent copy to apply to. None of them matches
  a rule-2 or rule-3 subject.
- **A19 `ae229f9` — drop.** The PR-#22 merge commit. It carries no content beyond
  A1-A18, which are individually classified above.

## Section B — earlier explorer ledger (rows #1-#19, from draft D20)

| # | Item | Upstream evidence @ `a86dd8a` | Category | Disposition |
|---|------|-------------------------------|----------|-------------|
| B1 | Security remediation bundle (part 1) | `src/core/SLSManager.hpp:124-140` | security | already-upstream |
| B2 | Per-source-IP handshake `ConnRateLimiter` | none | security | drop |
| B3 | Security remediation bundle (part 2) | `src/core/SLSMapData.cpp:70-128` | security | already-upstream |
| B4 | Security remediation bundle (part 3) | `src/core/SLSRole.cpp:998-1007` | security | already-upstream |
| B5 | Audio-gap concealment filler | removed upstream in `47e3ac4` | feature | drop |
| B6 | Publisher stats | upstream `src/core/SLSManager.cpp` stats path | feature | already-upstream |
| B7 | Viewer replay / egress | upstream PR #16 | feature | already-upstream |
| B8 | Stock-libsrt compat probe (CI leg only) | none | ci | port |
| B9 | SLS receive profiles + L1-L3 mode table | none | feature | drop |
| B10 | Native CI matrix (CERALIVE-srt leg, ASan/UBSan/TSan, changed-line clang-format, baselined clang-tidy, fuzz smoke, coverage, API/reconnect E2E) | none | ci | port |
| B11 | Docker build-check (multi-arch, binary verification, Trivy, CycloneDX SBOM, advisory CodeQL) | none | ci | port |
| B12 | `publish-image.yml` + its contract scripts (retagged semver per D18) | none | ci | port |
| B13 | `scripts/check-srt-pin.sh` (4-site pin contract) | none | ci | port |
| B14 | Repository contract scripts (`check-action-refs.sh`, `check-tracked-workspace-evidence.sh`, `validate-image-release.sh`, `check-image-tags-unused.sh`, `check-image-publish-workflow.rb`, `test-image-publish-contracts.sh`) | none | ci | port |
| B15 | Fuzz CI policy | none | ci | port |
| B16 | Config / loopback E2E (`srt_loopback.sh`, multi-listener fixture) | none | ci | port |
| B17 | C++ RAII / concurrency cleanup | upstream `src/core/SLSLock.hpp` + role lifecycle | refactor | already-upstream |
| B18 | Fork documentation, part 1 (verbatim fork docs) | none | docs | drop |
| B19 | Fork documentation, part 2 (verbatim fork docs) | none | docs | drop |

Row count: 19.

### Section B notes

- **B2 `drop`, not `already-upstream`.** The per-source-IP handshake limiter
  never reached upstream, but SLS is deployed behind a protecting proxy, so every
  client presents the proxy's source address and the limiter would throttle
  legitimate traffic collectively — a regression, not a hardening.
- **B5 `drop`.** Upstream deliberately removed gap concealment in `47e3ac4`:
  concealment moves to an OBS media-source plugin on the playback side and the
  server relays the TS opaquely again. Rule 4 also excludes it outright.
- **B9 `drop`.** Rule 4: the receive-profile table and its L1-L3 tiers are
  explicitly out of scope for the new base.
- **B8 `port` is narrow.** Only the CI leg that proves SLS still builds against
  stock Haivision libsrt is carried; no source-level fallbacks, because the SLS
  source stays byte-identical to upstream. What makes it compile is the compat
  enumerator in the CERALIVE `srt` fork, not a change here.

## Roll-up

| Disposition | Section A | Section B | Total |
|-------------|-----------|-----------|-------|
| `port` | 6 | 8 | 14 |
| `drop` | 13 | 5 | 18 |
| `already-upstream` | 0 | 6 | 6 |
| **Total rows** | **19** | **19** | **38** |

Every Section-A `port` row has a `fix(auth)`, `fix(core)`, or `ci:` subject and
upstream evidence `none`. No row whose subject or item text names the receive
profiles, the mode table, or audio-gap concealment is carried forward.

This ledger is classification only. Nothing from either section has been applied
to the tree by the change that introduced this file.
