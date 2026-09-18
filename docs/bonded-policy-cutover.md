# Bonded policy cutover: one NAK-on gated profile on both bonded ports

Status: **pending owner action**. The receiver code, the profile contract, and this
runbook are complete on `feat/bonded-path-convergence`. The Dockerfile still pins
the CERALIVE/srt branch tip (`ca14c8bd06c89d2fd7b69bb3d8eea48dd47c2e3e`), not a
release. The single remaining engineering step is the re-pin to the merged
`srt-v1.5.7+ceralive.1` SHA plus the receiver PR, which happens only after the
libsrt release exists. See [Preconditions](#preconditions) for the exact checks.

## Converged policy

Before this change the two bonded listeners carried different policies, and the
freeze/NAK/TTL choice was a placeholder inherited from the historical 30-vs-40
calibration. After it, both bonded ports carry ONE policy. L3 is byte-unchanged.

| Listener | `sls.conf` directive | Before | After |
|---|---|---|---|
| L1 | `listen_publisher_srtla` (4002) | freeze, NAK on, TTL40, floor100, FEC, no gate | freeze, NAK on, **periodic NAK gate**, **TTL200**, floor100, FEC |
| L2 | `listen_publisher_srtla_classic` (4003) | freeze, NAK **off**, TTL40, floor100, no FEC, no gate | **identical to L1** (deprecated alias) |
| L3 | `listen_publisher` / player / fallback | no freeze, default NAK, TTL200, no floor, no FEC | unchanged |

Startup emits one policy line per listener. The two bonded lines must read
identically apart from the profile name:

```
profile=L1-bonded freeze=1 nakreport=1 periodic_nak_gate=1 lossmaxttl=200 floor=100 fec_accept=1
profile=L2-bonded-alias freeze=1 nakreport=1 periodic_nak_gate=1 lossmaxttl=200 floor=100 fec_accept=1
profile=L3-direct freeze=0 nakreport=default periodic_nak_gate=0 lossmaxttl=200 floor=0 fec_accept=0
```

Together with `SRT compat mode: reorderfreeze+periodicnakgate`, those three lines
are the proof that a running image took the converged path. A build against
stock or BELABOX libsrt refuses bonded startup with
`bonded profile requires SRTO_PERIODICNAKGATE (libsrt >= 1.5.7+ceralive.1); refusing to start listener`
and exits nonzero. There's no silent downgrade.

### Why the policy is static

`lossmaxttl=200` is the measured upstream-parity fallback from spike M1, not a
tuned optimum. M1's frozen rule chose `core_failure_upstream_parity: TTL*=200`
because no TTL passed every owned cell (owned passes per TTL: 40 → 0/8,
200 → 3/8, 500 → 2/8) and the maximum identifiable freeze penalty of
644.64892578125 ms (>250 ms) independently caps the choice at 200. Todo 24
re-ran that rule with the released `ours-3.3.0` / scenario-C cell added
(nine new observations, 68 cells / 204 outcomes total): combined TTL* stayed
200 and the controller conjunction stayed false (24-Mbit diagnostic best TTL
200, relative gap 0%, strict CI separation false). There's no bitrate-bucket
controller, no live sockopt change, and no new `sls.conf` directive.

Full decision record: [`docs/evidence/bpc/task-24-lossmaxttl.md`](evidence/bpc/task-24-lossmaxttl.md).

### Why it can't be negotiated per stream

Per-streamid policy is structurally impossible here. `srtla_rec` is libsrt-free
and the SRT handshake terminates at the encoder, so the receiver never sees the
SRTLA sender's lineage. The only levers the receiver holds are TTL and the gate,
and those must be fixed before `accept()` because libsrt copies listener options
onto accepted sockets.

## Preconditions

Do not start the rollout until all four hold.

1. `CERALIVE/srt` PR #24 is merged and tagged `srt-v1.5.7+ceralive.1`.
2. The manual `publish-release.yml` dispatch for that tag has COMPLETED. The
   GitHub release object is created before that workflow's later steps run, so
   `gh release view` alone proves nothing. Run:

   ```
   gh run list -R CERALIVE/srt --workflow publish-release.yml --branch srt-v1.5.7+ceralive.1 --status success --json conclusion | jq -e '.[0].conclusion=="success"'
   ```

3. Both `.deb` assets are attached, checked by literal name (string equality via
   `index`, never a regex):

   ```
   gh release view srt-v1.5.7+ceralive.1 -R CERALIVE/srt --json assets | jq -e --argjson want '["libsrt1.5-ceralive_1.5.7+ceralive.1_amd64.deb","libsrt1.5-ceralive_1.5.7+ceralive.1_arm64.deb"]' '(.assets|map(.name)) as $have | all($want[]; . as $w | ($have|index($w)) != null)'
   ```

4. This repo's `ARG SRT_COMMIT`, the three `ci.yml` `SRT_COMMIT` values, and
   `scripts/check-srt-pin.sh`'s `EXPECTED_PIN` are re-pinned together to the SHA
   the tag resolves to, and `docker build .` is green on that pin.

## Deploy runbook

Each step is independently revertible. Do not collapse them.

### Step 1: libsrt release

Owner action (preconditions 1 to 3 above). Nothing in this repo moves until the
tag and its assets exist.

### Step 2: receiver image

Re-pin (precondition 4), open the receiver PR, merge to `master`, then publish
the image through the manual, fail-closed `Publish Image` workflow as described in
[`docs/IMAGE-RELEASE.md`](IMAGE-RELEASE.md). Verify the signed manifest digest.
Confirm the container log shows `SRT compat mode: reorderfreeze+periodicnakgate`
and the three policy lines above.

### Step 3: receiver rollout on BOTH ports

Roll the image out with 4002 and 4003 both served by the new binary. Because the
aliases share one policy, a device landing on either port gets the same
behavior. Every bonded publisher reconnects exactly once during the restart;
that's the expected cost, because NAKREPORT and the gate are pre-connect options
and can't change on a live socket.

Do NOT flip platform routing in this step.

### Step 4: soak for at least 24 hours

Watch the authenticated `/stats` endpoint per publisher. Keys and their sources
are catalogued in the sender repo's `docs/evidence/bpc/sls-stats-map.json`.

| What to watch | `/stats` key | Healthy | Investigate |
|---|---|---|---|
| Reorder hold (freeze working, not starving) | `msRcvBuf`, `latency` | `msRcvBuf` hovers near negotiated `latency`, no sustained climb | `msRcvBuf` pinned at `latency` for minutes: the freeze is holding the buffer at its ceiling |
| Drops | `pktRcvDrop`, `bytesRcvDrop`, `viewerPktSndDrop` | flat or slowly rising under real loss | step increases with no matching `pktRcvLoss` change: TTL200 hold is expiring packets |
| Retransmit pressure | `pktRcvRetrans`, `pktRecvNAKTotal`, `pktSentNAKTotal` | NAK rate tracks `pktRcvLoss` | `pktSentNAKTotal` growing while `pktRcvRetrans` doesn't: the gate is suppressing NAKs the sender never answers |
| Reconnects | `uptime` (publisher and per player), `ingestDiscontinuities` | `uptime` monotonic after the one rollout reconnect | repeated `uptime` resets or `ingestDiscontinuities` steps: the device is bouncing |
| Ring pressure | `ringOverruns`, `sendBackpressure`, `maxReaderBacklogMs` | zero / bounded | any `ringOverruns` step |

A soak that shows none of the investigate conditions on the production sender
population clears Step 5. A soak that shows them is the trigger for
[Rollback](#rollback), not for tuning.

### Step 5: platform routing flip (todo 28)

The last step, and the only one that touches `ceralive-platform`. Flip routing so
new bonded sessions land on 4002. This step is independently revertible: undo
the platform change and sessions land back on 4003, which still serves the same
converged policy. Nothing in this repo participates in that flip.

## Rollback

Both bonded listeners switch together with one environment variable, read once by
`libsrt_init` before any listener is created and logged at INFO:

| `SLS_BONDED_PROFILE_OVERRIDE` | Both bonded listeners |
|---|---|
| `converged` (default, or unset) | freeze, NAK on, gate on, TTL200, floor100, FEC |
| `legacy-l1` | freeze, NAK on, gate off, TTL40, floor100, FEC |
| `legacy-l2` | freeze, NAK off, gate off, TTL40, floor100, no FEC |

Procedure:

1. Set `SLS_BONDED_PROFILE_OVERRIDE=legacy-l1` (the pre-cutover 4002 policy) or
   `legacy-l2` (the pre-cutover 4003 policy) in the container environment.
2. Restart the process. A config reload does NOT reread the environment.
3. Confirm the INFO line names the override and the two bonded policy lines read
   `periodic_nak_gate=0 lossmaxttl=40`.

Each device reconnects once, for the same reason the rollout caused one
reconnect: NAKREPORT and the gate are pre-connect options. An unknown value warns
and keeps `converged`; it never silently downgrades. L3 is never changed by any
value. The override does not need a rebuild, and it does not need the platform
flip to be undone first; the two are independent.

## Known limitations

These are measured, not projected. Scenario C is the high-rate, loss-plus-reorder
cell that stresses the TTL hold hardest.

### Foreign senders on the converged receiver (M3, N=3 per cell)

Ratios are `new_median_bps / old_median_bps` for the converged receiver against
the pre-cutover one. Passing means the run settled AND met the retransmission
criterion.

| Sender | Scenario | Passing runs | Goodput ratio | Disposition |
|---|---|---:|---:|---|
| `belabox-c` | C | 0/3 | 1.158 | `known_limitation` |
| `irlserver-rust-enhanced` | C | 1/3 | 1.459 | `known_limitation` |
| `irlserver-rust-classic` | all four | 12/12 | 1.76 on C | pass |

Across all four existing senders, the converged receiver settled 48/48 runs and
passed 39/48; the pre-cutover receiver settled 25/48 and passed 6/48. The
foreign C failures are a retransmission-criterion miss on a link that already
carries more goodput than before, and the receiver has no lever to fix them
short of a per-lineage policy, which it can't have (see above).

### Released CeraLive sender 3.3.0 on scenario C (Todo 24 re-evaluation)

M3 flagged this cell `receiver_pr_blocker` (0/3 passing, ratio 1.751). Todo 24
re-measured it at TTL 40/200/500, N=3 each, under M1's strict 3/3 settled and
<=5% retransmission criterion:

| TTL | Settled | Owned joint pass | Retransmissions % (runs 0/1/2) | Useful Mbit/s (runs 0/1/2) |
|---|---|---|---|---|
| 40 | 1/3 | 0/3 | 55.835962 / 58.573583 / 55.339310 | 11.566061 / 12.275999 / 12.732212 |
| 200 | 3/3 | 0/3 | 40.591255 / 26.554053 / 51.584450 | 15.826216 / 18.693517 / 14.921861 |
| 500 | 3/3 | 3/3 | 0.505766 / 0.192057 / 0.313309 | 22.401654 / 22.404812 / 22.403409 |

TTL500 fixes this one cell but does not make the combined matrix pass, and M1's
freeze cap forbids it. So at the shipped TTL200, released 3.3.0 on scenario C
still fails the retransmission criterion. This is NOT relabelled as a foreign
limitation or a pass. It's the residual the owner accepts by dispatching the
rollout. The same classification results under M3's looser 2/3, <=10% rule.

### The new CeraLive sender

The original M3 spike listed `ours-new` as failing `["C","M1"]` on the converged
receiver at 200 (C: 0/3, ratio 1.016; M1: 0/3, ratio 1.151). That figure is
superseded by the M4 lineage campaign, which measured `ours-new` on SLS port
4003 over M1/M4/M6 (9/9 settled, all registration/carry/latency conformance
passing) but could not prove retransmission <=10% because the SLS publisher
stats expose no received-packet denominator (`retransmit_unknown`). M4's final
verdict recorded zero lineage passes across all five lineages and shipped
`enhanced` as the only member. Treat the new sender's C/M1 performance as
unproven on this receiver, not as passing.

### Twin-port equivalence

The S-TWINPORT spike could not prove 4002 and 4003 equivalent from
attached-player measurements (outcome `distinct`, valid paired indices too few,
`d_goodput_pct` and `d_loss_pp` unidentifiable). Equivalence rests on the policy
tables and startup lines being literally identical, checked on every attempt,
not on a goodput comparison.

## Port 4003 deprecation timeline

| Release | 4003 state |
|---|---|
| This cutover (receiver PR) | Served, same policy as 4002, one WARN per process: `listen_publisher_srtla_classic is a deprecated alias of listen_publisher_srtla (same policy); it will be removed in a future release` |
| Platform routing flip (todo 28) | Still served; new sessions land on 4002; 4003 exists only for devices that have not picked up the new routing |
| One release after the platform flip | `listen_publisher_srtla_classic` removed; a config that still names it fails to parse |

Removing 4003 before the platform flip would strand devices on old routing.
Removing it in the same release as the flip removes the revert path. One release
after is the earliest safe point.
