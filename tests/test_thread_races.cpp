#include "doctest.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <time.h>
#include <vector>

#include "SLSEpollThread.hpp"
#include "SLSRole.hpp"
#include "SLSSrt.hpp"
#include "common.hpp"

// Regression tests for the cross-thread fixes in ledger rows E10 and E11. Each
// reproduces one access pattern the server E2Es exposed. A data race is only
// visible to ThreadSanitizer, so these fail on the TSan ctest leg if a fix
// regresses; on the other legs they still exercise the same paths.

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
    std::thread reader(
        [&]
        {
            while (!done.load())
                (void)role.is_invalid();
        });
    role.mark_invalid();
    done = true;
    reader.join();
    CHECK(role.is_invalid());
}

TEST_CASE("E11: the bitrate is read on one thread while the owning worker updates it")
{
    BitrateRole role;
    std::atomic<bool> done{false};
    std::thread reader(
        [&]
        {
            while (!done.load())
                (void)role.get_bitrate();
        });
    for (int kbps = 1; kbps <= 1000; kbps++)
        role.store_bitrate(kbps);
    done = true;
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
        std::thread stats(
            [&]
            {
                SRT_TRACEBSTATS sample{};
                while (!done.load())
                    role.get_statistics(&sample, 0);
            });
        role.invalid_srt();
        done = true;
        stats.join();
        SRT_TRACEBSTATS after{};
        CHECK(role.get_statistics(&after, 0) == SLS_ERROR);
    }
    srt_cleanup();
}

TEST_CASE("E11: worker threads format timestamps concurrently")
{
    // srt-live-server loads the time zone before starting any thread; the
    // workers then call sls_gettime_default_string() at the same time.
    tzset();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
        threads.emplace_back(
            []
            {
                char stamp[STR_DATE_TIME_LEN] = {0};
                for (int i = 0; i < 200; i++)
                    sls_gettime_default_string(stamp, sizeof(stamp));
            });
    for (auto &thread : threads)
        thread.join();
    char stamp[STR_DATE_TIME_LEN] = {0};
    sls_gettime_default_string(stamp, sizeof(stamp));
    CHECK(std::strlen(stamp) > 0);
}
