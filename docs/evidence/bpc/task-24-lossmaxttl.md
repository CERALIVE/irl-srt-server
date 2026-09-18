# Todo 24: measured static LOSSMAXTTL, with the M3 blocker extension

## Decision

**Original TTL*=200 → combined TTL*=200. Controller=false → false.**
`src/core/SLSSrt.cpp` retains `kBondedLossMaxTtl=200`, now documented as M1's
measured upstream-parity fallback rather than an unmeasured placeholder. L1 and
L2 use it; explicit legacy overrides retain 40; L3 remains byte-unchanged at 200.

No live-change spike or bitrate controller is required. No runtime config surface,
NAKREPORT change, or periodic-NAK-gate mutation was introduced.

**The released-sender C performance failure is not relabelled a known foreign
limitation or a PASS.** Todo 24 performed the mandated blocker re-evaluation;
TTL200 still fails C. Rollout remains owner-gated, with this residual explicitly
visible. Nothing was pushed or published by this task.

## Frozen-rule extension, not a full re-sweep

The reference is sender `scripts/bench/{m1_rule,m1_models,m1_report,report}.py`
and `docs/evidence/bpc/m1-ttl/{method,report}.md`, `summary.json`, `spike.json`.
The original 65 cells / 195 observations are retained unchanged. One additional
sender/scenario pair, released `ours-3.3.0` enhanced / C, was measured at each of
40/200/500, N=3: **nine new one-attempt outcomes**, 68 combined cells / 204 outcomes.
Production SRT preset, seed 20260913, catalog scenario-C windows, freeze on,
NAK on, periodic gate on; no source or receiver substitution.

The current historical runner resolves the release through its existing
`m3-interop` lock. That campaign identifier is retained for the supplementary
manifest so the read-only sender checkout and lock need no changes. Reduction
uses the existing `load_records(..., m3_outcomes=True)`, configuration-integrity
checks, M1 outcome models and bootstrap statistics. Only full-window successes
or measured `settle_timeout` observations qualify; infrastructure failures do not.

The local decision adapter first runs the original `m1_rule.decide` on the
original summary; its output matches the original spike byte-for-byte (SHA256
`5abb87999b500d4d01314b7be08f74a240325c5a52dcfd99559fba6bc48fe7b9`). It then
includes the added owned cell using M1's strict 3/3 settled and <=5% retransmission
criterion, not the foreign-only 2/3 and <=10% allowance. Consequently the complete
population sizes become 9 owned and 15 total cells per TTL, rather than 8/14.
All branch precedence, tie breaks, freeze thresholds, diagnostic selection and
controller thresholds remain M1's. Synthetic tests separately establish that
the added cell can change selection and trigger a controller, and that missing
TTLs or duplicate indices are rejected. No synthetic values enter the evidence.

## New observations

| TTL | Settled | Owned joint pass | Retransmissions % (indices 0/1/2) | Useful Mbit/s (indices 0/1/2) |
|---|---|---|---|---|
| 40 | 1/3 | 0/3 | 55.835962 / 58.573583 / 55.339310 | 11.566061 / 12.275999 / 12.732212 |
| 200 | 3/3 | 0/3 | 40.591255 / 26.554053 / 51.584450 | 15.826216 / 18.693517 / 14.921861 |
| 500 | 3/3 | 3/3 | 0.505766 / 0.192057 / 0.313309 | 22.401654 / 22.404812 / 22.403409 |

The same pass/fail classification results even under M3's looser 2/3, <=10%
criterion. TTL500 helps this added cell; it does not make the combined matrix pass.

| TTL | Original owned / 8 | Combined owned / 9 | Original total / 14 | Combined total / 15 |
|---|---:|---:|---:|---:|
| 40 | 0 | 0 | 3 | 3 |
| 200 | 3 | 3 | 8 | 8 |
| 500 | 2 | 3 | 8 | 9 |

The no-complete-owned-TTL branch takes precedence over the total-cell score:
`core_failure_upstream_parity` selects 200. Independently, the original maximum
identifiable freeze penalty remains >=644.64892578125 ms (>250 ms), capping any
choice at 200. L3 keeps freeze off; a one-link bond inherits the accepted penalty;
freeze and TTL must be considered together. No claim of a cure follows from
Todo 8's compensating in-flight-aware NAK mechanism.

The original 24-Mbit diagnostic remains unchanged:

| TTL | Median useful Mbit/s | 95% bootstrap median CI |
|---|---:|---|
| 40 | 4.495222 | [4.398832, 4.511599] |
| 200 | 5.432214 | [5.381446, 5.507314] |
| 500 | 5.262830 | [5.250665, 5.357114] |

Best TTL=200 equals the combined static choice; relative gap=0%, strict CI
separation=false. Thus the controller conjunction is false after re-evaluation,
not merely copied from the original spike. M-TTL-LIVE and its conditional four
controller tests are not applicable.

## Provenance and limits

- Released sender SHA256: `ebe34ee2bb6c8d832801e20ec32adc7cf8aba9c544915de751efad14f8f1d77c`,
  locked v3.3.0 `.deb` artifact, not rebuilt or replaced by the current sender.
- Receiver SRT source: `ca14c8bd06c89d2fd7b69bb3d8eea48dd47c2e3e`.
  `srt-live-transmit` SHA256: `1610a06b83070bcf5297cac730e954fdae496975a3d06258482949d21fafcb69`.
- CeraLive SRTLA receiver SHA256: `fa3524c844d1cd7f805fecc0b06fc68f1594c3a4b9e53cf27b7ea37a0c1e6ab7`.
- Attempt 1 was stopped after the workspace filesystem filled. Its partial
  observations are excluded. Only this checkout's generated sanitizer outputs
  were cleaned; attempt 2 used the same manifest with raw captures on another
  filesystem. No retries or cherry-picked replacement indices were pooled.
- Attempt 2: sequential namespace/netem measurement, CPUs 4–27, shared host lock,
  1800-second external bound; completed in **763.56 seconds**. Runner exit **101**
  reflects the two retained TTL40 settling failures, not missing measurements.
  Reduction exit **0**, all nine observations present (seven settled, two measured
  timeouts). This is not real-radio validation or an independently idle-host claim.
- Local attempt report: `test-results/task-24-attempt-2/task-24-bonded-path-convergence.md`.
  It identifies the exact manifest, adapter, raw artifacts, test logs and commands.
  Portable measurements and hashes are in `task-24-lossmaxttl.json` beside this file.

## Profile verification

Tests were changed before the production annotation: a real loopback publisher
connects to each profile, SLS accepts it, and independent literal expectations
check inherited LOSSMAXTTL plus initial reorder tolerance. Existing fresh-process
CTest override lanes also exercise this new case under converged, both legacy
overrides, and invalid-override fallback. L3's literal table is unchanged.

A temporary bonded TTL40 mutation produced the expected `40 != 200` failures
on both accepted bonded sockets; it was restored to measured 200. Because the
selected number equals the original placeholder, this is a falsifiability check,
not a claim that the pre-task numeric behavior was wrong.

The referenced SRT library classifies LOSSMAXTTL as `SRTO_POST_SPEC`, range
0..INT_MAX, and copies listener configuration to accepted sockets. No post-connect
change is necessary on this static branch. NAKREPORT/gate remain pre-connect only.

| Gate | Result |
|---|---|
| `docker build .` | PASS, exit 0; all 10 CTests passed |
| Local CMake build / CTest | Build PASS; host run 9/10, loopback blocked by an existing process on UDP4001; only failed loopback rerun in a private network namespace PASS (1/1) |
| ASan + UBSan | Build and CTest PASS, 9/9 |
| TSan | Build and CTest PASS, 9/9 |
| Changed C++ LSP diagnostics | No errors using the existing Clang `build-lsp` compilation database |

Docker's netem loss/differential leg explicitly self-skipped without NET_ADMIN;
its real connectivity/media loopback passed. Sanitizer suites intentionally omit
the FFmpeg loopback. The pre-existing nonblocking `conf_parse` characterization
policy is unchanged. Local port isolation did not stop or alter the unrelated
server. Initial clangd use of the GCC database reported unsupported
`-fcompare-debug-second`; a temporary, uncommitted `.clangd` selected the existing
Clang database and was removed after diagnostics. Existing legacy warnings/hints
were not converted into unrelated source edits. No successful gate was rerun.
