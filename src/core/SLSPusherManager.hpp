
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

#pragma once

#include <memory>
#include <vector>
#include <string>
#include <mutex>

#include "SLSRelayManager.hpp"
#include "SLSLock.hpp"
#include "conf.hpp"

/**
 * CSLSPusherManager
 */
class CSLSPusherManager final : public CSLSRelayManager
{
public:
    CSLSPusherManager();
    ~CSLSPusherManager() override;

    int start() override;
    int add_reconnect_stream(char *relay_url) override;
    int reconnect(int64_t cur_tm_ms) override;

    // Detach + kick every live child pusher and prevent further registration.
    // Call before
    // releasing the publisher's shared_ptr. Back-pointers are weak and locked
    // for use; detach only takes m_child_relays_mutex, never m_rwclock.
    void detach_child_relays();

private:
    int connect_all();
    CSLSRelay *create_relay() override;
    int set_relay_param(std::shared_ptr<CSLSRelay> relay) override;
    int check_relay_param();
    int reconnect_all(int64_t cur_tm_ms, bool no_publisher);

    CSLSRWLock m_rwclock;
    std::map<std::string, int64_t> m_map_reconnect_relay; // relay:timeout
    // Keyed by the fully substituted URL, retained for every retry. Guarded
    // by m_rwclock along with the reconnect queue; never re-resolve a webhook URL.
    std::map<std::string, sockaddr_storage> m_vetted_by_url;

    // Weak handles to the child pushers spawned via set_relay_param(). Weak so
    // tracking never extends a pusher's lifetime; the owning worker's role map
    // remains the sole owner. Guarded by its OWN mutex (NOT m_rwclock) so the
    // detach path cannot deadlock/invert against reconnect queue updates.
    std::vector<std::weak_ptr<CSLSRelay>> m_child_relays;
    std::mutex m_child_relays_mutex;
    bool m_stopping = false; // guarded by m_child_relays_mutex
};
