#include "doctest.h"

#include "SLSListener.hpp"

// The server conf block is zeroed before parsing, so an omitted player-key
// directive used to read 0: bounds of 0 refused every key, a rate limit of 0
// refused every validation request and a timeout of 0 failed every lookup.

TEST_CASE("player key settings: omitted directives take their documented defaults")
{
    sls_conf_base_t *block = sls_conf_server_t::create_conf();
    const sls_player_key_settings_t settings = sls_player_key_settings(static_cast<sls_conf_server_t *>(block));
    CHECK(settings.auth_timeout_ms == 2000);
    CHECK(settings.cache_duration_ms == 60000);
    CHECK(settings.rate_limit_requests == -1);
    CHECK(settings.rate_limit_window_ms == 60000);
    CHECK(settings.min_length == 8);
    CHECK(settings.max_length == 64);
    sls_conf_server_t::destroy_conf(block);
}

TEST_CASE("player key settings: configured values are kept")
{
    sls_conf_base_t *block = sls_conf_server_t::create_conf();
    auto *conf = static_cast<sls_conf_server_t *>(block);
    conf->player_key_auth_timeout = 500;
    conf->player_key_cache_duration = 1000;
    conf->player_key_rate_limit_requests = 5;
    conf->player_key_rate_limit_window = 2000;
    conf->player_key_min_length = 4;
    conf->player_key_max_length = 128;
    sls_player_key_settings_t settings = sls_player_key_settings(conf);
    CHECK(settings.auth_timeout_ms == 500);
    CHECK(settings.cache_duration_ms == 1000);
    CHECK(settings.rate_limit_requests == 5);
    CHECK(settings.rate_limit_window_ms == 2000);
    CHECK(settings.min_length == 4);
    CHECK(settings.max_length == 128);

    conf->player_key_rate_limit_requests = -1;
    settings = sls_player_key_settings(conf);
    CHECK(settings.rate_limit_requests == -1);
    sls_conf_server_t::destroy_conf(block);
}
