<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## ANTI-PATTERNS (things the legacy fork did that this base does NOT)

- **No receive profiles or modes.** No profile table, no per-listener mode selection, no
  L1/L2/L3 tiers. SRTLA behaviour is upstream's: one boolean, set on `listen_publisher_srtla`.
- **No `lossmaxttl` tuning.** Upstream's listener default (`200` packets, not ms) stands.
- **No audio-gap concealment.** Upstream removed it (`47e3ac4`); the server relays the TS
  opaquely and gap handling belongs to the playback side.
- **No per-source-IP connection limiter.** SLS sits behind a protected proxy, so every client
  shares the proxy's source address; a limiter would throttle legitimate traffic collectively.
- **No compat shim in `src/`.** `#ifdef SRTO_SRTLAPATCHES` fallbacks belong in the srt fork,
  never here; the negative CI leg exists to catch exactly that.
- **No CalVer, no `latest` tag, no moved tags.** Release tags equal PROJECT_VERSION and are
  immutable; rollback is a platform-side digest selection.
- **No auto-sync with upstream, no upstream remote left attached.**
- **No verbatim copy of legacy docs.** The legacy `AGENTS.md`, the audio-gap feature doc,
  `docs/evidence/`, and the legacy `docs/upstream-*.md` sync notes are not carried; they
  describe a fork that no longer exists.

