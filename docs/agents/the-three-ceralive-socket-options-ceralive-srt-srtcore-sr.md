<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## THE THREE CERALIVE SOCKET OPTIONS (`CERALIVE/srt` `srtcore/srt.h`)

| Number | Enumerator | Type | Meaning |
|---|---|---|---|
| `118` | `SRTO_SRTLAPATCHES` | bool | Compat enumerator with upstream `irlserver/srt`'s name and semantics. Setter: `bReorderFreeze = on; iPeriodicNakGate = on ? SRTLA_PATCHES_DEFAULT_NAKGATE : 0`. Getter: `bReorderFreeze && iPeriodicNakGate != 0`. Only 0 / non-zero are meaningful. URI key `srtlapatches`. |
| `119` | `SRTO_PERIODICNAKGATE` | int, tri-state | `0` off (Haivision behaviour: periodic NAK always sent). `1` filter: subtract still-reorderable fresh-loss ranges, send the periodic NAK only for genuine loss. `2` suppress: never send the periodic NAK (bit-exact upstream `SRTLAPATCHES` site 4). Any other value is `SRT_EINVPARAM`. URI key `periodicnakgate`. |
| `120` | `SRTO_REORDERFREEZE` | bool | Freeze `m_iReorderTolerance` at `iMaxReorderTolerance`: no decay on ordered/early runs. Pre-existing CERALIVE option. URI key `reorderfreeze`. |

Upstream `irlserver/srt` numbers its `SRTO_SRTLAPATCHES` **120**, the same number CERALIVE
already used for `REORDERFREEZE`. Numbers are ABI; never renumber any of the three.

**Setting `PERIODICNAKGATE` after `SRTLAPATCHES` overrides it** (last write wins). SLS only
sets `SRTLAPATCHES`, so the server's effective behaviour is whatever the compat default
resolves to.

### Equivalence: upstream `SRTLAPATCHES=1` vs CERALIVE `REORDERFREEZE=1 + PERIODICNAKGATE`

Verified by reading both trees (upstream `f2297192:srtcore/core.cpp` against CERALIVE's).

| # | Upstream gate site | CERALIVE | Verdict |
|---|---|---|---|
| 1 | `initial_loss_ttl = srtlaPatches ? iMaxReorderTolerance : m_iReorderTolerance` | `initial_loss_ttl = m_iReorderTolerance` | **Exactly equivalent.** Every write to `m_iReorderTolerance` was enumerated: init sets it to the max; the only decrements are the two freeze-gated sites; the only other write is an increase capped at the max. While `bReorderFreeze`, `m_iReorderTolerance == iMaxReorderTolerance`. |
| 2 | 50-consecutive-ordered decay gated `!srtlaPatches` | gated `!bReorderFreeze` | Equivalent. |
| 3 | 10-consecutive-early decay gated `!srtlaPatches` | gated `!bReorderFreeze` | Equivalent. |
| 4 | periodic NAK: `if (!srtlaPatches) sendCtrl(UMSG_LOSSREPORT)` (suppressed entirely) | `PERIODICNAKGATE`: `2` = suppressed (bit-exact), `1` = filtered (genuine loss still reported) | **Divergent by design; resolved by measurement, not on paper.** |

### The periodic-NAK default is 2 (D10 A/B complete)

`SRTLA_PATCHES_DEFAULT_NAKGATE` (`srtcore/socketconfig.h` in `CERALIVE/srt`) is
**`2`** (upstream-exact suppress), confirmed by the D10 compat-harness A/B: 24 valid
runs, four netem cells, N=3 per arm. Filter (`1`) won loss on only one of four cells;
the goodput guard held on all four, so the pre-registered rule selected suppress (`2`).
The released `srt-v1.5.7+ceralive.2` pins this outcome. On the SRTLA listener the
effective options are **118 on, 119 = 2, 120 on**. This is simulation evidence, not
a claim of real bonded-hardware validation.

