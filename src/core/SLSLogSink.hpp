#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "spdlog/details/log_msg.h"
#include "spdlog/sinks/dist_sink.h"

#include "SLSLogRedact.hpp"

// The one sink every SLS logger writes through. It owns the real sinks
// (stdout, the optional log file) and:
//   - scrubs each rendered payload with sls_redact_log() before any child sink
//     sees it, so redaction does not depend on every call site remembering to
//     scrub what it prints;
//   - records when a child write starts and finishes, so a sink that blocks
//     (a full pipe, a stalled FIFO reader, a hung disk) is visible as a stall.
//
// It runs on the asynchronous logging threads, never on a media worker: the
// loggers in front of it are spdlog async loggers that drop the oldest queued
// line instead of blocking when the queue is full (see SLSLog.cpp).
class CSLSRedactingDistSink final : public spdlog::sinks::dist_sink<std::mutex>
{
public:
    // Steady-clock milliseconds at which the write now in progress started,
    // or 0 when no write is in progress.
    int64_t busy_since_ms() const
    {
        return m_busy_since_ms.load(std::memory_order_acquire);
    }

    static int64_t steady_now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override
    {
        const std::string redacted = sls_redact_log(std::string_view(msg.payload.data(), msg.payload.size()));
        spdlog::details::log_msg scrubbed(msg);
        scrubbed.payload = spdlog::string_view_t(redacted.data(), redacted.size());
        mark_busy();
        spdlog::sinks::dist_sink<std::mutex>::sink_it_(scrubbed);
        m_busy_since_ms.store(0, std::memory_order_release);
    }

    void flush_() override
    {
        mark_busy();
        spdlog::sinks::dist_sink<std::mutex>::flush_();
        m_busy_since_ms.store(0, std::memory_order_release);
    }

private:
    void mark_busy()
    {
        // +1 keeps a write that starts at steady time 0 distinguishable from idle.
        m_busy_since_ms.store(steady_now_ms() + 1, std::memory_order_release);
    }

    std::atomic<int64_t> m_busy_since_ms{0};
};
