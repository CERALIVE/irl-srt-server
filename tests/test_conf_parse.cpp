#include "doctest.h"

#include <string>
#include <vector>

#include "common.hpp"
#include "conf.hpp"
#include "SLSManager.hpp"   // sls_conf_srt_t
#include "SLSListener.hpp"  // sls_conf_server_t (+ SrtProfile via SLSSrt.hpp)
#include "SLSPublisher.hpp" // sls_conf_app_t

// CHARACTERIZATION tests for the sls.conf loader (src/core/conf.cpp). These
// drive the REAL parser end to end — sls_conf_open() over a committed fixture,
// then walk the resulting srt -> server -> app tree exactly the way
// CSLSManager::start() does — and pin the current parse result: server count,
// per-directive expanded port lists, the static profile routing
// (listen_publisher* -> L1/L2/L3), and the publisher/player domain-app split.
//
// They are deliberately a SNAPSHOT of today's behavior, not a redesign: they
// exist to lock the parser before any conf.cpp modularization (todo 32/33) so a
// regression in counts/ports/profiles/domain-app surfaces here instead of at
// bind time. Nothing in here changes parser behavior.
//
// Fixtures reused as the spec requires: src/sls.conf (the representative
// two-server example) and tests/e2e/sls-loopback.conf (the single-server e2e
// config). tests/fixtures/conf-multiport.conf additionally drives the
// comma-list / inclusive-range portlist forms through the loader.

namespace {

// DO NOT DELETE: ODR-use that force-links the srt/server/app conf-block
// registrars. They self-register from static initializers in SLSManager.o /
// SLSListenerCore.o / SLSPublisher.o, which the linker would otherwise drop
// from the static sls_core archive (unreferenced), leaving sls_conf_open()
// unable to resolve the block names ("name not found"). No behavior change.
const void *const kForceConfLinkage[] = {
    &sls_conf_srt_t::runtime_conf,
    &sls_conf_server_t::runtime_conf,
    &sls_conf_app_t::runtime_conf,
};

std::string fixture(const char *rel)
{
    (void)kForceConfLinkage;
    return std::string(SLS_TEST_SOURCE_DIR) + "/" + rel;
}

// RAII wrapper around sls_conf_open / sls_conf_close. The parser publishes its
// tree into process-global state (sls_first_conf + g_current_conf), so each
// test case must release it before the next opens — even if an assertion fails
// mid-case. Destructor guarantees that.
struct LoadedConf
{
    int ret;
    explicit LoadedConf(const std::string &path) { ret = sls_conf_open(path.c_str()); }
    ~LoadedConf() { sls_conf_close(); }
    LoadedConf(const LoadedConf &) = delete;
    LoadedConf &operator=(const LoadedConf &) = delete;

    sls_conf_srt_t *srt() const { return (sls_conf_srt_t *)sls_conf_get_root_conf(); }
};

// Expand a stored portlist spec the same way CSLSManager::start does, so the
// assertions compare the loader's stored spec against concrete ports.
std::vector<int> expand_ports(const char *spec)
{
    std::vector<int> p;
    sls_parse_port_list(spec, p);
    return p;
}

// Mirror of CSLSManager::start()'s create_for_spec() wiring: which server
// directive feeds which SRT receive/serve profile. This is the static profile
// routing under test — re-pointing a directive to a different profile (or
// renaming a directive) must break this table.
struct ListenerSpec
{
    const char *label;
    SrtProfile profile;
    std::vector<int> ports;
};

std::vector<ListenerSpec> listener_specs(const sls_conf_server_t *s)
{
    return {
        {"publisher", SrtProfile::L3Direct, expand_ports(s->listen_publisher)},
        {"publisher-srtla", SrtProfile::L1FreezeNak, expand_ports(s->listen_publisher_srtla)},
        {"publisher-srtla-classic", SrtProfile::L2Classic, expand_ports(s->listen_publisher_srtla_classic)},
        {"player", SrtProfile::L3Direct, expand_ports(s->listen_player)},
    };
}

const ListenerSpec &spec_for(const std::vector<ListenerSpec> &specs, const char *label)
{
    for (const auto &sp : specs)
    {
        if (std::string(sp.label) == label)
            return sp;
    }
    REQUIRE_MESSAGE(false, "listener spec not found: " << label);
    return specs[0];
}

} // namespace

TEST_CASE("conf loader: src/sls.conf parses to the documented two-server tree")
{
    LoadedConf conf(fixture("src/sls.conf"));
    REQUIRE(conf.ret == SLS_OK);

    sls_conf_srt_t *srt = conf.srt();
    REQUIRE(srt != nullptr);

    // srt-block scalars.
    CHECK(srt->worker_threads == 1);
    CHECK(srt->worker_connections == 300);
    CHECK(srt->http_port == 8181);
    CHECK(srt->stat_post_interval == 1);

    // Two `server {}` blocks chained via sibling.
    sls_conf_server_t *server1 = (sls_conf_server_t *)srt->child;
    REQUIRE(server1 != nullptr);
    CHECK(sls_conf_get_conf_count((sls_conf_base_t *)server1) == 2);

    SUBCASE("server 1: IRL/hybrid — all three receive profiles + player")
    {
        const sls_conf_server_t *s = server1;

        // Domain/app split: publisher route (publish/live) must differ from the
        // player route (play/live) — the documented "must not be equal" rule.
        CHECK(std::string(s->domain_publisher) == "publish");
        CHECK(std::string(s->domain_player) == "play");
        CHECK(std::string(s->default_sid) == "publish/live/feed1");

        CHECK(s->latency_min == 200);
        CHECK(s->latency_max == 5000);
        CHECK(s->backlog == 100);
        CHECK(s->idle_streams_timeout == 30);

        auto specs = listener_specs(s);
        CHECK(spec_for(specs, "publisher").ports == std::vector<int>{4001});
        CHECK(spec_for(specs, "publisher-srtla").ports == std::vector<int>{4002});
        CHECK(spec_for(specs, "publisher-srtla-classic").ports == std::vector<int>{4003});
        CHECK(spec_for(specs, "player").ports == std::vector<int>{4000});

        // Static profile routing (L1/L2/L3) per directive.
        CHECK(spec_for(specs, "publisher").profile == SrtProfile::L3Direct);
        CHECK(spec_for(specs, "publisher-srtla").profile == SrtProfile::L1FreezeNak);
        CHECK(spec_for(specs, "publisher-srtla-classic").profile == SrtProfile::L2Classic);
        CHECK(spec_for(specs, "player").profile == SrtProfile::L3Direct);

        // app block.
        sls_conf_app_t *app = (sls_conf_app_t *)s->child;
        REQUIRE(app != nullptr);
        CHECK(sls_conf_get_conf_count((sls_conf_base_t *)app) == 1);
        CHECK(std::string(app->app_player) == "live");
        CHECK(std::string(app->app_publisher) == "live");
        CHECK(app->audio_gap_fill == true);

        // Publisher route != player route once domain+app are combined.
        std::string pub_route = std::string(s->domain_publisher) + "/" + app->app_publisher;
        std::string play_route = std::string(s->domain_player) + "/" + app->app_player;
        CHECK(pub_route == "publish/live");
        CHECK(play_route == "play/live");
        CHECK(pub_route != play_route);
    }

    SUBCASE("server 2: low-latency direct SRT — no SRTLA listeners")
    {
        const sls_conf_server_t *s = (sls_conf_server_t *)server1->sibling;
        REQUIRE(s != nullptr);

        CHECK(s->latency_min == 20);
        CHECK(s->latency_max == 1000);
        CHECK(s->idle_streams_timeout == 3);

        auto specs = listener_specs(s);
        CHECK(spec_for(specs, "publisher").ports == std::vector<int>{30002});
        CHECK(spec_for(specs, "player").ports == std::vector<int>{30003});
        // No SRTLA directives in this server -> empty (memset-zeroed) specs.
        CHECK(spec_for(specs, "publisher-srtla").ports.empty());
        CHECK(spec_for(specs, "publisher-srtla-classic").ports.empty());

        sls_conf_app_t *app = (sls_conf_app_t *)s->child;
        REQUIRE(app != nullptr);
        CHECK(std::string(app->app_player) == "live");
        CHECK(std::string(app->app_publisher) == "live");
        CHECK(app->max_players_per_stream == 3);
        CHECK(app->max_input_bitrate_kbps == 20000);
        CHECK(app->max_input_bitrate_violation_timeout == 30);
        CHECK(app->max_input_bitrate_spike_tolerance == 120);
    }
}

TEST_CASE("conf loader: tests/e2e/sls-loopback.conf parses to one server, all 3 profiles")
{
    LoadedConf conf(fixture("tests/e2e/sls-loopback.conf"));
    REQUIRE(conf.ret == SLS_OK);

    sls_conf_srt_t *srt = conf.srt();
    REQUIRE(srt != nullptr);
    CHECK(srt->worker_threads == 1);
    CHECK(srt->worker_connections == 100);
    CHECK(srt->http_port == 8181);

    sls_conf_server_t *s = (sls_conf_server_t *)srt->child;
    REQUIRE(s != nullptr);
    CHECK(sls_conf_get_conf_count((sls_conf_base_t *)s) == 1);

    CHECK(std::string(s->domain_publisher) == "publish");
    CHECK(std::string(s->domain_player) == "play");
    CHECK(std::string(s->default_sid) == "publish/live/feed1");
    CHECK(s->latency_min == 200);
    CHECK(s->latency_max == 5000);
    CHECK(s->backlog == 100);
    CHECK(s->idle_streams_timeout == 30);

    auto specs = listener_specs(s);
    // L3 direct on 4001, L1 freeze+NAK on 4002, L2 Classic on 4003, players 4000.
    CHECK(spec_for(specs, "publisher").ports == std::vector<int>{4001});
    CHECK(spec_for(specs, "publisher").profile == SrtProfile::L3Direct);
    CHECK(spec_for(specs, "publisher-srtla").ports == std::vector<int>{4002});
    CHECK(spec_for(specs, "publisher-srtla").profile == SrtProfile::L1FreezeNak);
    CHECK(spec_for(specs, "publisher-srtla-classic").ports == std::vector<int>{4003});
    CHECK(spec_for(specs, "publisher-srtla-classic").profile == SrtProfile::L2Classic);
    CHECK(spec_for(specs, "player").ports == std::vector<int>{4000});
    CHECK(spec_for(specs, "player").profile == SrtProfile::L3Direct);

    sls_conf_app_t *app = (sls_conf_app_t *)s->child;
    REQUIRE(app != nullptr);
    CHECK(std::string(app->app_player) == "live");
    CHECK(std::string(app->app_publisher) == "live");
}

TEST_CASE("conf loader: multi-port lists and ranges expand through the real loader")
{
    LoadedConf conf(fixture("tests/fixtures/conf-multiport.conf"));
    REQUIRE(conf.ret == SLS_OK);

    sls_conf_srt_t *srt = conf.srt();
    REQUIRE(srt != nullptr);
    sls_conf_server_t *s = (sls_conf_server_t *)srt->child;
    REQUIRE(s != nullptr);

    auto specs = listener_specs(s);
    // list + inclusive range mix.
    CHECK(spec_for(specs, "player").ports ==
          std::vector<int>{4000, 4010, 5000, 5001, 5002});
    // plain comma list.
    CHECK(spec_for(specs, "publisher").ports == std::vector<int>{4001, 4002});
    // ascending range.
    CHECK(spec_for(specs, "publisher-srtla").ports ==
          std::vector<int>{6000, 6001, 6002, 6003});
    // single port.
    CHECK(spec_for(specs, "publisher-srtla-classic").ports == std::vector<int>{7000});

    // Profile routing is independent of how many ports a directive carries.
    CHECK(spec_for(specs, "publisher-srtla").profile == SrtProfile::L1FreezeNak);
    CHECK(spec_for(specs, "publisher-srtla-classic").profile == SrtProfile::L2Classic);
}
