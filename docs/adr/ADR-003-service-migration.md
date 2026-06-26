# ADR-003: Reject service migration — harden in place

**Status:** Accepted
**Date:** 2026-06-26

---

## Context

`irl-srt-server` is a focused SRT relay: receive a bonded MPEG-TS stream, re-serve it downstream. The codebase is C++17 with three main areas that could theoretically be rewritten or replaced:

- **cpp-httplib** — the HTTP control/stats API and webhook callbacks
- **Ring buffer** (`SLSRecycleArray`) — the manual circular buffer between publisher and player roles
- **Config parser** — the `sls.conf` domain/app/stream routing parser
- **TS parser** — the length-driven MPEG-TS packet boundary logic

The question arose during the srt-quality-hardening plan: should any of these be migrated to Rust, replaced with a different language runtime, or pulled in as a new heavy dependency?

---

## Decision

**No migration. Harden in place.**

The existing C++ components will be hardened through:

- Fuzzing (libFuzzer targets for the config parser and TS parser)
- Sanitizer CI legs (ASan/UBSan and TSan, already wired in Todo 1)
- RAII ownership fixes (ring buffer, role lifecycle)
- Targeted correctness fixes for the specific bugs found

---

## Consequences

### Why not migrate

**FFI complexity.** A Rust component called from C++ (or vice versa) requires a C ABI boundary, careful lifetime contracts across that boundary, and a second build toolchain in the Docker image and CI. For a relay this size, the FFI seam introduces more risk than the code it replaces.

**Regression surface.** Rewriting a component that already has known-good behavior under load (cpp-httplib at v0.48.0 is a mature, widely deployed library) trades a bounded set of C++ bugs for an unbounded set of rewrite bugs. The hardening plan addresses the actual bugs found; a rewrite addresses hypothetical ones.

**cpp-httplib maturity.** The library handles the HTTP control API and webhook callbacks. It's pinned at a release tag, has no known exploitable issues in this usage pattern, and the attack surface is internal (not internet-facing in production). Replacing it gains nothing concrete.

**Relay scope.** This service does no encoding, no transcoding, no media processing. The parser and ring buffer are small, well-bounded, and now covered by sanitizer CI. The cost/benefit of a language migration doesn't close for a component of this size and scope.

### What hardening buys instead

- Fuzzing catches input-driven parser bugs deterministically, without a rewrite
- ASan/UBSan catches memory errors at the C++ level with zero FFI overhead
- TSan catches data races in the ring buffer and role state
- RAII fixes eliminate the specific UAF and leak classes already found

---

## Revisit triggers

Reopen this decision if:

1. Fuzzing surfaces a **class** of parser bug (not a single instance) that resists a C++ fix — e.g. a structural issue in the config grammar that requires a full reparse, or a TS framing assumption baked into the parser that can't be patched incrementally.
2. cpp-httplib reaches end-of-life or a CVE is found that affects this usage pattern and has no upstream fix.
3. The relay scope expands significantly — e.g. media processing, transcoding, or a plugin system — such that the FFI cost is amortized across a much larger new component.

None of these conditions currently hold.

---

*ADR-002 ("SRT patch necessity") exists as prose in `AGENTS.md` and `README.md` — it is not a file in `docs/adr/`.*
