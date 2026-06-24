#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

// Per-source-IP connection rate limiter evaluated inside the SRT handshake
// callback (srt_listen_callback), BEFORE srt_accept. A single source hammering
// a listener with connect attempts is therefore bounced at the handshake — no
// srt_accept, no CSLSSrt, no role, and no ring buffer is ever created for the
// rejected attempt.
//
// The limiter is a token bucket per source IP: a sustained refill budget
// (requests per window) plus a short burst allowance (bucket capacity). This
// admits a legitimate reconnect storm — OBS / srtla flapping, or several real
// clients behind one NAT/CDN egress reconnecting at once — while throttling a
// sustained flood from a single address. Defaults are deliberately generous
// (10 req/s sustained, burst 20) so normal operation is never throttled;
// operators behind very large shared-NAT ingress can raise the budget or set
// the request count to a non-positive value to disable the limiter entirely.
//
// Memory is bounded: the bucket map is hard-capped (MAX entries) and idle
// (full) buckets are swept periodically, so a distributed / spoofed-source
// flood cannot grow the map without limit — it merely fills to the cap, after
// which new untracked sources fail open (allowed) rather than consuming
// unbounded memory. Per-source-IP limiting inherently cannot stop a
// spoofed-source flood (each forged address appears once); its job is to bound
// a single real source and keep memory bounded, which it does.
//
// One instance is owned by CSLSManager (shared_ptr) and injected into every
// listener's handshake-callback context, so the limiter outlives any listener
// socket whose callback still references it. The callback runs on libsrt's
// internal accept thread, so every mutating method is mutex-guarded.
class ConnRateLimiter
{
public:
    // requests_per_window: sustained refill budget (tokens added per window).
    //   <= 0 disables the limiter (every attempt is allowed).
    // window_ms: refill window in milliseconds (<= 0 coerced to 1000).
    // burst: bucket capacity / max tokens. <= 0 derives 2 * requests_per_window.
    explicit ConnRateLimiter(int requests_per_window = 10, int window_ms = 1000, int burst = 0);

    // True if the limiter is active (a positive request budget is configured).
    // A plain read of an immutable-after-construction field, so no lock: the
    // limiter is fully configured by the constructor before its shared_ptr is
    // published to any listener.
    bool enabled() const { return m_requests_per_window > 0; }

    // Charge one connection attempt from this source IP. Returns true if the
    // attempt is over budget and should be REJECTED, false if it is allowed
    // (a token is consumed on allow). A disabled limiter or an empty IP always
    // returns false (allowed). Thread-safe.
    bool should_reject(const std::string &ip);

    // Drop idle (full) buckets now. should_reject sweeps periodically on its
    // own; exposed for tests and explicit callers.
    void cleanup();

    // Number of currently tracked source IPs. Test / inspection helper.
    size_t tracked_sources() const;

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point last_refill;
    };

    void refill_locked(Bucket &b, std::chrono::steady_clock::time_point now) const;
    void cleanup_locked(std::chrono::steady_clock::time_point now);

    mutable std::mutex m_mtx;
    std::unordered_map<std::string, Bucket> m_buckets;
    int m_requests_per_window; // refill budget per window; <= 0 == disabled
    int m_window_ms;           // refill window length (ms)
    double m_capacity;         // bucket capacity == burst allowance (tokens)
    std::chrono::steady_clock::time_point m_last_sweep{};
};
