#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "spdlog/details/log_msg.h"
#include "spdlog/formatter.h"
#include "spdlog/sinks/sink.h"

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
//
// Writes are serialized by their own lock, which a blocked child keeps held.
// The child list is a copy-on-write snapshot behind a separate lock that no
// write holds, so add_sink() never waits for a write in progress: a SIGHUP
// reload adds the log file from the main thread while its listeners are
// closed, and must not wait on a stdout nobody reads.
class CSLSRedactingDistSink final : public spdlog::sinks::sink
{
public:
    void add_sink(std::shared_ptr<spdlog::sinks::sink> sink)
    {
        std::lock_guard<std::mutex> lock(m_children_mutex);
        auto next = std::make_shared<Children>(*m_children);
        next->push_back(std::move(sink));
        m_children = std::move(next);
    }

    void log(const spdlog::details::log_msg &msg) override
    {
        const std::string redacted = sls_redact_log(std::string_view(msg.payload.data(), msg.payload.size()));
        spdlog::details::log_msg scrubbed(msg);
        scrubbed.payload = spdlog::string_view_t(redacted.data(), redacted.size());
        std::lock_guard<std::mutex> lock(m_write_mutex);
        const std::shared_ptr<const Children> children = snapshot();
        mark_busy();
        for (const auto &child : *children)
        {
            if (child->should_log(scrubbed.level))
                child->log(scrubbed);
        }
        m_busy_since_ms.store(0, std::memory_order_release);
    }

    void flush() override
    {
        std::lock_guard<std::mutex> lock(m_write_mutex);
        const std::shared_ptr<const Children> children = snapshot();
        mark_busy();
        for (const auto &child : *children)
            child->flush();
        m_busy_since_ms.store(0, std::memory_order_release);
    }

    void set_pattern(const std::string &pattern) override
    {
        for (const auto &child : *snapshot())
            child->set_pattern(pattern);
    }

    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override
    {
        for (const auto &child : *snapshot())
            child->set_formatter(formatter->clone());
    }

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

private:
    using Children = std::vector<std::shared_ptr<spdlog::sinks::sink>>;

    std::shared_ptr<const Children> snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_children_mutex);
        return m_children;
    }

    void mark_busy()
    {
        // +1 keeps a write that starts at steady time 0 distinguishable from idle.
        m_busy_since_ms.store(steady_now_ms() + 1, std::memory_order_release);
    }

    mutable std::mutex m_children_mutex;
    std::shared_ptr<const Children> m_children = std::make_shared<const Children>();
    std::mutex m_write_mutex;
    std::atomic<int64_t> m_busy_since_ms{0};
};
