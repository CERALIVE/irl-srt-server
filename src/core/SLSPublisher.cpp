
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

#include <errno.h>
#include <string.h>
#include "spdlog/spdlog.h"

#include "SLSPublisher.hpp"
#include "SLSPlayer.hpp"
#include "SLSLog.hpp"
#include "SLSLogCategory.hpp"
#include "SLSPusherManager.hpp"
#include "SLSRelayManager.hpp"
#include "util.hpp"

/**
 * app conf
 */
SLS_CONF_DYNAMIC_IMPLEMENT(app)

/**
 * CSLSPublisher class implementation
 */

CSLSPublisher::CSLSPublisher()
{
    m_is_write = 0;
    m_map_publisher = NULL;

    snprintf(m_role_name, sizeof(m_role_name), "publisher");

    // Record publisher start for summary logging
    sls_get_summary_logger().record_publisher_start();
}

CSLSPublisher::~CSLSPublisher()
{
    // release
    //  Record publisher stop for summary logging
    sls_get_summary_logger().record_publisher_stop();
}

int CSLSPublisher::init()
{
    int ret = CSLSRole::init();
    if (m_conf)
    {
        sls_conf_app_t *app_conf = ((sls_conf_app_t *)m_conf);
        // m_exit_delay = ((sls_conf_app_t *)m_conf)->publisher_exit_delay;

        // Initialize bitrate limiter if configured
        if (app_conf->max_input_bitrate_kbps > 0)
        {
            int violation_timeout = app_conf->max_input_bitrate_violation_timeout;
            if (violation_timeout <= 0)
            {
                violation_timeout = 30; // Default to 30 seconds if not configured
            }
            // Spike tolerance: config is percentage (e.g. 120 = 1.2x), default to 120 if not set
            float spike_tolerance = 1.2f;
            if (app_conf->max_input_bitrate_spike_tolerance > 0)
            {
                spike_tolerance = app_conf->max_input_bitrate_spike_tolerance / 100.0f;
            }
            ret = init_bitrate_limiter(app_conf->max_input_bitrate_kbps, violation_timeout, spike_tolerance);
            if (ret != SLS_OK)
            {
                spdlog::error("[{}] CSLSPublisher::init, failed to initialize bitrate limiter", fmt::ptr(this));
                return ret;
            }
        }
    }

    return ret;
}

int CSLSPublisher::uninit()
{
    int ret = SLS_OK;

    // Dynamic pusher teardown order is load-bearing (UAF fix): (1) detach+kick
    // the child pushers so none reconnects through the manager; (2) drop the
    // publisher from the maps so a concurrent reconnect() sees no_publisher and
    // spawns no fresh child; (3) ONLY THEN free the manager+SRI. Freeing first
    // (the old order) let an orphaned pusher deref a freed manager.
    if (m_dynamic_pusher_manager)
    {
        m_dynamic_pusher_manager->detach_child_relays();
    }

    if (m_map_data)
    {
        // Ownership check before deleting the ring by key. The takeover flow
        // (kick incumbent -> reap -> refuse new conn -> re-register) means the
        // reaped publisher is normally still the registered owner here, but
        // that is an ordering invariant, not a structural one: if a future
        // change ever lets a new publisher register before the old one is
        // reaped, an unconditional remove() would yank the live ring out from
        // under the new session and every viewer on it. In that case skip the
        // delete and let the new owner's ring (and its own uninit) manage it.
        std::shared_ptr<CSLSRole> current_owner =
            m_map_publisher ? m_map_publisher->get_publisher(m_map_data_key) : nullptr;
        if (current_owner && current_owner.get() != this)
        {
            spdlog::warn("[{}] CSLSPublisher::uninit, stream={} is now owned by publisher [{}], "
                         "skipping ring removal.",
                         fmt::ptr(this), m_map_data_key, fmt::ptr(current_owner.get()));
        }
        else
        {
            // Flight recorder: the diagnostic gauges live on the ring we are about
            // to free, so publisher takeover / reconnect would otherwise erase the
            // one session's data an operator most wants (the streamer got fed up and
            // reconnected). Flush the ring's final peaks to a single greppable line
            // (stream=<name>) before remove() drops it. One line per session end, so
            // it stays quiet even with many concurrent streams.
            spdlog::info("[{}] CSLSPublisher::uninit, session end stream={}, peakReaderBacklogBytes={}, "
                         "ringOverruns={}, viewerBackpressure={}.",
                         fmt::ptr(this), m_map_data_key, get_max_reader_backlog(false), get_ring_overrun_count(),
                         get_viewer_backpressure_events(false));
            ret = m_map_data->remove(m_map_data_key);
            spdlog::info("[{}] CSLSPublisher::uninit, removed publisher from m_map_data, ret={:d}.", fmt::ptr(this),
                         ret);
        }
    }

    if (m_map_publisher)
    {
        ret = m_map_publisher->remove(this);
        spdlog::info("[{}] CSLSPublisher::uninit, removed publisher from m_map_publisher, ret={:d}.", fmt::ptr(this),
                     ret);
    }

    if (m_dynamic_pusher_manager)
    {
        m_dynamic_pusher_manager.reset();
        spdlog::info("[relay] dynamic pusher torn down for {}", m_map_data_key);
    }
    m_dynamic_pusher_sri.reset();

    return CSLSRole::uninit();
}

void CSLSPublisher::set_map_publisher(CSLSMapPublisher *publisher)
{
    m_map_publisher = publisher;
}

int CSLSPublisher::handler()
{
    int ret = handler_read_data();
    // The webhook flips m_http_passed and populates m_push_urls
    // asynchronously inside check_http_passed; once both happen we spin up
    // exactly one CSLSPusherManager carrying every accepted URL.
    if (ret >= 0 && m_ring_added.load(std::memory_order_acquire) && !m_dynamic_pusher_manager && !m_push_urls.empty())
    {
        try_spawn_dynamic_pusher();
    }
    return ret;
}

void CSLSPublisher::on_worker_tick()
{
    if (!m_http_passed.load(std::memory_order_acquire))
        check_http_passed();
}

void CSLSPublisher::try_spawn_dynamic_pusher()
{
    if (m_role_list == nullptr || m_map_data == nullptr || m_map_publisher == nullptr)
    {
        spdlog::warn("[relay] cannot spawn dynamic pusher for {}: missing context (role_list/map_data/map_publisher)",
                     m_map_data_key);
        // Avoid retrying every handler tick on a misconfigured deploy.
        m_push_urls.clear();
        m_push_vetted_addrs.clear();
        return;
    }

    // Split m_map_data_key ("app_uplive/stream_name") for the pusher manager.
    const char *slash = strchr(m_map_data_key, '/');
    if (slash == nullptr || slash == m_map_data_key)
    {
        spdlog::warn("[relay] cannot spawn dynamic pusher: malformed key='{}'", m_map_data_key);
        m_push_urls.clear();
        m_push_vetted_addrs.clear();
        return;
    }
    std::string app_uplive(m_map_data_key, slash - m_map_data_key);
    std::string stream_name(slash + 1);

    if (m_push_vetted_addrs.size() != m_push_urls.size())
    {
        spdlog::warn("[relay] cannot spawn dynamic pusher: missing vetted destination addresses");
        m_push_urls.clear();
        m_push_vetted_addrs.clear();
        return;
    }

    m_dynamic_pusher_sri = std::make_shared<SLS_RELAY_INFO>();
    snprintf(m_dynamic_pusher_sri->m_type, sizeof(m_dynamic_pusher_sri->m_type), "push");
    m_dynamic_pusher_sri->m_mode = SLS_PM_ALL;
    m_dynamic_pusher_sri->m_reconnect_interval = 10;
    m_dynamic_pusher_sri->m_idle_streams_timeout = 10;
    m_dynamic_pusher_sri->m_upstreams = m_push_urls;
    m_dynamic_pusher_sri->m_vetted_addrs = m_push_vetted_addrs;

    m_dynamic_pusher_manager = std::make_shared<CSLSPusherManager>();
    m_dynamic_pusher_manager->set_relay_conf(m_dynamic_pusher_sri);
    m_dynamic_pusher_manager->set_relay_info(app_uplive.c_str(), stream_name.c_str());
    m_dynamic_pusher_manager->set_map_data(m_map_data);
    m_dynamic_pusher_manager->set_map_publisher(m_map_publisher);
    m_dynamic_pusher_manager->set_role_list(m_role_list);
    m_dynamic_pusher_manager->set_listen_port(m_listen_port);

    if (SLS_OK != m_dynamic_pusher_manager->start())
    {
        spdlog::warn("[relay] dynamic pusher start failed for {}, will retry on reconnect", m_map_data_key);
        // Leave the manager allocated so the reconnect loop in CSLSGroup
        // gets a chance to retry; teardown in uninit() will clean it up.
    }
    else
    {
        spdlog::info("[relay] dynamic pusher started for {} | upstream_count={}", m_map_data_key, m_push_urls.size());
    }
}

void CSLSPublisher::on_map_data_set()
{
    // The ring (and the scanner state that hangs off its key) is allocated
    // lazily on the first authorized packet, so at accept-time set_map_data()
    // there is nothing to enable yet. handler_read_data re-invokes this hook
    // right after the lazy add, with m_ring_added set.
    if (!m_ring_added.load(std::memory_order_acquire))
        return;
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0)
        return;

    if (is_audio_gap_fill_enabled())
    {
        m_map_data->set_audio_gap_fill(m_map_data_key, true);
        spdlog::info("[{}] CSLSPublisher::on_map_data_set, audio gap filling enabled for {}", fmt::ptr(this),
                     m_map_data_key);
    }

    const sls_conf_app_t *app_conf = (const sls_conf_app_t *)m_conf;
    if (app_conf != nullptr && app_conf->timecode_sei)
    {
        m_map_data->set_timecode_scan(m_map_data_key, true);
    }
}

bool CSLSPublisher::is_audio_gap_fill_enabled() const
{
    const sls_conf_app_t *app_conf = (const sls_conf_app_t *)m_conf;
    return app_conf && app_conf->audio_gap_fill;
}
