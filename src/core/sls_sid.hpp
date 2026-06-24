#pragma once

#include <map>
#include <string>

#include <srt/srt.h>

// Streamid parsing and handshake-time validation, factored out so the SRT
// listen callback (which runs before srt_accept, with no CSLSSrt instance
// yet) and the post-accept handler share one definition of a well-formed,
// safe publisher streamid.

class AuthRejectCache;
class ConnRateLimiter;

// State the SRT handshake callbacks consult before srt_accept, passed as the
// libsrt listen-callback `opaque`. Both pointers may be null (the gate they
// drive is then simply skipped). The pointed-to objects are owned by
// CSLSManager (shared_ptr) and outlive every listener socket whose callback
// references them, so the callback never dereferences freed state; the bundle
// itself is likewise held by CSLSManager for its whole lifetime.
struct SLSListenCallbackCtx {
    // Publisher negative-auth cache (null on player listeners): rejects
    // streamids that recently failed webhook authorization.
    AuthRejectCache *auth_reject_cache = nullptr;
    // Per-source-IP connection rate limiter (null = feature absent): rejects a
    // source flooding the listener with handshakes before any accept.
    ConnRateLimiter *conn_rate_limiter = nullptr;
};

// Parse an SRT publisher streamid into its key/value components. Handles
// both the "#!::h=..,sls_app=..,r=.." form and the bare "host/app/stream"
// form. Each value is trimmed of surrounding whitespace and newlines so a
// stray paste artifact does not change the resulting map key.
std::map<std::string, std::string> sls_parse_streamid(const char *sid);

// True if the streamid is non-empty, parses, carries h / sls_app / r, and
// all three pass sls_is_safe_name. Mirrors the post-accept checks in
// SLSListenerHandler so the callback and the handler agree on "valid".
bool sls_validate_sid_format(const char *sid);

// Canonical cache key for the auth-reject cache. Byte-level variants of the
// same logical streamid (trailing whitespace, reordered k/v pairs, different
// delimiter form) hash to different map slots and let a misbehaving client
// bypass the negative cache. Reduce a streamid to "h/sls_app/r" when all
// three are present; fall back to the raw streamid when it does not parse so
// behavior is unchanged for unparseable input.
std::string sls_canonical_sid_key(const std::string &streamid);

// srt_listen_callback hook for the publisher listener. Runs on the listener
// thread during the handshake, before srt_accept and before any webhook
// lookup. Rejects, in order: malformed streamids (SRT_REJ_ROGUE), sources
// over the per-IP connection rate limit (SRT_REJ_RESOURCE), and streamids in
// the negative auth cache (SRT_REJ_RESOURCE). The opaque is a
// SLSListenCallbackCtx* injected at registration and may be null (format gate
// still applies). Returns 0 to accept, -1 to reject. Must stay non-blocking:
// a parse plus one short locked lookup each, no HTTP or long-held locks.
int sls_publisher_listen_callback(void *opaque, SRTSOCKET ns, int hsversion,
                                  const struct sockaddr *peeraddr,
                                  const char *streamid);

// srt_listen_callback hook for the player listener. Rejects malformed
// streamids (SRT_REJ_ROGUE) and sources over the per-IP connection rate limit
// (SRT_REJ_RESOURCE) at the handshake, before srt_accept and any
// per-connection allocation, so a connect/RST flood is bounced cheaply
// instead of costing an accept + CSLSSrt + player object. The post-accept
// handler still resolves the publisher and (for player-key apps) authorizes
// the key. The opaque is a SLSListenCallbackCtx* (its auth_reject_cache is
// null for players). Returns 0 to accept, -1 to reject. Must stay
// non-blocking.
int sls_player_listen_callback(void *opaque, SRTSOCKET ns, int hsversion,
                               const struct sockaddr *peeraddr,
                               const char *streamid);
