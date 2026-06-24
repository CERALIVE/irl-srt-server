#include "conn_rate_limiter.hpp"

namespace {
// Hard ceiling on tracked source IPs. Each entry is a small string plus a
// token-bucket POD, so this bounds the limiter's memory. Sized well above any
// plausible legitimate concurrent source count; only a distributed /
// spoofed-source flood ever reaches it. Mirrors MAX_RATE_LIMIT_ENTRIES used by
// the player-key rate limiter.
constexpr size_t MAX_CONN_RATE_LIMIT_ENTRIES = 100000;
// Minimum spacing between full sweeps of the bucket map.
constexpr auto CONN_RATE_LIMIT_SWEEP_INTERVAL = std::chrono::seconds(1);
} // namespace

ConnRateLimiter::ConnRateLimiter(int requests_per_window, int window_ms, int burst)
{
    m_requests_per_window = requests_per_window;
    m_window_ms = window_ms > 0 ? window_ms : 1000;
    m_capacity = burst > 0
                     ? static_cast<double>(burst)
                     : static_cast<double>(requests_per_window > 0 ? requests_per_window * 2 : 0);
}

void ConnRateLimiter::refill_locked(Bucket &b, std::chrono::steady_clock::time_point now) const
{
    // Monotonic clock: now should never precede last_refill, but guard anyway
    // so a spurious non-advance never adds negative tokens.
    if (now <= b.last_refill) {
        return;
    }
    double elapsed_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - b.last_refill).count();
    double rate = static_cast<double>(m_requests_per_window) / static_cast<double>(m_window_ms); // tokens/ms
    b.tokens += elapsed_ms * rate;
    if (b.tokens > m_capacity) {
        b.tokens = m_capacity;
    }
    b.last_refill = now;
}

void ConnRateLimiter::cleanup_locked(std::chrono::steady_clock::time_point now)
{
    // A bucket that has refilled back to full capacity has been idle for at
    // least one full burst window and carries no state worth keeping, so drop
    // it. This keeps the map bounded by the set of *actively connecting*
    // sources rather than every source ever seen.
    for (auto it = m_buckets.begin(); it != m_buckets.end();) {
        refill_locked(it->second, now);
        if (it->second.tokens >= m_capacity) {
            it = m_buckets.erase(it);
        } else {
            ++it;
        }
    }
}

bool ConnRateLimiter::should_reject(const std::string &ip)
{
    if (m_requests_per_window <= 0) {
        return false; // disabled
    }
    if (ip.empty()) {
        return false; // no source to attribute the attempt to; never reject
    }

    std::lock_guard<std::mutex> lk(m_mtx);
    auto now = std::chrono::steady_clock::now();

    // Time-gated map-wide sweep so the O(n) scan runs at most once per second
    // regardless of connection rate.
    if (now - m_last_sweep >= CONN_RATE_LIMIT_SWEEP_INTERVAL) {
        cleanup_locked(now);
        m_last_sweep = now;
    }

    auto it = m_buckets.find(ip);
    if (it == m_buckets.end()) {
        // New source. Cap the tracked-IP set so a distributed / spoofed-source
        // flood cannot grow the map without bound: force a sweep, and if the
        // map is still full of actively-charging sources, fail open (allow,
        // untracked) rather than admit a new entry. This bounds memory without
        // locking out the sources already being tracked.
        if (m_buckets.size() >= MAX_CONN_RATE_LIMIT_ENTRIES) {
            cleanup_locked(now);
            m_last_sweep = now;
            if (m_buckets.size() >= MAX_CONN_RATE_LIMIT_ENTRIES) {
                return false;
            }
        }
        Bucket b;
        b.tokens = m_capacity; // a fresh source starts with a full burst
        b.last_refill = now;
        it = m_buckets.emplace(ip, b).first;
    } else {
        refill_locked(it->second, now);
    }

    Bucket &bucket = it->second;
    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        return false; // allowed
    }
    return true; // over budget — reject before srt_accept
}

void ConnRateLimiter::cleanup()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto now = std::chrono::steady_clock::now();
    cleanup_locked(now);
    m_last_sweep = now;
}

size_t ConnRateLimiter::tracked_sources() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_buckets.size();
}
