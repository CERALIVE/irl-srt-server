
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
#include <sys/socket.h>

#include "SLSRelay.hpp"
#include "SLSMapPublisher.hpp"
#include "conf.hpp"
#include "SLSRoleList.hpp"

struct SLS_RELAY_INFO
{
    std::vector<std::string> m_upstreams;
    // Addresses pre-vetted by validate_push_url, index-aligned with
    // m_upstreams. Empty for static-config relays and pullers, which never run
    // the push-URL validator and keep the legacy resolve-at-open path.
    std::vector<sockaddr_storage> m_vetted_addrs;
    char m_type[32];
    int m_mode;
    int m_reconnect_interval;   // unit: s
    int m_idle_streams_timeout; // unit: s
};

/**
 * CSLSRelayManager
 */
// Relay managers are owned by std::shared_ptr and referenced elsewhere by
// std::weak_ptr. The per-publisher dynamic pusher manager (CSLSPublisher::
// m_dynamic_pusher_manager) has a session lifetime, so a raw back-pointer held
// by a worker's reconnect queue or by a child CSLSRelay could outlive it -- and
// did: check_reconnect_relay kept calling reconnect() on a freed manager every
// housekeeping pass, which is what surfaced as "invalid mode | mode=48" for a
// field only ever assigned SLS_PM_ALL. Every holder now takes a weak_ptr and
// locks it, so a freed manager is observed as expired instead of dereferenced.
class CSLSRelayManager : public std::enable_shared_from_this<CSLSRelayManager>
{
public:
    CSLSRelayManager();
    virtual ~CSLSRelayManager();

    virtual int start() = 0;
    virtual int reconnect(int64_t cur_tm_ms) = 0;

    virtual int add_reconnect_stream(char *relay_url) = 0;

    void set_map_publisher(CSLSMapPublisher *publisher);
    void set_map_data(CSLSMapData *map_data);
    void set_role_list(CSLSRoleList *role_list);

    void set_relay_conf(std::shared_ptr<SLS_RELAY_INFO> sri);
    void set_relay_info(const char *app_uplive, const char *stream_name);
    void set_listen_port(int port);

protected:
    CSLSMapPublisher *m_map_publisher;
    CSLSMapData *m_map_data;
    CSLSRoleList *m_role_list;
    // Owned, not borrowed. A manager can briefly outlive the object that
    // created its config (a publisher's dynamic pusher SRI, or CSLSMapRelay's
    // conf map across a reload) because holders lock a weak_ptr to the manager
    // and pin it for the call. Sharing the SRI keeps m_sri valid for exactly
    // as long as the manager itself.
    std::shared_ptr<SLS_RELAY_INFO> m_sri;
    int64_t m_reconnect_begin_tm; // unit: ms
    int m_listen_port;

    char m_app_uplive[1024];
    char m_stream_name[1024];

    int connect(const char *url, const sockaddr_storage *vetted_addr = nullptr);
    int connect_hash();

    virtual CSLSRelay *create_relay() = 0;
    std::string get_hash_url();

    virtual int set_relay_param(std::shared_ptr<CSLSRelay> relay) = 0;
};
