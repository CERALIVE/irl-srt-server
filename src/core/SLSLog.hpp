
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

#pragma once

#include <cstdarg>
#include <cstdint>
#include <memory>
#include <stdio.h>
#include "spdlog/spdlog.h"

#include "common.hpp"
#include "SLSLock.hpp"
#include "SLSLogCategory.hpp"
#include "SLSLogRateLimiter.hpp"
#include "SLSSessionTracker.hpp"
#include "SLSSummaryLogger.hpp"

static const char APP_NAME[] = "srt-live";

/**
 * Logging configuration structure
 */
struct sls_log_config_t
{
    bool rate_limit_enabled;
    int rate_limit_window_sec;
    int rate_limit_threshold;
    bool summary_enabled;
    int summary_interval_sec;
    bool session_ids_enabled;
    bool json_format;

    // Per-category log levels (nullptr means use global level)
    spdlog::level::level_enum category_levels[static_cast<int>(SLSLogCategory::COUNT)];
    bool category_level_set[static_cast<int>(SLSLogCategory::COUNT)];
};

/**
 * Initialize logger with default settings
 */
int initialize_logger();

/**
 * Set global log level
 */
int sls_set_log_level(char *log_level);

/**
 * Set log file
 */
int sls_set_log_file(char *log_file);

/**
 * Set category-specific log level
 */
int sls_set_category_log_level(SLSLogCategory category, const char *log_level);

/**
 * Get current log configuration
 */
sls_log_config_t &sls_get_log_config();

/**
 * Get global rate limiter
 */
CSLSLogRateLimiter &sls_get_rate_limiter();

/**
 * Get global summary logger
 */
CSLSSummaryLogger &sls_get_summary_logger();

/**
 * Check if category should be logged at given level
 */
bool sls_should_log_category(SLSLogCategory category, spdlog::level::level_enum level);

/*
 * Asynchronous logging. Both loggers below queue each line for a background
 * thread and never block the caller: when a queue is full the oldest queued
 * line is dropped and counted. A stalled stdout, FIFO or log file therefore
 * costs log lines, never media-worker time. The operational logger and the
 * security logger have separate queues, so a flood of operational lines
 * cannot push security events out.
 */

// Security-relevant events, counted exactly and exported in /stats. The
// counters are plain atomics, so they survive any log-queue overrun.
enum class SLSSecurityEvent : int
{
    AUTH_REFUSAL = 0,      // an authorization webhook or key check refused a caller
    ADMISSION_REFUSAL = 1, // a handshake refused before authorization (bad SID, ACL, caps)
    KICK = 2,              // a worker tore a role down on a kick request
    ADMIN_DISCONNECT = 3,  // /disconnect or /disconnect-player accepted a request
    COUNT = 4
};

void sls_count_security_event(SLSSecurityEvent event);

// Global token bucket shared by every unauthenticated refusal line (burst 50,
// 20 lines/s). Junk handshakes cannot flood the queue with refusals; lines
// over budget are counted as suppressed and summarised every 10 s.
bool sls_refusal_log_allowed();

// The logger for security events. Never null.
std::shared_ptr<spdlog::logger> sls_security_logger();

struct sls_log_health_t
{
    uint64_t dropped = 0;          // operational lines dropped on queue overrun
    uint64_t security_dropped = 0; // security lines dropped on queue overrun
    uint64_t suppressed_refusals = 0;
    uint64_t stall_events = 0; // times a sink write was seen blocked >= 1 s
    bool stalled = false;      // a sink write is blocked right now
    uint64_t security_events[static_cast<int>(SLSSecurityEvent::COUNT)] = {};
};
sls_log_health_t sls_get_log_health();

// Stall detection and the periodic drop/suppression summary. Called from the
// main loop.
void sls_log_watchdog_tick();

// True when a sink write is blocked, so a final flush at exit would hang.
bool sls_log_is_stalled();

namespace spdlog::details
{
class thread_pool;
}
class CSLSRedactingDistSink;

// True once both pools' queues are empty and the sink has no write in
// progress; false when that has not happened within timeout_ms.
bool sls_log_wait_idle(spdlog::details::thread_pool &pool, spdlog::details::thread_pool &security_pool,
                       const CSLSRedactingDistSink &sink, int64_t timeout_ms);

// Called once at the end of main, so exit never waits on a log sink without a
// bound. Stops new lines, then gives the log threads 2 s to write the queued
// ones. Returns true when they did; a backstop then ends the process should a
// sink block again before the threads are joined. Returns false when a sink is
// still blocked: the queued lines are dropped, their count goes to stderr if it
// can take the line without blocking, and the caller must exit without joining
// the log threads. Security events stay counted either way: the counter is
// bumped when the event happens, not when its line is written.
bool sls_log_drain_for_exit();

// srt_setloghandler target: libsrt's own lines go to the operational logger,
// asynchronously, so a blocked sink never stops a libsrt thread.
void sls_libsrt_log_handler(void *opaque, int level, const char *file, int line, const char *area, const char *message);

// An unauthenticated refusal: counted, then logged on the security logger if
// the refusal budget allows.
#define SLS_REFUSAL_LOG(event, level, ...)                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        sls_count_security_event(event);                                                                               \
        if (sls_refusal_log_allowed())                                                                                 \
            sls_security_logger()->level(__VA_ARGS__);                                                                 \
    } while (0)

// A security event that is never rate-limited (kicks, admin disconnects).
#define SLS_SECURITY_LOG(event, level, ...)                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        sls_count_security_event(event);                                                                               \
        sls_security_logger()->level(__VA_ARGS__);                                                                     \
    } while (0)
