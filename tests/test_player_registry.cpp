#include "doctest.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "SLSPlayerRegistry.hpp"

namespace
{

PlayerRegistration registration(const std::string &stream_key, const std::shared_ptr<int> &role,
                                const std::shared_ptr<PlayerStatsSnapshot> &snapshot = nullptr)
{
    PlayerRegistration r;
    r.stream_key = stream_key;
    r.client_id = sls_player_client_id("203.0.113.7");
    r.connected_at_ms = 1000;
    r.latency_ms = 1000;
    r.liveness = role;
    r.snapshot = snapshot;
    return r;
}

} // namespace

TEST_CASE("PlayerRegistry: counts live players per stream")
{
    CSLSPlayerRegistry registry;
    auto a = std::make_shared<int>(1);
    auto b = std::make_shared<int>(2);
    auto c = std::make_shared<int>(3);
    registry.add(registration("live/stream/one", a));
    registry.add(registration("live/stream/one", b));
    registry.add(registration("live/stream/two", c));

    CHECK(registry.count("live/stream/one") == 2);
    CHECK(registry.count("live/stream/two") == 1);
    CHECK(registry.count("live/stream/none") == 0);
}

TEST_CASE("PlayerRegistry: a freed role stops counting")
{
    CSLSPlayerRegistry registry;
    auto a = std::make_shared<int>(1);
    auto b = std::make_shared<int>(2);
    registry.add(registration("live/stream/one", a));
    registry.add(registration("live/stream/one", b));

    a.reset();
    CHECK(registry.count("live/stream/one") == 1);
}

TEST_CASE("PlayerRegistry: a closed socket stops counting before the role is freed")
{
    CSLSPlayerRegistry registry;
    auto role = std::make_shared<int>(1);
    auto snapshot = std::make_shared<PlayerStatsSnapshot>();
    registry.add(registration("live/stream/one", role, snapshot));

    snapshot->closed = true;
    CHECK(registry.count("live/stream/one") == 0);
}

TEST_CASE("PlayerRegistry: list reports uptime and the worker's latest stats")
{
    CSLSPlayerRegistry registry;
    auto role = std::make_shared<int>(1);
    auto snapshot = std::make_shared<PlayerStatsSnapshot>();
    snapshot->rtt_ms = 42.5;
    snapshot->mbps_send_rate = 6.1;
    snapshot->pkt_snd_drop_total = 3;
    registry.add(registration("live/stream/one", role, snapshot));

    auto players = registry.list("live/stream/one", 61000);
    REQUIRE(players.size() == 1);
    CHECK(players[0].uptime_s == 60);
    CHECK(players[0].rtt_ms == doctest::Approx(42.5));
    CHECK(players[0].mbps_send_rate == doctest::Approx(6.1));
    CHECK(players[0].pkt_snd_drop_total == 3);
    CHECK(players[0].client_id.size() == 16);
}

TEST_CASE("player ids: the client id is stable per IP and does not contain it")
{
    const std::string id = sls_player_client_id("203.0.113.7");
    CHECK(id.size() == 16);
    CHECK(id == sls_player_client_id("203.0.113.7"));
    CHECK(id != sls_player_client_id("203.0.113.8"));
    CHECK(id.find("203") == std::string::npos);
}

TEST_CASE("player ids: the key id is a SHA-256 prefix and empty without a key")
{
    // printf -n abc | sha256sum -> ba7816bf8f01cfea...
    CHECK(sls_player_key_id("abc") == "ba7816bf8f01cfea");
    CHECK(sls_player_key_id("").empty());
}

namespace
{

struct LivePlayer
{
    std::shared_ptr<int> role = std::make_shared<int>(0);
    std::shared_ptr<PlayerStatsSnapshot> snapshot = std::make_shared<PlayerStatsSnapshot>();
    uint64_t session_id = sls_next_player_session_id();
};

PlayerRegistration session_registration(const std::string &stream_key, const LivePlayer &player, bool authorized,
                                        const std::string &peer_ip = "203.0.113.7")
{
    PlayerRegistration r = registration(stream_key, player.role, player.snapshot);
    r.session_id = player.session_id;
    r.peer_ip = peer_ip;
    r.authorized = authorized;
    return r;
}

} // namespace

TEST_CASE("PlayerRegistry: session ids are unique, increasing and shared by every registry")
{
    // One counter for the process, not one per registry: a reload builds a
    // new manager and registry, and its players must not reuse old ids.
    const uint64_t a = sls_next_player_session_id();
    const uint64_t b = sls_next_player_session_id();
    CSLSPlayerRegistry first;
    CSLSPlayerRegistry second;
    LivePlayer p1;
    LivePlayer p2;
    first.add(session_registration("live/stream/one", p1, true));
    second.add(session_registration("live/stream/one", p2, true));
    CHECK(a > 0);
    CHECK(b > a);
    CHECK(p1.session_id > b);
    CHECK(p2.session_id > p1.session_id);

    auto listed = second.list("live/stream/one", 1000);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].session_id == p2.session_id);
}

TEST_CASE("PlayerRegistry: a kick selects exactly one session, even when two share a peer address")
{
    // Two SRT sessions multiplexed on one peer ip:port (C61-K2): only the
    // session id tells them apart.
    CSLSPlayerRegistry registry;
    LivePlayer first;
    LivePlayer second;
    registry.add(session_registration("live/stream/one", first, true, "203.0.113.7"));
    registry.add(session_registration("live/stream/one", second, true, "203.0.113.7"));

    CHECK(registry.request_kick("live/stream/one", second.session_id));
    CHECK(second.snapshot->kick_requested.load());
    CHECK_FALSE(first.snapshot->kick_requested.load());
    // The kicked player stops counting at once; the other is untouched.
    CHECK(registry.count("live/stream/one") == 1);
    CHECK(registry.list("live/stream/one", 1000)[0].session_id == first.session_id);
}

TEST_CASE("PlayerRegistry: a kick needs the stream and the session to match together")
{
    CSLSPlayerRegistry registry;
    LivePlayer one;
    LivePlayer two;
    registry.add(session_registration("live/stream/one", one, true));
    registry.add(session_registration("live/stream/two", two, true));

    CHECK_FALSE(registry.request_kick("live/stream/two", one.session_id));
    CHECK_FALSE(registry.request_kick("live/stream", one.session_id));
    CHECK_FALSE(registry.request_kick("live/stream/one", 0));
    CHECK_FALSE(registry.request_kick("live/stream/one", one.session_id + 1000000));
    CHECK_FALSE(one.snapshot->kick_requested.load());
    CHECK_FALSE(two.snapshot->kick_requested.load());
}

TEST_CASE("PlayerRegistry: closed, freed and already-kicked sessions cannot be kicked")
{
    CSLSPlayerRegistry registry;
    LivePlayer closed;
    LivePlayer freed;
    LivePlayer kicked;
    registry.add(session_registration("live/stream/one", closed, true));
    registry.add(session_registration("live/stream/one", freed, true));
    registry.add(session_registration("live/stream/one", kicked, true));

    closed.snapshot->closed = true;
    freed.role.reset();
    CHECK(registry.request_kick("live/stream/one", kicked.session_id));

    CHECK_FALSE(registry.request_kick("live/stream/one", closed.session_id));
    CHECK_FALSE(registry.request_kick("live/stream/one", freed.session_id));
    CHECK_FALSE(registry.request_kick("live/stream/one", kicked.session_id));
    CHECK(registry.count("live/stream/one") == 0);
}

TEST_CASE("PlayerRegistry: pending players do not count against the player limit")
{
    CSLSPlayerRegistry registry;
    PlayerAdmissionLimits limits;
    limits.max_authorized = 2;
    std::vector<LivePlayer> pending(10);
    for (auto &p : pending)
        CHECK(registry.admit(session_registration("live/stream/one", p, false), limits) == PlayerAdmission::ADMITTED);
    CHECK(registry.count_pending("live/stream/one") == 10);
    CHECK(registry.count_authorized("live/stream/one") == 0);

    // An authorized reader still gets in while ten pending ones wait.
    LivePlayer hosted;
    CHECK(registry.admit(session_registration("live/stream/one", hosted, true), limits) == PlayerAdmission::ADMITTED);
    CHECK(registry.authorize("live/stream/one", pending[0].session_id, 2) == PlayerAdmission::ADMITTED);
    // The limit still caps authorized players.
    CHECK(registry.authorize("live/stream/one", pending[1].session_id, 2) == PlayerAdmission::PLAYER_LIMIT);
    LivePlayer late;
    CHECK(registry.admit(session_registration("live/stream/one", late, true), limits) == PlayerAdmission::PLAYER_LIMIT);
    CHECK(registry.admit(session_registration("live/stream/one", late, false), limits) ==
          PlayerAdmission::PLAYER_LIMIT);
    CHECK(registry.count_authorized("live/stream/one") == 2);

    // Authorizing an already authorized session is idempotent; an unknown or
    // closed one is gone.
    CHECK(registry.authorize("live/stream/one", hosted.session_id, 2) == PlayerAdmission::ADMITTED);
    pending[2].snapshot->closed = true;
    CHECK(registry.authorize("live/stream/one", pending[2].session_id, 0) == PlayerAdmission::GONE);
    CHECK(registry.authorize("live/stream/other", pending[3].session_id, 0) == PlayerAdmission::GONE);
}

TEST_CASE("PlayerRegistry: pending players are capped per stream and per peer")
{
    CSLSPlayerRegistry registry;
    PlayerAdmissionLimits limits;
    limits.max_pending_per_stream = 3;
    limits.max_pending_per_peer = 2;

    LivePlayer a1, a2, a3, b1, b2, c1;
    CHECK(registry.admit(session_registration("s/one", a1, false, "198.51.100.1"), limits) ==
          PlayerAdmission::ADMITTED);
    CHECK(registry.admit(session_registration("s/two", a2, false, "198.51.100.1"), limits) ==
          PlayerAdmission::ADMITTED);
    // A third pending socket from the same peer, on any stream, is refused.
    CHECK(registry.admit(session_registration("s/three", a3, false, "198.51.100.1"), limits) ==
          PlayerAdmission::PEER_PENDING_LIMIT);

    CHECK(registry.admit(session_registration("s/one", b1, false, "198.51.100.2"), limits) ==
          PlayerAdmission::ADMITTED);
    CHECK(registry.admit(session_registration("s/one", b2, false, "198.51.100.2"), limits) ==
          PlayerAdmission::ADMITTED);
    CHECK(registry.admit(session_registration("s/one", c1, false, "198.51.100.3"), limits) ==
          PlayerAdmission::STREAM_PENDING_LIMIT);

    // Authorized players are not pending: authorizing frees a pending slot.
    CHECK(registry.authorize("s/one", b1.session_id, 0) == PlayerAdmission::ADMITTED);
    CHECK(registry.admit(session_registration("s/one", c1, false, "198.51.100.3"), limits) ==
          PlayerAdmission::ADMITTED);
    // A closed pending socket frees its peer slot too.
    a1.snapshot->closed = true;
    CHECK(registry.admit(session_registration("s/three", a3, false, "198.51.100.1"), limits) ==
          PlayerAdmission::ADMITTED);
}

TEST_CASE("PlayerRegistry: exempt registrations skip the pending caps but not the player limit")
{
    CSLSPlayerRegistry registry;
    PlayerAdmissionLimits limits;
    limits.max_authorized = 1;
    limits.max_pending_per_stream = 1;
    limits.max_pending_per_peer = 1;

    LivePlayer flood, preview1, preview2;
    CHECK(registry.admit(session_registration("s/one", flood, false, "198.51.100.9"), limits) ==
          PlayerAdmission::ADMITTED);
    auto exempt = [](PlayerRegistration r)
    {
        r.pending_cap_exempt = true;
        return r;
    };
    CHECK(registry.admit(exempt(session_registration("s/one", preview1, false, "127.0.0.1")), limits) ==
          PlayerAdmission::ADMITTED);
    CHECK(registry.admit(exempt(session_registration("s/one", preview2, false, "127.0.0.1")), limits) ==
          PlayerAdmission::ADMITTED);
    CHECK(registry.authorize("s/one", preview1.session_id, 1) == PlayerAdmission::ADMITTED);
    CHECK(registry.authorize("s/one", preview2.session_id, 1) == PlayerAdmission::PLAYER_LIMIT);
}

TEST_CASE("PlayerRegistry: concurrent authorizations never exceed the player limit")
{
    CSLSPlayerRegistry registry;
    constexpr int kPlayers = 64;
    constexpr int kLimit = 5;
    std::vector<LivePlayer> players(kPlayers);
    for (auto &p : players)
        registry.add(session_registration("live/stream/one", p, false));

    std::atomic<int> admitted{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kPlayers; i++)
    {
        threads.emplace_back(
            [&, i]
            {
                if (registry.authorize("live/stream/one", players[i].session_id, kLimit) == PlayerAdmission::ADMITTED)
                    admitted.fetch_add(1);
            });
    }
    for (auto &t : threads)
        t.join();
    CHECK(admitted.load() == kLimit);
    CHECK(registry.count_authorized("live/stream/one") == kLimit);
}

TEST_CASE("PlayerRegistry: kicks race closes and frees without touching another session")
{
    // 1000 rounds: one thread kicks the target while another closes and frees
    // it. The target may or may not be found, but a bystander is never
    // flagged and nothing is used after release (run under TSan/ASan in CI).
    CSLSPlayerRegistry registry;
    LivePlayer bystander;
    registry.add(session_registration("live/stream/one", bystander, true));
    for (int round = 0; round < 1000; round++)
    {
        auto target = std::make_unique<LivePlayer>();
        registry.add(session_registration("live/stream/one", *target, true));
        const uint64_t id = target->session_id;
        std::atomic<bool> go{false};
        std::thread kicker(
            [&]
            {
                while (!go.load())
                {
                }
                registry.request_kick("live/stream/one", id);
            });
        std::thread closer(
            [&]
            {
                while (!go.load())
                {
                }
                target->snapshot->closed.store(true);
                target.reset();
            });
        go.store(true);
        kicker.join();
        closer.join();
        CHECK_FALSE(registry.request_kick("live/stream/one", id));
    }
    CHECK_FALSE(bystander.snapshot->kick_requested.load());
    CHECK(registry.count("live/stream/one") == 1);
}

TEST_CASE("player ids: loopback peers are recognised in every form SLS reports")
{
    CHECK(sls_is_loopback_peer("127.0.0.1"));
    CHECK(sls_is_loopback_peer("127.10.0.5"));
    CHECK(sls_is_loopback_peer("::1"));
    CHECK(sls_is_loopback_peer("::ffff:127.0.0.1"));
    CHECK_FALSE(sls_is_loopback_peer("10.0.0.1"));
    CHECK_FALSE(sls_is_loopback_peer("::ffff:10.0.0.1"));
    CHECK_FALSE(sls_is_loopback_peer("1270.0.0.1"));
    CHECK_FALSE(sls_is_loopback_peer(""));
}

TEST_CASE("pending pool: internal readers keep their own budget while the public one is full")
{
    // A public flood at its cap refuses the next public reader only.
    CHECK(sls_pending_player_room(false, 0, 0));
    CHECK(sls_pending_player_room(false, SLS_MAX_PUBLIC_PENDING_PLAYERS - 1, SLS_MAX_INTERNAL_PENDING_PLAYERS));
    CHECK_FALSE(sls_pending_player_room(false, SLS_MAX_PUBLIC_PENDING_PLAYERS, 0));
    CHECK(sls_pending_player_room(true, SLS_MAX_PUBLIC_PENDING_PLAYERS, 0));
    CHECK(sls_pending_player_room(true, SLS_MAX_PUBLIC_PENDING_PLAYERS, SLS_MAX_INTERNAL_PENDING_PLAYERS - 1));
    // The internal budget is bounded too.
    CHECK_FALSE(sls_pending_player_room(true, 0, SLS_MAX_INTERNAL_PENDING_PLAYERS));
}

TEST_CASE("boot epoch: 16 lowercase hex characters, the same for the whole process")
{
    const std::string &epoch = sls_boot_epoch();
    CHECK(sls_valid_boot_epoch(epoch));
    CHECK(&sls_boot_epoch() == &epoch);
    CHECK(sls_boot_epoch() == epoch);
}

TEST_CASE("boot epoch: only 16 lowercase hex characters are well formed")
{
    CHECK(sls_valid_boot_epoch("0123456789abcdef"));
    CHECK_FALSE(sls_valid_boot_epoch(""));
    CHECK_FALSE(sls_valid_boot_epoch("0123456789ABCDEF"));
    CHECK_FALSE(sls_valid_boot_epoch("0123456789abcde"));
    CHECK_FALSE(sls_valid_boot_epoch("0123456789abcdef0"));
    CHECK_FALSE(sls_valid_boot_epoch("0123456789abcdeg"));
    CHECK_FALSE(sls_valid_boot_epoch(" 123456789abcdef"));
}

TEST_CASE("reserved grants: one pending grant per key, a bounded number per peer")
{
    // The first presenter of a reserved key gets its grant; a copy of the key
    // from anywhere else is public while that grant is pending.
    CHECK(sls_reserved_grant_allowed(0, 0));
    CHECK_FALSE(sls_reserved_grant_allowed(SLS_MAX_RESERVED_GRANTS_PER_KEY, 0));
    // One peer presenting many reserved keys is capped too.
    CHECK(sls_reserved_grant_allowed(0, SLS_MAX_RESERVED_GRANTS_PER_PEER - 1));
    CHECK_FALSE(sls_reserved_grant_allowed(0, SLS_MAX_RESERVED_GRANTS_PER_PEER));
    CHECK(SLS_MAX_RESERVED_GRANTS_PER_KEY == 1);
}
