#include "doctest.h"

#include <memory>

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
