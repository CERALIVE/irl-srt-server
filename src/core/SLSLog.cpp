
/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2020 Edward.Wu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <strings.h>
#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/async_logger.h"
#include "spdlog/sinks/ansicolor_sink.h"
#include "spdlog/sinks/basic_file_sink.h"

#include "SLSLog.hpp"
#include "SLSJsonSink.hpp"
#include "SLSLock.hpp"
#include "SLSLogSink.hpp"

std::mutex LOGGER_MUTEX;

// Global logging infrastructure
static sls_log_config_t g_log_config;
static CSLSLogRateLimiter g_rate_limiter;
static CSLSSummaryLogger g_summary_logger;
static std::string g_log_file_path;

namespace
{
// Queue depth of the operational logger; SLS_LOG_QUEUE_SIZE overrides it.
constexpr size_t kDefaultLogQueueSize = 8192;
constexpr size_t kSecurityLogQueueSize = 1024;
constexpr int64_t kStallThresholdMs = 1000;
constexpr int64_t kSummaryIntervalMs = 10000;
constexpr double kRefusalBurst = 50.0;
constexpr double kRefusalPerSecond = 20.0;
const char SECURITY_LOGGER_NAME[] = "srt-live-security";

// Created once in initialize_logger() and kept for the life of the process.
// The async loggers hold their thread pools only weakly, so these handles are
// what keep the logging threads alive.
std::shared_ptr<CSLSRedactingDistSink> g_root_sink;
std::shared_ptr<spdlog::details::thread_pool> g_log_pool;
std::shared_ptr<spdlog::details::thread_pool> g_security_pool;
std::shared_ptr<spdlog::logger> g_security_logger;

std::atomic<uint64_t> g_security_events[static_cast<int>(SLSSecurityEvent::COUNT)];
std::atomic<uint64_t> g_suppressed_refusals{0};
std::atomic<uint64_t> g_stall_events{0};
std::atomic<bool> g_stalled{false};

std::mutex g_refusal_mutex;
double g_refusal_tokens = kRefusalBurst;
int64_t g_refusal_refill_ms = 0;

// Main-loop state for sls_log_watchdog_tick.
int64_t g_stall_started_ms = 0;
uint64_t g_dropped_at_stall = 0;
int64_t g_last_summary_ms = 0;
uint64_t g_summarised_dropped = 0;
uint64_t g_summarised_suppressed = 0;

size_t log_queue_size()
{
    const char *env = std::getenv("SLS_LOG_QUEUE_SIZE");
    if (env == nullptr || *env == '\0')
        return kDefaultLogQueueSize;
    char *end = nullptr;
    unsigned long long value = std::strtoull(env, &end, 10);
    if (end == env || *end != '\0' || value < 16 || value > 1048576)
        return kDefaultLogQueueSize;
    return static_cast<size_t>(value);
}

uint64_t pool_overruns(const std::shared_ptr<spdlog::details::thread_pool> &pool)
{
    return pool ? static_cast<uint64_t>(pool->overrun_counter()) : 0;
}
} // namespace

int initialize_logger()
{
    g_root_sink = std::make_shared<CSLSRedactingDistSink>();
    g_root_sink->add_sink(std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>());

    g_log_pool = std::make_shared<spdlog::details::thread_pool>(log_queue_size(), 1);
    g_security_pool = std::make_shared<spdlog::details::thread_pool>(kSecurityLogQueueSize, 1);

    auto combined_logger = std::make_shared<spdlog::async_logger>(APP_NAME, g_root_sink, g_log_pool,
                                                                  spdlog::async_overflow_policy::overrun_oldest);
    combined_logger->set_level(DEFAULT_LOG_LEVEL);
    spdlog::set_default_logger(combined_logger);

    g_security_logger = std::make_shared<spdlog::async_logger>(SECURITY_LOGGER_NAME, g_root_sink, g_security_pool,
                                                               spdlog::async_overflow_policy::overrun_oldest);
    g_security_logger->set_level(DEFAULT_LOG_LEVEL);

    // Initialize log configuration with defaults
    g_log_config.rate_limit_enabled = true;
    g_log_config.rate_limit_window_sec = 60;
    g_log_config.rate_limit_threshold = 5;
    g_log_config.summary_enabled = true;
    g_log_config.summary_interval_sec = 60;
    g_log_config.session_ids_enabled = true;
    g_log_config.json_format = false;

    // Initialize all category levels as "not set"
    for (int i = 0; i < static_cast<int>(SLSLogCategory::COUNT); i++)
    {
        g_log_config.category_level_set[i] = false;
        g_log_config.category_levels[i] = spdlog::level::info;
    }

    return 0;
}

int sls_set_log_level(char *log_level)
{
    log_level = sls_strlower(log_level); // to upper
    std::string log_level_str(log_level);
    spdlog::level::level_enum new_level = spdlog::level::from_str(log_level_str);
    spdlog::get(APP_NAME)->set_level(new_level);
    if (g_security_logger)
        g_security_logger->set_level(new_level);
    spdlog::warn("Setting logging level to {}", spdlog::level::to_string_view(new_level));
    return SLS_OK;
}

int sls_set_log_file(char *log_file)
{
    if (log_file && strlen(log_file) > 0)
    {
        g_log_file_path = log_file;

        spdlog::sink_ptr file_sink;

        // Use JSON sink if JSON format is enabled
        if (g_log_config.json_format)
        {
            file_sink = std::make_shared<json_file_sink_mt>(log_file);
        }
        else
        {
            file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file);
        }

        // The file sits behind the redacting root sink like stdout does.
        // add_sink never waits for a write in progress, so a reload is not
        // held up by a logging thread blocked on stdout (SLSLogSink.hpp).
        if (g_root_sink)
        {
            g_root_sink->add_sink(file_sink);
        }
        else
        {
            LOGGER_MUTEX.lock();
            spdlog::get(APP_NAME)->sinks().push_back(file_sink);
            LOGGER_MUTEX.unlock();
        }
        return SLS_OK;
    }
    return SLS_ERROR;
}

int sls_set_category_log_level(SLSLogCategory category, const char *log_level)
{
    std::string log_level_str(log_level);
    spdlog::level::level_enum new_level = spdlog::level::from_str(log_level_str);

    int cat_idx = static_cast<int>(category);
    g_log_config.category_levels[cat_idx] = new_level;
    g_log_config.category_level_set[cat_idx] = true;

    return SLS_OK;
}

sls_log_config_t &sls_get_log_config()
{
    return g_log_config;
}

CSLSLogRateLimiter &sls_get_rate_limiter()
{
    return g_rate_limiter;
}

CSLSSummaryLogger &sls_get_summary_logger()
{
    return g_summary_logger;
}

bool sls_should_log_category(SLSLogCategory category, spdlog::level::level_enum level)
{
    int cat_idx = static_cast<int>(category);

    // Check category-specific level if set
    if (g_log_config.category_level_set[cat_idx])
    {
        return level >= g_log_config.category_levels[cat_idx];
    }

    // Fall back to global level
    return level >= spdlog::get(APP_NAME)->level();
}

void sls_count_security_event(SLSSecurityEvent event)
{
    const int idx = static_cast<int>(event);
    if (idx >= 0 && idx < static_cast<int>(SLSSecurityEvent::COUNT))
        g_security_events[idx].fetch_add(1, std::memory_order_relaxed);
}

bool sls_refusal_log_allowed()
{
    const int64_t now = CSLSRedactingDistSink::steady_now_ms();
    std::lock_guard<std::mutex> lock(g_refusal_mutex);
    if (g_refusal_refill_ms == 0)
        g_refusal_refill_ms = now;
    const double elapsed_s = static_cast<double>(now - g_refusal_refill_ms) / 1000.0;
    g_refusal_refill_ms = now;
    g_refusal_tokens = std::min(kRefusalBurst, g_refusal_tokens + elapsed_s * kRefusalPerSecond);
    if (g_refusal_tokens >= 1.0)
    {
        g_refusal_tokens -= 1.0;
        return true;
    }
    g_suppressed_refusals.fetch_add(1, std::memory_order_relaxed);
    return false;
}

std::shared_ptr<spdlog::logger> sls_security_logger()
{
    if (g_security_logger)
        return g_security_logger;
    return spdlog::default_logger();
}

sls_log_health_t sls_get_log_health()
{
    sls_log_health_t health;
    health.dropped = pool_overruns(g_log_pool);
    health.security_dropped = pool_overruns(g_security_pool);
    health.suppressed_refusals = g_suppressed_refusals.load(std::memory_order_relaxed);
    health.stall_events = g_stall_events.load(std::memory_order_relaxed);
    health.stalled = sls_log_is_stalled();
    for (int i = 0; i < static_cast<int>(SLSSecurityEvent::COUNT); i++)
        health.security_events[i] = g_security_events[i].load(std::memory_order_relaxed);
    return health;
}

bool sls_log_is_stalled()
{
    if (!g_root_sink)
        return false;
    const int64_t busy_since = g_root_sink->busy_since_ms();
    return busy_since > 0 && CSLSRedactingDistSink::steady_now_ms() - busy_since >= kStallThresholdMs;
}

void sls_log_watchdog_tick()
{
    if (!g_root_sink)
        return;
    const int64_t now = CSLSRedactingDistSink::steady_now_ms();
    const bool stalled_now = sls_log_is_stalled();
    const uint64_t dropped = pool_overruns(g_log_pool);

    if (stalled_now && !g_stalled.load(std::memory_order_relaxed))
    {
        g_stalled.store(true, std::memory_order_relaxed);
        g_stall_events.fetch_add(1, std::memory_order_relaxed);
        g_stall_started_ms = g_root_sink->busy_since_ms();
        g_dropped_at_stall = dropped;
    }
    else if (!stalled_now && g_stalled.load(std::memory_order_relaxed))
    {
        g_stalled.store(false, std::memory_order_relaxed);
        g_security_logger->warn("[system] log sink was blocked for {} ms; {} log line(s) dropped meanwhile",
                                now - g_stall_started_ms, dropped - g_dropped_at_stall);
    }

    if (g_last_summary_ms == 0)
        g_last_summary_ms = now;
    if (now - g_last_summary_ms < kSummaryIntervalMs)
        return;
    const uint64_t suppressed = g_suppressed_refusals.load(std::memory_order_relaxed);
    if (dropped != g_summarised_dropped)
    {
        g_security_logger->warn("[system] log queue overrun: {} line(s) dropped in the last {} s ({} in total)",
                                dropped - g_summarised_dropped, (now - g_last_summary_ms) / 1000, dropped);
    }
    if (suppressed != g_summarised_suppressed)
    {
        g_security_logger->info("[system] suppressed {} unauthenticated refusal log line(s) in the last {} s",
                                suppressed - g_summarised_suppressed, (now - g_last_summary_ms) / 1000);
    }
    g_summarised_dropped = dropped;
    g_summarised_suppressed = suppressed;
    g_last_summary_ms = now;
}
