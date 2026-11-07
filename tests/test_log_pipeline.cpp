#include "doctest.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include "spdlog/async_logger.h"
#include "spdlog/details/thread_pool.h"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/sinks/ostream_sink.h"
#include "spdlog/spdlog.h"

#include "SLSLog.hpp"
#include "SLSLogSink.hpp"
#include "SLSSrt.hpp"

namespace
{

// A sink whose writes block until released, standing in for a stdout pipe or
// FIFO nobody reads. Its bounded waits use system_clock deadlines on purpose:
// libstdc++ implements a steady_clock wait with pthread_cond_clockwait, which
// the GCC 11 ThreadSanitizer runtime does not intercept, so the TSan lane would
// report the waiting thread as still holding the mutex. A system_clock wait is
// pthread_cond_timedwait, which TSan sees.
class BlockingSink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    // Waits (bounded) until a write is blocked inside the sink.
    bool wait_until_blocked(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_state_mutex);
        return m_cv.wait_until(lock, std::chrono::system_clock::now() + timeout, [this] { return m_blocked; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_released = true;
        m_cv.notify_all();
    }

protected:
    void sink_it_(const spdlog::details::log_msg &) override
    {
        std::unique_lock<std::mutex> lock(m_state_mutex);
        m_blocked = true;
        m_cv.notify_all();
        // Bounded so a broken test cannot hang the suite forever.
        m_cv.wait_until(lock, std::chrono::system_clock::now() + std::chrono::seconds(30),
                        [this] { return m_released; });
    }
    void flush_() override {}

private:
    std::mutex m_state_mutex;
    std::condition_variable m_cv;
    bool m_blocked = false;
    bool m_released = false;
};

} // namespace

TEST_CASE("log pipeline: the root sink scrubs every line before any child sink sees it")
{
    std::ostringstream captured;
    auto root = std::make_shared<CSLSRedactingDistSink>();
    root->add_sink(std::make_shared<spdlog::sinks::ostream_sink_mt>(captured));
    spdlog::logger logger("redact-test", root);
    logger.set_level(spdlog::level::trace);

    // Call sites that never scrubbed: the sink must still catch them.
    logger.trace("Received stream_id 'play/r/cl_out_TRACEsecret1' from 127.0.0.1:4000");
    logger.debug("GET http://127.0.0.1:8090/sls/player-key/NODEsecret2?player_key=cl_out_KEYsecret3 completed");
    logger.info("new pub, key_stream_name=publish/r/relay9?k=cl_live_PUBsecret4.");
    logger.error("http refused, http_url='http://127.0.0.1:8090/admission/HOOKsecret5'");
    logger.flush();

    const std::string out = captured.str();
    for (const char *secret : {"TRACEsecret1", "NODEsecret2", "KEYsecret3", "PUBsecret4", "HOOKsecret5"})
    {
        CAPTURE(secret);
        CHECK(out.find(secret) == std::string::npos);
    }
    CHECK(out.find("relay9") != std::string::npos);
    CHECK(out.find("127.0.0.1:4000") != std::string::npos);
    CHECK(out.find("<redacted>") != std::string::npos);
}

TEST_CASE("log pipeline: a blocked sink drops the oldest lines and never blocks the caller")
{
    auto blocking = std::make_shared<BlockingSink>();
    auto root = std::make_shared<CSLSRedactingDistSink>();
    root->add_sink(blocking);
    auto pool = std::make_shared<spdlog::details::thread_pool>(16, 1);
    auto logger = std::make_shared<spdlog::async_logger>("stall-test", root, pool,
                                                         spdlog::async_overflow_policy::overrun_oldest);

    logger->info("first line, picked up by the logging thread and stuck in the sink");
    REQUIRE(blocking->wait_until_blocked(std::chrono::seconds(10)));
    CHECK(root->busy_since_ms() > 0);

    // The media worker's side of the contract: logging returns promptly
    // whatever the sink is doing. Run it on another thread with a deadline so
    // a regression fails the test instead of hanging it.
    auto producer = std::async(std::launch::async,
                               [&]
                               {
                                   for (int i = 0; i < 1000; i++)
                                       logger->warn("flood line {} with key=cl_live_FLOODsecret", i);
                               });
    REQUIRE(producer.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    producer.get();

    // 1000 lines into a 16-slot queue: at least 984 overwrote an older one.
    CHECK(pool->overrun_counter() >= 984);
    CHECK(root->busy_since_ms() > 0);

    blocking->release();
    logger->flush();
}

TEST_CASE("log pipeline: adding a sink never waits for a write blocked in another sink")
{
    // A SIGHUP reload adds the log file from the main thread, with its
    // listeners closed, while a logging thread may be stuck on a stdout
    // nobody reads. add_sink must not wait for that write.
    auto blocking = std::make_shared<BlockingSink>();
    auto root = std::make_shared<CSLSRedactingDistSink>();
    root->add_sink(blocking);
    auto pool = std::make_shared<spdlog::details::thread_pool>(16, 1);
    auto logger = std::make_shared<spdlog::async_logger>("reload-test", root, pool,
                                                         spdlog::async_overflow_policy::overrun_oldest);
    logger->info("first line, stuck in the blocked sink");
    REQUIRE(blocking->wait_until_blocked(std::chrono::seconds(10)));

    std::ostringstream captured;
    auto file_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured);
    auto adder = std::async(std::launch::async, [&] { root->add_sink(file_sink); });
    const bool added_while_blocked = adder.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    blocking->release();
    adder.get();
    REQUIRE(added_while_blocked);

    // Once the stall clears, the added sink gets the lines written after it.
    spdlog::logger direct("reload-direct", root);
    direct.info("after the reload, key=cl_live_RELOADsecret");
    const std::string out = captured.str();
    CHECK(out.find("after the reload") != std::string::npos);
    CHECK(out.find("RELOADsecret") == std::string::npos);
    logger->flush();
}

TEST_CASE("log pipeline: refusal lines share one budget and the excess is counted")
{
    const uint64_t suppressed_before = sls_get_log_health().suppressed_refusals;
    int allowed = 0;
    for (int i = 0; i < 500; i++)
    {
        if (sls_refusal_log_allowed())
            allowed++;
    }
    // Burst of 50 plus whatever refilled during the loop (20/s).
    CHECK(allowed >= 1);
    CHECK(allowed <= 60);
    CHECK(sls_get_log_health().suppressed_refusals - suppressed_before == static_cast<uint64_t>(500 - allowed));
}

TEST_CASE("log pipeline: security events are counted even when their line is suppressed")
{
    auto before = sls_get_log_health();
    // Drain the refusal budget so the lines below are suppressed.
    while (sls_refusal_log_allowed())
    {
    }
    for (int i = 0; i < 25; i++)
        SLS_REFUSAL_LOG(SLSSecurityEvent::AUTH_REFUSAL, warn, "suppressed refusal {}", i);
    SLS_SECURITY_LOG(SLSSecurityEvent::KICK, info, "kick");
    SLS_SECURITY_LOG(SLSSecurityEvent::ADMIN_DISCONNECT, info, "admin disconnect");
    sls_count_security_event(SLSSecurityEvent::ADMISSION_REFUSAL);

    auto after = sls_get_log_health();
    auto delta = [&](SLSSecurityEvent e)
    { return after.security_events[static_cast<int>(e)] - before.security_events[static_cast<int>(e)]; };
    CHECK(delta(SLSSecurityEvent::AUTH_REFUSAL) == 25);
    CHECK(delta(SLSSecurityEvent::KICK) == 1);
    CHECK(delta(SLSSecurityEvent::ADMIN_DISCONNECT) == 1);
    CHECK(delta(SLSSecurityEvent::ADMISSION_REFUSAL) == 1);
    CHECK(after.suppressed_refusals - before.suppressed_refusals >= 25);
}

TEST_CASE("log pipeline: libsrt's own lines go to the logger, never straight to stderr")
{
    std::ostringstream captured;
    auto previous = spdlog::default_logger();
    auto capture =
        std::make_shared<spdlog::logger>("libsrt-capture", std::make_shared<spdlog::sinks::ostream_sink_mt>(captured));
    capture->set_level(spdlog::level::trace);
    spdlog::set_default_logger(capture);
    REQUIRE(CSLSSrt::libsrt_init() == SLS_OK);
    // libsrt logs an error from inside srt_accept, the call that stopped
    // behind a blocked stdout while holding libsrt's global lock.
    const SRTSOCKET sock = srt_create_socket();
    CHECK(srt_accept(sock, nullptr, nullptr) == SRT_INVALID_SOCK);
    srt_close(sock);
    spdlog::set_default_logger(previous);

    const std::string out = captured.str();
    CHECK(out.find("[libsrt]") != std::string::npos);
    CHECK(out.find("not in listening state") != std::string::npos);
}

TEST_CASE("log pipeline: the exit drain gives up on a blocked sink at its deadline")
{
    auto blocking = std::make_shared<BlockingSink>();
    auto root = std::make_shared<CSLSRedactingDistSink>();
    root->add_sink(blocking);
    auto pool = std::make_shared<spdlog::details::thread_pool>(16, 1);
    auto security_pool = std::make_shared<spdlog::details::thread_pool>(16, 1);
    auto logger =
        std::make_shared<spdlog::async_logger>("drain-test", root, pool, spdlog::async_overflow_policy::overrun_oldest);
    logger->info("first line, stuck in the sink");
    REQUIRE(blocking->wait_until_blocked(std::chrono::seconds(10)));
    for (int i = 0; i < 8; i++)
        logger->info("queued line {}", i);

    // Exit's side of the contract: the drain returns, unfinished, instead of
    // waiting on the sink.
    auto drain = std::async(std::launch::async, [&] { return sls_log_wait_idle(*pool, *security_pool, *root, 200); });
    REQUIRE(drain.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK_FALSE(drain.get());
    CHECK(pool->queue_size() == 8);

    // Once the sink takes writes again, the same drain completes.
    blocking->release();
    CHECK(sls_log_wait_idle(*pool, *security_pool, *root, 10000));
    CHECK(pool->queue_size() == 0);
}
