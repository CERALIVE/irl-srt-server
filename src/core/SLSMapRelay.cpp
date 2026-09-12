
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

#include "SLSMapRelay.hpp"

#include <errno.h>
#include <string.h>
#include "spdlog/spdlog.h"

#include "SLSPullerManager.hpp"
#include "SLSPusherManager.hpp"
#include "SLSLog.hpp"
#include "util.hpp"

/**
 * CSLSMapRelay class implementation
 */

CSLSMapRelay::CSLSMapRelay() {}

CSLSMapRelay::~CSLSMapRelay()
{
    clear();
}

std::shared_ptr<CSLSRelayManager> CSLSMapRelay::add_relay_manager(const char *app_uplive, const char *stream_name)
{
    // find conf info
    std::shared_ptr<SLS_RELAY_INFO> sri = get_relay_conf(std::string(app_uplive));
    if (!sri)
    {
        return nullptr;
    }

    std::string key_stream_name = std::string(app_uplive) + std::string("/") + std::string(stream_name);
    CSLSLock lock(&m_rwclock, true);
    std::shared_ptr<CSLSRelayManager> cur_manager;
    std::map<std::string, std::shared_ptr<CSLSRelayManager>>::iterator item;
    item = m_map_relay_manager.find(key_stream_name);
    if (item != m_map_relay_manager.end())
    {
        cur_manager = item->second;
        if (cur_manager)
        {
            spdlog::info("[{}] CSLSMapRelay::add, cur_manager={}, exist, app_uplive={}, stream_name={}.",
                         fmt::ptr(this), fmt::ptr(cur_manager.get()), app_uplive, stream_name);
            return cur_manager;
        }
    }

    if (strcmp(sri->m_type, "pull") == 0)
        cur_manager = std::make_shared<CSLSPullerManager>();
    else if (strcmp(sri->m_type, "push") == 0)
        cur_manager = std::make_shared<CSLSPusherManager>();
    else
    {
        spdlog::info("[{}] CSLSMapRelay::add, failed, wrong relay type, app_uplive={}, stream_name={}.", fmt::ptr(this),
                     app_uplive, stream_name);
        return nullptr;
    }
    cur_manager->set_relay_conf(sri);
    cur_manager->set_relay_info(app_uplive, stream_name);

    m_map_relay_manager[key_stream_name] = cur_manager;
    spdlog::info("[{}] CSLSMapRelay::add_relay_manager, ok, app_uplive={}, stream_name={}, cur_manager={}.",
                 fmt::ptr(this), app_uplive, stream_name, fmt::ptr(cur_manager.get()));
    return cur_manager;
}

void CSLSMapRelay::clear()
{
    CSLSLock lock(&m_rwclock, true);
    spdlog::info("[{}] CSLSMapRelay::clear.", fmt::ptr(this));

    // Dropping the map's shared_ptrs releases each manager once the last
    // weak_ptr holder (a worker reconnect queue, a child relay) is done with
    // it, so this can no longer free a manager out from under a worker.
    m_map_relay_manager.clear();

    // Same reasoning as the manager map: a manager still pinned by a weak_ptr
    // holder keeps its own shared_ptr to the SRI, so clearing here can never
    // pull the config out from under an in-flight reconnect.
    m_map_relay_info.clear();
}

int CSLSMapRelay::add_relay_conf(std::string app_uplive, sls_conf_relay_t *cr)
{
    if (NULL == cr)
    {
        spdlog::error("[{}] CSLSMapRelay::add_app_conf, failed, cr is null.", fmt::ptr(this));
        return SLS_ERROR;
    }

    std::shared_ptr<SLS_RELAY_INFO> sri = get_relay_conf(app_uplive);
    if (sri)
    {
        spdlog::error("[{}] CSLSMapRelay::add_app_conf, failed, sri exists, app_uplive={}.", fmt::ptr(this),
                      app_uplive.c_str());
        return SLS_ERROR;
    }
    sri = std::make_shared<SLS_RELAY_INFO>();
    strlcpy(sri->m_type, cr->type, sizeof(sri->m_type));
    sri->m_reconnect_interval = cr->reconnect_interval;
    sri->m_idle_streams_timeout = cr->idle_streams_timeout;

    if (strcmp(cr->mode, "loop") == 0)
    {
        sri->m_mode = SLS_PM_LOOP;
    }
    else if (strcmp(cr->mode, "all") == 0)
    {
        sri->m_mode = SLS_PM_ALL;
    }
    else if (strcmp(cr->mode, "hash") == 0)
    {
        sri->m_mode = SLS_PM_HASH;
    }
    else
    {
        sri->m_mode = SLS_PM_HASH;
        spdlog::info("[{}] CSLSMapRelay::add_app_conf, wrong mode='{}', using default SLS_PM_LOOP.", fmt::ptr(this),
                     cr->mode);
    }

    // parse upstreams
    sri->m_upstreams = sls_conf_string_split(cr->upstreams, " ");
    if (sri->m_upstreams.size() == 0)
    {
        spdlog::error("[{}] CSLSMapRelay::add_app_conf, no upstreams configured, upstreams='{}'.", fmt::ptr(this),
                      cr->upstreams);
        return SLS_ERROR;
    }
    m_map_relay_info[app_uplive] = std::move(sri);
    return SLS_OK;
}

std::shared_ptr<SLS_RELAY_INFO> CSLSMapRelay::get_relay_conf(std::string app_uplive)
{
    std::shared_ptr<SLS_RELAY_INFO> sri;
    std::map<std::string, std::shared_ptr<SLS_RELAY_INFO>>::iterator item;
    item = m_map_relay_info.find(app_uplive);
    if (item != m_map_relay_info.end())
    {
        sri = item->second;
    }
    return sri;
}
