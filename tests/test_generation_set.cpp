#include "doctest.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "SLSGenerationSet.hpp"

namespace
{

// Stands in for CSLSManager: records whether it is still alive when used.
struct Generation
{
    explicit Generation(int id_, std::atomic<int> &live_) : id(id_), live(live_)
    {
        live.fetch_add(1);
    }
    ~Generation()
    {
        alive.store(false);
        live.fetch_sub(1);
    }
    int id;
    std::atomic<int> &live;
    std::atomic<bool> alive{true};
};

} // namespace

TEST_CASE("generation set: current first, then retiring newest to oldest")
{
    std::atomic<int> live{0};
    CSLSGenerationSet<Generation> set;
    CHECK_FALSE(static_cast<bool>(set.current()));
    CHECK(set.all().empty());

    auto g1 = std::make_shared<Generation>(1, live);
    auto g2 = std::make_shared<Generation>(2, live);
    auto g3 = std::make_shared<Generation>(3, live);
    set.publish(g1);
    set.retire_current();
    CHECK_FALSE(static_cast<bool>(set.current()));
    set.publish(g2);
    set.retire_current();
    set.publish(g3);

    auto all = set.all();
    REQUIRE(all.size() == 3);
    CHECK(all[0]->id == 3);
    CHECK(all[1]->id == 2);
    CHECK(all[2]->id == 1);
    CHECK(set.retiring().size() == 2);

    set.remove(g1);
    CHECK(set.all().size() == 2);
    auto cleared = set.clear();
    CHECK(cleared.size() == 2);
    CHECK(set.all().empty());
}

TEST_CASE("generation set: a snapshot keeps a removed generation alive until it is dropped")
{
    std::atomic<int> live{0};
    CSLSGenerationSet<Generation> set;
    {
        auto g = std::make_shared<Generation>(7, live);
        set.publish(g);
    }
    auto snapshot = set.all();
    set.retire_current();
    set.remove(snapshot.front());
    CHECK(set.all().empty());
    CHECK(live.load() == 1);
    CHECK(snapshot.front()->alive.load());
    snapshot.clear();
    CHECK(live.load() == 0);
}

TEST_CASE("generation set: readers racing reloads never see a destroyed generation")
{
    // HTTP threads snapshot and use generations while the main thread
    // publishes, retires and removes them, as SIGHUP reloads do. Run under
    // TSan/ASan in CI: any use after free or unsynchronised access fails.
    std::atomic<int> live{0};
    CSLSGenerationSet<Generation> set;
    set.publish(std::make_shared<Generation>(0, live));
    std::atomic<bool> stop{false};
    std::atomic<long> uses{0};
    std::atomic<long> dead_uses{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; r++)
    {
        readers.emplace_back(
            [&]
            {
                while (!stop.load())
                {
                    for (const auto &g : set.all())
                    {
                        if (!g->alive.load())
                            dead_uses.fetch_add(1);
                        uses.fetch_add(1);
                    }
                }
            });
    }

    for (int i = 1; i <= 2000; i++)
    {
        set.retire_current();
        set.publish(std::make_shared<Generation>(i, live));
        for (const auto &old : set.retiring())
            set.remove(old);
    }
    // Let the readers make progress on the final generation before stopping.
    while (uses.load() < 4000)
    {
    }
    stop.store(true);
    for (auto &t : readers)
        t.join();

    CHECK(dead_uses.load() == 0);
    CHECK(set.all().size() == 1);
    set.clear();
    CHECK(live.load() == 0);
}
