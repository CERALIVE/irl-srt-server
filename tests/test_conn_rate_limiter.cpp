#include "doctest.h"

#include <chrono>
#include <string>
#include <thread>

#include "conn_rate_limiter.hpp"

// Per-source-IP token-bucket connection rate limiter evaluated at the SRT
// handshake. These tests pin the contract the listen callback relies on: a
// single source is throttled after its burst, distinct sources are
// independent, the limiter can be disabled, an empty source is never charged,
// and the tracked-IP map stays bounded.

TEST_CASE("ConnRateLimiter: a fresh source is allowed up to its burst capacity")
{
    // 10 req/window, burst defaults to 2x = 20. The first 20 attempts from a
    // single IP exhaust the bucket; the 21st is over budget. Rapid calls span
    // microseconds, so refill within the loop is negligible (< 1 token).
    ConnRateLimiter rl(10, 1000, 0);
    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(rl.should_reject("203.0.113.7"));
    }
    CHECK(rl.should_reject("203.0.113.7"));
}

TEST_CASE("ConnRateLimiter: an explicit burst sets the bucket capacity")
{
    ConnRateLimiter rl(10, 1000, 3);
    CHECK_FALSE(rl.should_reject("198.51.100.1"));
    CHECK_FALSE(rl.should_reject("198.51.100.1"));
    CHECK_FALSE(rl.should_reject("198.51.100.1"));
    CHECK(rl.should_reject("198.51.100.1"));
}

TEST_CASE("ConnRateLimiter: distinct source IPs have independent budgets")
{
    ConnRateLimiter rl(10, 1000, 2);
    CHECK_FALSE(rl.should_reject("10.0.0.1"));
    CHECK_FALSE(rl.should_reject("10.0.0.1"));
    CHECK(rl.should_reject("10.0.0.1")); // first IP exhausted

    // A different source still has its full burst.
    CHECK_FALSE(rl.should_reject("10.0.0.2"));
    CHECK_FALSE(rl.should_reject("10.0.0.2"));
    CHECK(rl.should_reject("10.0.0.2"));
}

TEST_CASE("ConnRateLimiter: a non-positive request budget disables the limiter")
{
    ConnRateLimiter rl(-1, 1000, 0);
    CHECK_FALSE(rl.enabled());
    for (int i = 0; i < 1000; ++i) {
        CHECK_FALSE(rl.should_reject("192.0.2.50"));
    }

    ConnRateLimiter rl_zero(0, 1000, 0);
    CHECK_FALSE(rl_zero.enabled());
    CHECK_FALSE(rl_zero.should_reject("192.0.2.51"));
}

TEST_CASE("ConnRateLimiter: an empty source IP is never charged or rejected")
{
    ConnRateLimiter rl(1, 1000, 1);
    // Exhaust a real IP to prove the limiter is otherwise live.
    CHECK_FALSE(rl.should_reject("192.0.2.9"));
    CHECK(rl.should_reject("192.0.2.9"));
    // The empty source is always allowed and never tracked.
    for (int i = 0; i < 100; ++i) {
        CHECK_FALSE(rl.should_reject(""));
    }
}

TEST_CASE("ConnRateLimiter: tokens refill over time so a throttled source recovers")
{
    // 10 req / 50ms => 0.2 tokens/ms; burst 3. Exhaust, then wait one full
    // window (refills well past capacity, clamped to 3) and check recovery.
    ConnRateLimiter rl(10, 50, 3);
    CHECK_FALSE(rl.should_reject("172.16.0.5"));
    CHECK_FALSE(rl.should_reject("172.16.0.5"));
    CHECK_FALSE(rl.should_reject("172.16.0.5"));
    CHECK(rl.should_reject("172.16.0.5")); // exhausted

    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    CHECK_FALSE(rl.should_reject("172.16.0.5")); // refilled, allowed again
}

TEST_CASE("ConnRateLimiter: cleanup drops idle (fully refilled) buckets")
{
    ConnRateLimiter rl(10, 50, 3);
    CHECK_FALSE(rl.should_reject("172.16.0.6"));
    CHECK(rl.tracked_sources() == 1);

    // After a full window the bucket refills to capacity; cleanup() then
    // reclaims it, keeping the map bounded by actively-connecting sources.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    rl.cleanup();
    CHECK(rl.tracked_sources() == 0);
}

TEST_CASE("ConnRateLimiter: many distinct sources are tracked independently and bounded")
{
    ConnRateLimiter rl(10, 1000, 5);
    for (int i = 0; i < 2000; ++i) {
        std::string ip = "100.64." + std::to_string(i / 256) + "." + std::to_string(i % 256);
        CHECK_FALSE(rl.should_reject(ip)); // first attempt per IP always fits the burst
    }
    CHECK(rl.tracked_sources() == 2000);
}
