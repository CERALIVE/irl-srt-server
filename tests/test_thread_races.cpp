#include "doctest.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <time.h>
#include <vector>

#include "SLSEpollThread.hpp"
#include "SLSGroup.hpp"
#include "SLSRole.hpp"
#include "SLSSrt.hpp"
#include "common.hpp"

// Regression tests for the cross-thread fixes in ledger rows E10 and E11. Each
// reproduces one access pattern the server E2Es exposed. A data race is only
// visible to ThreadSanitizer, so these fail on the TSan ctest leg if a fix
// regresses; on the other legs they still exercise the same paths.
//
// The threads count their passes in relaxed atomics, which order nothing for
// ThreadSanitizer: waiting on them keeps the racing accesses on both sides of
// a write without making them happen-before it.

namespace
{

class IdleThread final : public CSLSEpollThread
{
protected:
    int handler() override
    {
        std::this_thread::yield();
        return 0;
    }
};

class BitrateRole final : public CSLSRole
{
public:
    void store_bitrate(int kbps)
    {
        m_kbitrate = kbps;
    }
};

// Yields until a relaxed pass counter reaches target.
void wait_for_passes(const std::atomic<long> &passes, long target)
{
    while (passes.load(std::memory_order_relaxed) < target)
        std::this_thread::yield();
}

} // namespace

TEST_CASE("E10: a starting worker thread does not read the id its starter is still storing")
{
    // CSLSEpollThread::work() used to log m_th_id, which CSLSThread::start()
    // assigns after pthread_create() has already started the thread.
    for (int i = 0; i < 20; i++)
    {
        IdleThread thread;
        REQUIRE(thread.start() == 0);
        CHECK(thread.stop() == 0);
    }
}

TEST_CASE("E11: a role's state is read on one thread while another invalidates it")
{
    // /stats and the workers read m_state while the main thread uninits
    // listeners on reload and shutdown.
    CSLSRole role;
    role.init();
    std::atomic<bool> done{false};
    std::atomic<long> passes{0};
    std::atomic<bool> seen{false}; // keeps each read from being optimized out
    std::thread reader(
        [&]
        {
            while (!done.load(std::memory_order_relaxed))
            {
                seen.store(role.is_invalid(), std::memory_order_relaxed);
                passes.fetch_add(1, std::memory_order_relaxed);
            }
        });
    wait_for_passes(passes, 100);
    role.mark_invalid();
    wait_for_passes(passes, passes.load(std::memory_order_relaxed) + 100);
    done.store(true, std::memory_order_relaxed);
    reader.join();
    CHECK(role.is_invalid());
}

TEST_CASE("E11: the bitrate is read on one thread while the owning worker updates it")
{
    BitrateRole role;
    std::atomic<bool> done{false};
    std::atomic<long> passes{0};
    std::atomic<int> seen{0}; // keeps each read from being optimized out
    std::thread reader(
        [&]
        {
            while (!done.load(std::memory_order_relaxed))
            {
                seen.store(role.get_bitrate(), std::memory_order_relaxed);
                passes.fetch_add(1, std::memory_order_relaxed);
            }
        });
    wait_for_passes(passes, 100);
    for (int kbps = 1; kbps <= 1000; kbps++)
        role.store_bitrate(kbps);
    wait_for_passes(passes, passes.load(std::memory_order_relaxed) + 100);
    done.store(true, std::memory_order_relaxed);
    reader.join();
    CHECK(role.get_bitrate() == 1000);
}

TEST_CASE("E11: get_statistics may run while the owning worker closes the socket")
{
    // /stats reads a publisher's statistics on the HTTP thread; its worker may
    // close and delete the socket at the same moment.
    REQUIRE(srt_startup() == 0);
    {
        CSLSRole role;
        role.init();
        auto *srt = new CSLSSrt;
        srt->libsrt_set_fd(srt_create_socket());
        REQUIRE(role.set_srt(srt) == 0);
        std::atomic<bool> done{false};
        std::atomic<long> passes{0};
        std::thread stats(
            [&]
            {
                SRT_TRACEBSTATS sample{};
                while (!done.load(std::memory_order_relaxed))
                {
                    role.get_statistics(&sample, 0);
                    passes.fetch_add(1, std::memory_order_relaxed);
                }
            });
        wait_for_passes(passes, 100);
        role.invalid_srt();
        wait_for_passes(passes, passes.load(std::memory_order_relaxed) + 100);
        done.store(true, std::memory_order_relaxed);
        stats.join();
        SRT_TRACEBSTATS after{};
        CHECK(role.get_statistics(&after, 0) == SLS_ERROR);
    }
    srt_cleanup();
}

TEST_CASE("E11: a worker reads its reload flag while the main thread sets it")
{
    // CSLSManager::reload() sets each worker's flag from the main thread while
    // the worker checks it at the top of every handler() pass.
    REQUIRE(srt_startup() == 0);
    {
        CSLSGroup group;
        REQUIRE(group.init_epoll() == 0);
        std::atomic<bool> done{false};
        std::atomic<long> passes{0};
        std::thread worker(
            [&]
            {
                while (!done.load(std::memory_order_relaxed))
                {
                    group.handler();
                    passes.fetch_add(1, std::memory_order_relaxed);
                }
            });
        wait_for_passes(passes, 5);
        group.reload();
        wait_for_passes(passes, passes.load(std::memory_order_relaxed) + 5);
        done.store(true, std::memory_order_relaxed);
        worker.join();
        // An empty worker stops itself once it sees the flag.
        CHECK(group.is_exit());
        group.uninit_epoll();
    }
    srt_cleanup();
}

TEST_CASE("E11: worker threads format timestamps concurrently")
{
    // srt-live-server loads the time zone before starting any thread; the
    // workers then call sls_gettime_default_string() at the same time.
    tzset();
    constexpr int kThreads = 4;
    std::atomic<long> started{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++)
        threads.emplace_back(
            [&]
            {
                // All threads format at once, not one after another.
                started.fetch_add(1, std::memory_order_relaxed);
                wait_for_passes(started, kThreads);
                char stamp[STR_DATE_TIME_LEN] = {0};
                for (int i = 0; i < 500; i++)
                    sls_gettime_default_string(stamp, sizeof(stamp));
            });
    for (auto &thread : threads)
        thread.join();
    char stamp[STR_DATE_TIME_LEN] = {0};
    sls_gettime_default_string(stamp, sizeof(stamp));
    CHECK(std::strlen(stamp) > 0);
}
