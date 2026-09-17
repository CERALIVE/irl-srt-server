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
#include <sys/stat.h>
#include <cassert>
#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"
#include "SLSRole.hpp"
#include "SLSLog.hpp"
#include "SLSLogCategory.hpp"
#include "SLSPublisher.hpp"
#include "SLSPushUrlValidator.hpp"
#include "util.hpp"
#include "SLSBitrateLimit.hpp"
#include "auth_reject_cache.hpp"
#include "sls_sid.hpp"
#include "sls_idle.hpp"

namespace
{
constexpr int kHttpRequestTimeoutSeconds = 5;
constexpr int64_t kHttpAuthorizationDeadlineMs = 4000;
} // namespace

CSLSRole::CSLSRole()
{
    m_srt = nullptr;
    m_is_write = true;
    m_stat_start_time = sls_gettime_ms();
    m_invalid_begin_tm = sls_gettime_ms();
    m_stat_bitrate_last_tm = m_invalid_begin_tm;
    m_stat_bitrate_interval = 1000;
    m_stat_bitrate_datacount = 0;
    m_kbitrate = 0;
    m_idle_streams_timeout = 10;
    m_latency = 20;
    m_state = SLS_RS_UNINIT;
    m_back_log = 1024;
    m_port = 0;
    memset(m_peer_ip, 0, IP_MAX_LEN);
    m_peer_port = 0;
    memset(m_role_name, 0, STR_MAX_LEN);
    memset(m_streamid, 0, URL_MAX_LEN);
    memset(m_http_url, 0, URL_MAX_LEN);
    m_http_passed.store(true, std::memory_order_relaxed);
    m_conf = nullptr;
    m_map_data = nullptr;
    memset(m_map_data_key, 0, URL_MAX_LEN);
    memset(&m_map_data_id, 0, sizeof(SLSRecycleArrayID));
    memset(m_data, 0, DATA_BUFF_SIZE);
    m_data_len = 0;
    m_data_pos = 0;
    m_need_reconnect.store(false, std::memory_order_relaxed);
    m_http_future = nullptr;
    m_bitrate_limiter = nullptr;
    snprintf(m_role_name, sizeof(m_role_name), "role");
}

CSLSRole::~CSLSRole()
{
    cleanup_bitrate_limiter();
    // VirtualCall: uninit() is virtual and overridden by CSLSListener,
    // CSLSRelay (and its CSLSPuller/CSLSPusher leaves) and CSLSPublisher, so this
    // base call resolves to CSLSRole::uninit() rather than the override. That is
    // correct here: the owning worker / role container always invokes the derived
    // uninit() on the LIVE object before the role's shared_ptr is released
    // (CSLSGroup::check_invalid_sock / clear / check_new_role / stop and
    // CSLSRoleList::erase / reap_unadopted), and CSLSListener additionally calls
    // it from its own dtor (SLSListenerCore.cpp). That deterministic teardown —
    // not this dtor — performs the derived map self-removal; the design moved
    // cleanup out of the destructor precisely because a shared_ptr held in a map
    // would never reach zero otherwise (see CSLSGroup::check_new_role). By the
    // time we get here m_state == SLS_RS_UNINIT and m_srt == NULL, so
    // CSLSRole::uninit() is an idempotent backstop no-op.
    uninit(); // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
}

int CSLSRole::init()
{
    m_state = SLS_RS_INITED;
    m_map_data_id.bFirst = true;
    m_map_data_id.nDataCount = 0;
    m_map_data_id.nReadPos = 0;
    return SLS_OK;
}

int CSLSRole::uninit()
{
    m_http_future = nullptr;
    m_http_auth_deadline_ms.store(0, std::memory_order_release);
    if (SLS_RS_UNINIT != m_state.load(std::memory_order_acquire))
    {
        m_state = SLS_RS_UNINIT;
        // invalid_srt() unsubscribes before closing on every teardown path.
        invalid_srt();
    }
    return SLS_OK;
}

int CSLSRole::invalid_srt()
{
    bool closed = false;
    {
        std::lock_guard<std::mutex> socket_lock(m_srt_lifetime_mutex);
        if (!m_srt)
            return SLS_OK;

            // Single-owner teardown (Todo 19): roles live behind a
            // std::shared_ptr<CSLSRole> and only one thread ever frees m_srt for a
            // given role. The owning worker serialises get_state()/handler()/
            // invalid_srt() in its loop; a cross-thread kick only flips
            // m_kick_requested and the owner does the teardown in get_state(); and at
            // shutdown the workers are joined before the main thread drains the rest,
            // so ownership transfers but never overlaps. The debug tripwire below
            // claims this role's teardown for the current thread and asserts no
            // second thread is concurrently inside — the double-free of m_srt a
            // broken model would cause. It passes on the serialized shutdown handoff
            // (no concurrency) and fires only on a real race; the read/write-vs-delete
            // race is separately covered by the task-19 TSan storm. No runtime mutex
            // is added on the socket hot path.
#ifndef NDEBUG
        std::thread::id prev = std::thread::id{};
        bool claimed =
            m_invalidating_tid.compare_exchange_strong(prev, std::this_thread::get_id(), std::memory_order_acq_rel);
        assert(claimed && "CSLSRole::invalid_srt entered concurrently by a second thread "
                          "(single-owner shared_ptr teardown violated; would double-free m_srt)");
        (void)claimed;
#endif
        int fd = m_srt->libsrt_get_fd();
        spdlog::info("[{}] CSLSRole::invalid_srt, close sock={:d}, m_state={:d}.", fmt::ptr(this), fd,
                     m_state.load(std::memory_order_acquire));
        // Unsubscribe while the fd is valid: srt_close alone can leave a stale
        // subscription waking the worker after its role-map entry is gone.
        m_srt->libsrt_remove_from_epoll();
        m_epoll_out_armed = false;
        m_srt->libsrt_close();
        delete m_srt;
        m_srt = nullptr;
        closed = true;

        if (m_player_snapshot)
            m_player_snapshot->closed.store(true, std::memory_order_relaxed);
#ifndef NDEBUG
        m_invalidating_tid.store(std::thread::id{}, std::memory_order_release);
#endif
    }
    if (closed)
        on_close();
    return SLS_OK;
}

void CSLSRole::mark_invalid()
{
    m_state = SLS_RS_INVALID;
    invalid_srt();
}

void CSLSRole::request_kick()
{
    // Release pairs with the owning worker's acquire in get_state().
    m_kick_requested.store(true, std::memory_order_release);
}

int CSLSRole::get_state(int64_t cur_time_ms)
{
    if (SLS_RS_INVALID == m_state.load(std::memory_order_acquire))
        return m_state.load(std::memory_order_acquire);
    if (m_kick_requested.load(std::memory_order_acquire))
    {
        spdlog::info("[{}] CSLSRole::get_state, kick requested for {} stream={}, fd={:d}.", fmt::ptr(this), m_role_name,
                     sls_redact_secret(m_map_data_key), get_fd());
        mark_invalid();
        return m_state.load(std::memory_order_acquire);
    }
    if (check_idle_streams_duration(cur_time_ms))
    {
        const bool never_received = m_last_recv_data_tm.load(std::memory_order_relaxed) == 0;
        spdlog::info("[{}] CSLSRole::get_state, idle reap for {}, fd={:d}, never_received_data={}, "
                     "first_data_timeout={:d}ms, idle_timeout={:d}s.",
                     fmt::ptr(this), m_role_name, get_fd(), never_received, m_first_data_timeout_ms,
                     m_idle_streams_timeout);
        mark_invalid();
        return m_state.load(std::memory_order_acquire);
    }
    int ret = get_sock_state();
    if (SLS_ERROR == ret || SRTS_BROKEN == ret || SRTS_CLOSED == ret || SRTS_NONEXIST == ret)
    {
        spdlog::info("[{}] CSLSRole::get_state, {} stream={}, get_sock_state ret={:d}, closing connection.",
                     fmt::ptr(this), m_role_name, sls_redact_secret(m_map_data_key), ret);
        mark_invalid();
    }
    return m_state.load(std::memory_order_acquire);
}

int CSLSRole::handler()
{
    return SLS_OK;
}

int CSLSRole::get_fd()
{
    std::lock_guard<std::mutex> socket_lock(m_srt_lifetime_mutex);
    if (m_srt)
        return m_srt->libsrt_get_fd();
    return 0;
}

int CSLSRole::set_eid(int eid)
{
    if (m_srt)
        return m_srt->libsrt_set_eid(eid);
    return 0;
}

int CSLSRole::set_srt(CSLSSrt *srt)
{
    std::lock_guard<std::mutex> socket_lock(m_srt_lifetime_mutex);
    if (m_srt)
    {
        spdlog::error("[{}] CSLSRole::setSrt, m_srt={} is not null.", fmt::ptr(this), fmt::ptr(m_srt));
        return SLS_ERROR;
    }
    m_srt = srt;
    return SLS_OK;
}

int CSLSRole::write(const char *buf, int size)
{
    if (m_srt == nullptr)
    {
        spdlog::error("[{}] CSLSRole::write, m_srt is NULL, cannot write {:d} bytes.", fmt::ptr(this), size);
        return SLS_ERROR;
    }
    if (buf == nullptr || size <= 0)
    {
        spdlog::error("[{}] CSLSRole::write, invalid parameters: buf={}, size={:d}.", fmt::ptr(this), fmt::ptr(buf),
                      size);
        return SLS_ERROR;
    }
    return m_srt->libsrt_write(buf, size);
}

int CSLSRole::add_to_epoll(int eid)
{
    int ret = SLS_ERROR;
    if (m_srt)
    {
        m_srt->libsrt_set_eid(eid);
        ret = m_srt->libsrt_add_to_epoll(eid, m_is_write);
        if (SLS_OK == ret)
            m_epoll_out_armed = false;
        spdlog::trace("[{}] CSLSRole::add_to_epoll, {}, sock={:d}, m_is_write={:d}, ret={:d}.", fmt::ptr(this),
                      m_role_name, get_fd(), m_is_write, ret);
    }
    return ret;
}

int CSLSRole::remove_from_epoll()
{
    int ret = SLS_ERROR;
    if (m_srt)
    {
        ret = m_srt->libsrt_remove_from_epoll();
        if (SLS_OK == ret)
            m_epoll_out_armed = false;
        spdlog::trace("[{}] CSLSRole::remove_from_epoll, {}, sock={:d}, ret={:d}.", fmt::ptr(this), m_role_name,
                      get_fd(), ret);
    }
    return ret;
}

int CSLSRole::set_epoll_out(bool enable)
{
    if (m_srt == nullptr)
        return SLS_ERROR;
    if (enable == m_epoll_out_armed)
        return SLS_OK;
    int ret = m_srt->libsrt_arm_epoll_out(enable);
    if (SLS_OK == ret)
        m_epoll_out_armed = enable;
    return ret;
}

void CSLSRole::update_egress_arming()
{
    if (!m_is_write || is_invalid())
        return;
    // Staged data we couldn't fully send means backpressure; otherwise disarm
    // OUT so an always-writable socket doesn't busy-return from epoll_wait.
    set_epoll_out(m_data_pos < m_data_len);
}

int CSLSRole::get_sock_state()
{
    if (m_srt)
        return m_srt->libsrt_getsockstate();
    return SLS_ERROR;
}

char *CSLSRole::get_role_name()
{
    return m_role_name;
}

char *CSLSRole::get_streamid()
{
    if (strlen(m_streamid) != 0)
        return m_streamid;
    int sid_size = sizeof(m_streamid);
    if (m_srt)
        m_srt->libsrt_getsockopt(SRTO_STREAMID, "SRTO_STREAMID", m_streamid, &sid_size);
    return m_streamid;
}

void CSLSRole::set_streamid(const char *sid)
{
    if (sid != nullptr)
        strlcpy(m_streamid, sid, sizeof(m_streamid));
}

char *CSLSRole::get_map_data_key()
{
    return m_map_data_key;
}

bool CSLSRole::is_reconnect()
{
    return m_need_reconnect.load(std::memory_order_relaxed);
}

void CSLSRole::set_conf(sls_conf_base_t *conf)
{
    m_conf = conf;
}

void CSLSRole::set_map_data(const char *map_key, CSLSMapData *map_data)
{
    if (map_key != nullptr)
    {
        strlcpy(m_map_data_key, map_key, sizeof(m_map_data_key));
        m_map_data = map_data;
        on_map_data_set();
    }
    else
    {
        spdlog::error("[{}] CSLSRole::set_map_data, failed, map_key is null.", fmt::ptr(this));
    }
}

void CSLSRole::set_idle_streams_timeout(int timeout)
{
    m_idle_streams_timeout = timeout;
}

void CSLSRole::set_first_data_timeout(int timeout_ms)
{
    m_first_data_timeout_ms = timeout_ms;
    if (timeout_ms > 0 && m_http_passed.load(std::memory_order_acquire))
        m_invalid_begin_tm = sls_gettime_ms();
}

bool CSLSRole::has_recent_recv_data(int64_t now_ms, int64_t within_ms) const
{
    int64_t last = m_last_recv_data_tm.load(std::memory_order_relaxed);
    if (last == 0)
        return false;
    return (now_ms - last) <= within_ms;
}

bool CSLSRole::check_idle_streams_duration(int64_t cur_time_ms)
{
    if (cur_time_ms == 0)
        cur_time_ms = sls_gettime_ms();
    // Until the first read, m_invalid_begin_tm is the connect timestamp.
    return sls_should_reap_role(cur_time_ms, m_last_recv_data_tm.load(std::memory_order_relaxed), m_invalid_begin_tm,
                                m_first_data_timeout_ms, m_idle_streams_timeout,
                                !m_http_passed.load(std::memory_order_acquire));
}

int CSLSRole::check_http_client()
{
    return m_http_future ? SLS_OK : SLS_ERROR;
}

int CSLSRole::close()
{
    std::lock_guard<std::mutex> socket_lock(m_srt_lifetime_mutex);
    if (m_srt)
    {
        m_srt->libsrt_remove_from_epoll();
        m_epoll_out_armed = false;
        m_srt->libsrt_close();
        delete m_srt;
        m_srt = nullptr;
    }
    return SLS_OK;
}

int CSLSRole::handler_read_data(int64_t *last_read_time)
{
    char szData[TS_UDP_LEN];
    if (SLS_OK != check_http_passed())
        return is_invalid() ? SLS_ERROR : SLS_OK;
    if (m_srt == nullptr)
    {
        spdlog::error("[{}] CSLSRole::handler_read_data, m_srt is null.", fmt::ptr(this));
        return SLS_ERROR;
    }
    int n = m_srt->libsrt_read(szData, TS_UDP_LEN);
    if (n <= 0)
    {
        int err_no = n < 0 ? CSLSSrt::libsrt_lasterror() : 0;
        if (err_no == SRT_EASYNCRCV)
        {
            // Spurious IN wake: no media or reader timestamp was delivered.
            SPDLOG_TRACE("[{}] CSLSRole::handler_read_data, no data ready, ignoring wake.", fmt::ptr(this));
            return SLS_OK;
        }
        if (err_no == SRT_ECONNLOST || err_no == SRT_ENOCONN)
            spdlog::info("[{}] CSLSRole::handler_read_data, peer gone (errno={:d}), closing connection.",
                         fmt::ptr(this), err_no);
        else
            spdlog::error("[{}] CSLSRole::handler_read_data, libsrt_read failure, n={:d}, errno={:d}, expected={:d}.",
                          fmt::ptr(this), n, err_no, TS_UDP_LEN);
        return SLS_ERROR;
    }
    // Reject misaligned input before it can reach any TS parser.
    if (n % TS_PACK_LEN != 0)
    {
        spdlog::error("[{}] CSLSRole::handler_read_data, dropping non-188-aligned read n={:d}.", fmt::ptr(this), n);
        mark_invalid();
        return SLS_ERROR;
    }
    m_invalid_begin_tm = sls_gettime_ms();
    m_last_recv_data_tm.store(m_invalid_begin_tm, std::memory_order_relaxed);
    if (m_bitrate_limiter)
    {
        auto result = m_bitrate_limiter->check_data_bitrate(n, m_invalid_begin_tm);
        if (result == CSLSBitrateLimit::BITRATE_DISCONNECT)
        {
            spdlog::error("[{}] CSLSRole::handler_read_data, disconnecting stream due to bitrate limit violation",
                          fmt::ptr(this));
            mark_invalid();
            return SLS_ERROR;
        }
    }
    m_stat_bitrate_datacount += n;
    int64_t d = m_invalid_begin_tm - m_stat_bitrate_last_tm;
    // A non-positive interval or clock delta must never divide by zero.
    if (d > 0 && d >= m_stat_bitrate_interval)
    {
        m_kbitrate = (int)(m_stat_bitrate_datacount * 8 / d);
        m_stat_bitrate_datacount = 0;
        m_stat_bitrate_last_tm = m_invalid_begin_tm;
    }
    if (n != TS_UDP_LEN)
        SPDLOG_TRACE("[{}] CSLSRole::handler_read_data, libsrt_read n={:d}, expect {:d}.", fmt::ptr(this), n,
                     TS_UDP_LEN);
    if (m_map_data == nullptr)
    {
        spdlog::error("[{}] CSLSRole::handler_read_data, no data handled, m_map_data is NULL.", fmt::ptr(this));
        return SLS_ERROR;
    }
    // Lazily allocate only on authorized media, not accept: silent/unauthenticated
    // connections must not pin a multi-megabyte ring. Relay adds are idempotent.
    if (!m_ring_added.load(std::memory_order_acquire))
    {
        int bitrate_hint = m_conf ? ((sls_conf_app_t *)m_conf)->max_input_bitrate_kbps : 0;
        if (SLS_OK != m_map_data->add(m_map_data_key, bitrate_hint, m_latency))
        {
            spdlog::error("[{}] CSLSRole::handler_read_data, m_map_data->add failed for key='{}' (stream/memory cap?), "
                          "kicking role.",
                          fmt::ptr(this), sls_redact_secret(m_map_data_key));
            mark_invalid();
            return SLS_ERROR;
        }
        m_ring_added.store(true, std::memory_order_release);
        on_map_data_set();
    }
    SPDLOG_TRACE("[{}] CSLSRole::handler_read_data, ok, libsrt_read n={:d}.", fmt::ptr(this), n);
    return m_map_data->put(m_map_data_key, szData, n, last_read_time);
}

int CSLSRole::get_statistics(SRT_TRACEBSTATS *currentStats, int clear)
{
    std::lock_guard<std::mutex> socket_lock(m_srt_lifetime_mutex);
    if (m_srt)
        return m_srt->libsrt_get_statistics(currentStats, clear);
    return SLS_ERROR;
}

int CSLSRole::get_bitrate()
{
    return m_kbitrate.load(std::memory_order_relaxed);
}

int CSLSRole::get_uptime()
{
    int64_t difference = sls_gettime_ms() - m_stat_start_time;
    return static_cast<int>(difference / 1000);
}

int CSLSRole::handler_write_data()
{
    int write_size = 0;
    if (SLS_OK != check_http_passed())
        return is_invalid() ? SLS_ERROR : SLS_OK;
    if (m_srt == nullptr)
    {
        spdlog::error("[{}] CSLSRole::handler_write_data, m_srt is NULL, cannot write data.", fmt::ptr(this));
        return SLS_ERROR;
    }
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0)
    {
        spdlog::error("[{}] CSLSRole::handler_write_data, no publisher ring binding.", fmt::ptr(this));
        return SLS_ERROR;
    }
    // Bound each worker pass to two batches: don't burst a stale live backlog.
    // EASYNCSND retains the cursor and OUT wakes the worker to retry it.
    for (int batch = 0; batch < MAX_EGRESS_BATCHES; ++batch)
    {
        if (m_srt == nullptr)
            return SLS_ERROR;
        if (m_data_len < TS_UDP_LEN)
        {
            int got = m_map_data->get(m_map_data_key, m_data, DATA_BUFF_SIZE, &m_map_data_id, TS_UDP_LEN);
            if (got <= 0)
                break;
            m_data_pos = 0;
            m_data_len = got;
            m_stat_bitrate_datacount += got;
            m_invalid_begin_tm = sls_gettime_ms();
            int64_t d = m_invalid_begin_tm - m_stat_bitrate_last_tm;
            if (d > 0 && d >= m_stat_bitrate_interval)
            {
                m_kbitrate = static_cast<int>(m_stat_bitrate_datacount * 8 / d);
                m_stat_bitrate_datacount = 0;
                m_stat_bitrate_last_tm = m_invalid_begin_tm;
            }
        }
        int len = m_data_len - m_data_pos;
        int remainer = len;
        while (remainer >= TS_UDP_LEN)
        {
            if (m_srt == nullptr)
                return SLS_ERROR;
            int ret = write(m_data + m_data_pos, TS_UDP_LEN);
            if (ret < TS_UDP_LEN)
            {
                if (ret < 0)
                {
                    int err_no = CSLSSrt::libsrt_lasterror();
                    if (err_no == SRT_EASYNCSND)
                    {
                        m_send_backpressure_count.fetch_add(1, std::memory_order_relaxed);
                        m_map_data->report_viewer_backpressure(m_map_data_key);
                        // Only zero progress for 3x latency (floor 500ms) kicks.
                        // Leave per-packet late drops to SRT's TLPKTDROP.
                        int64_t stuck_timeout_ms = backpressure_stuck_timeout_ms();
                        if (m_backpressure_stuck_since_ms == 0)
                            m_backpressure_stuck_since_ms = sls_gettime_ms();
                        else if (sls_gettime_ms() - m_backpressure_stuck_since_ms > stuck_timeout_ms)
                        {
                            spdlog::warn("[{}] CSLSRole::handler_write_data, viewer stuck in backpressure {} ms "
                                         "(>{}ms, latency={}ms), disconnecting. backpressureEvents={}.",
                                         fmt::ptr(this), sls_gettime_ms() - m_backpressure_stuck_since_ms,
                                         stuck_timeout_ms, m_latency,
                                         m_send_backpressure_count.load(std::memory_order_relaxed));
                            return SLS_ERROR;
                        }
                        SPDLOG_TRACE("[{}] CSLSRole::handler_write_data, backpressure, pos={:d}, remaining={:d}.",
                                     fmt::ptr(this), m_data_pos, remainer);
                        return write_size;
                    }
                    if (err_no == SRT_ECONNLOST || err_no == SRT_ENOCONN)
                        spdlog::info("[{}] CSLSRole::handler_write_data, peer gone (errno={:d}), closing connection.",
                                     fmt::ptr(this), err_no);
                    else
                        spdlog::error(
                            "[{}] CSLSRole::handler_write_data, write data failed, len={:d}, ret={:d}, errno={:d}.",
                            fmt::ptr(this), len, ret, err_no);
                    return SLS_ERROR;
                }
                // SRT messages are all-or-nothing; preserve the cursor if an
                // unexpected short write occurs, as with transient backpressure.
                spdlog::error("[{}] CSLSRole::handler_write_data, short write, len={:d}, ret={:d}, not {:d}.",
                              fmt::ptr(this), len, ret, TS_UDP_LEN);
                break;
            }
            m_data_pos += TS_UDP_LEN;
            write_size += TS_UDP_LEN;
            m_backpressure_stuck_since_ms = 0;
            remainer = m_data_len - m_data_pos;
        }
        if (m_data_pos > m_data_len)
            spdlog::error("[{}] CSLSRole::handler_write_data, data error, m_data_pos={:d} > m_data_len={:d}.",
                          fmt::ptr(this), m_data_pos, m_data_len);
        if (m_data_pos < m_data_len)
            return write_size;
        m_data_pos = m_data_len = 0;
    }
    return write_size;
}

void CSLSRole::set_stat_info_base(stat_info_t &v)
{
    m_stat_info_base = v;
}

stat_info_t CSLSRole::get_stat_info()
{
    m_stat_info_base.kbitrate = m_kbitrate.load(std::memory_order_relaxed);
    return m_stat_info_base;
}

int CSLSRole::get_peer_info(char *peer_name, int &peer_port)
{
    std::lock_guard<std::mutex> socket_lock(m_srt_lifetime_mutex);
    if (m_srt)
        return m_srt->libsrt_getpeeraddr(peer_name, peer_port);
    return SLS_ERROR;
}

void CSLSRole::set_http_url(const char *http_url)
{
    if (http_url == nullptr || strlen(http_url) == 0)
        return;
    strlcpy(m_http_url, http_url, sizeof(m_http_url));
    m_http_passed.store(false, std::memory_order_release);
    m_http_auth_deadline_ms.store(0, std::memory_order_release);
}

void CSLSRole::set_auth_reject_cache(std::shared_ptr<AuthRejectCache> cache)
{
    m_auth_reject_cache = std::move(cache);
}

int CSLSRole::on_connect()
{
    if (strlen(m_http_url) == 0)
        return SLS_OK;
    char on_event_url[URL_MAX_LEN] = {0};
    if (strlen(m_peer_ip) == 0)
        get_peer_info(m_peer_ip, m_peer_port);
    int ret = snprintf(on_event_url, sizeof(on_event_url),
                       "%s?on_event=on_connect&role_name=%s&srt_url=%s&remote_ip=%s&remote_port=%d", m_http_url,
                       url_encode(m_role_name).c_str(), url_encode(get_streamid()).c_str(), m_peer_ip, m_peer_port);
    if (ret < 0 || (unsigned)ret >= sizeof(on_event_url))
    {
        spdlog::error("[{}] CSLSRole::on_connect, on_event_url is too long, ret={:d}.", fmt::ptr(this), ret);
        return SLS_ERROR;
    }
    try
    {
        auto future =
            AsyncHttpClient::instance().post_async(on_event_url, "", "application/json", kHttpRequestTimeoutSeconds);
        m_http_future = std::make_shared<std::shared_future<AsyncHttpResponse>>(std::move(future));
        m_http_auth_deadline_ms.store(sls_gettime_ms() + kHttpAuthorizationDeadlineMs, std::memory_order_release);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[{}] CSLSRole::on_connect, failed to dispatch authorization request: {}.", fmt::ptr(this),
                      e.what());
        return SLS_ERROR;
    }
    return SLS_OK;
}

int CSLSRole::on_close()
{
    if (!m_http_passed.load(std::memory_order_acquire) || strlen(m_http_url) == 0)
        return SLS_OK;
    char on_event_url[URL_MAX_LEN] = {0};
    // VirtualCall: live uninit() already runs derived teardown before release;
    // the base-destructor invalid_srt() path has a null socket and skips this.
    if (strlen(m_peer_ip) == 0)
        get_peer_info(m_peer_ip, m_peer_port); // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
    int ret = snprintf(on_event_url, sizeof(on_event_url),
                       "%s?on_event=on_close&role_name=%s&srt_url=%s&remote_ip=%s&remote_port=%d", m_http_url,
                       url_encode(m_role_name).c_str(), url_encode(get_streamid()).c_str(), m_peer_ip, m_peer_port);
    if (ret < 0 || (unsigned)ret >= sizeof(on_event_url))
    {
        spdlog::error("[SLSRole::on_close] callback URL too long [len={:d}]", ret);
        return SLS_ERROR;
    }
    auto future = AsyncHttpClient::instance().post_async(on_event_url, "", "application/json", 5);
    m_http_future = std::make_shared<std::shared_future<AsyncHttpResponse>>(std::move(future));
    return SLS_OK;
}

int CSLSRole::check_http_passed()
{
    if (m_http_passed.load(std::memory_order_acquire))
        return SLS_OK;
    int64_t deadline_ms = m_http_auth_deadline_ms.load(std::memory_order_acquire);
    if (deadline_ms > 0 && sls_gettime_ms() >= deadline_ms)
    {
        spdlog::error("[{}] CSLSRole::check_http_passed, publisher authorization deadline expired.", fmt::ptr(this));
        m_http_future = nullptr;
        m_http_auth_deadline_ms.store(0, std::memory_order_release);
        mark_invalid();
        return SLS_ERROR;
    }
    if (!m_http_future)
    {
        spdlog::error("[{}] CSLSRole::check_http_passed, authorization pending without a request.", fmt::ptr(this));
        m_http_auth_deadline_ms.store(0, std::memory_order_release);
        mark_invalid();
        return SLS_ERROR;
    }
    using namespace std::chrono_literals;
    if (m_http_future->wait_for(0ms) != std::future_status::ready)
        return SLS_ERROR;
    auto response = m_http_future->get();
    m_http_future = nullptr;
    m_http_auth_deadline_ms.store(0, std::memory_order_release);
    if (!response.success || response.status_code != 200)
    {
        spdlog::error("[{}] CSLSRole::check_http_client_response, http refused, invalid {}, status={}.", fmt::ptr(this),
                      m_role_name, response.status_code);
        // Cache only explicit 401/403 responses, never transport or backend
        // failures. Peer scope prevents one source poisoning another's key.
        if (m_auth_reject_cache && response.success && (response.status_code == 401 || response.status_code == 403))
        {
            char peer_ip[IP_MAX_LEN] = {0};
            int peer_port = 0;
            get_peer_info(peer_ip, peer_port);
            m_auth_reject_cache->record_failure(sls_reject_cache_key(peer_ip, get_streamid()));
        }
        mark_invalid();
        return SLS_ERROR;
    }
    spdlog::info("[{}] CSLSRole::check_http_client_response, http finished, {}, status={}.", fmt::ptr(this),
                 m_role_name, response.status_code);
    // Start first-media probation only after the bounded authorization gate
    // permits reads; time spent waiting for the webhook is not encoder silence.
    m_invalid_begin_tm = sls_gettime_ms();
    m_http_passed.store(true, std::memory_order_release);
    if (!response.body.empty() && response.body[0] == '{')
    {
        sls_conf_app_t *app_conf = static_cast<sls_conf_app_t *>(m_conf);
        if (app_conf == nullptr || app_conf->push_destination_max <= 0)
            return SLS_OK;
        try
        {
            auto parsed = nlohmann::json::parse(response.body);
            if (!parsed.contains("pushTargets") || !parsed["pushTargets"].is_array())
                return SLS_OK;
            const auto &self_addrs = push_url_self_addresses();
            int kept = 0;
            for (const auto &entry : parsed["pushTargets"])
            {
                if (kept >= app_conf->push_destination_max)
                {
                    spdlog::warn("[relay] push destination rejected | reason=over_limit");
                    continue;
                }
                if (!entry.is_object() || !entry.contains("url") || !entry["url"].is_string())
                    continue;
                std::string url = entry["url"].get<std::string>();
                sockaddr_storage vetted_addr{};
                PushUrlReject verdict = validate_push_url(url, *app_conf, self_addrs, &vetted_addr);
                if (verdict != PushUrlReject::Ok)
                {
                    spdlog::warn("[relay] push destination rejected | reason={}", push_url_reject_reason(verdict));
                    continue;
                }
                m_push_urls.push_back(std::move(url));
                m_push_vetted_addrs.push_back(vetted_addr);
                ++kept;
            }
            if (kept > 0)
                spdlog::info("[relay] push destinations accepted for {} | count={} streamid='{}'", m_role_name, kept,
                             sls_redact_secret(get_streamid()));
        }
        catch (const std::exception &)
        {
            // JSON exception text may include webhook credentials or push URLs.
            spdlog::warn("[{}] CSLSRole::check_http_passed, invalid push-target JSON response", fmt::ptr(this));
        }
    }
    return SLS_OK;
}

void CSLSRole::on_map_data_set() {}

bool CSLSRole::is_audio_gap_fill_enabled() const
{
    return false;
}

bool CSLSRole::get_audio_gap_stats(CSLSMapData::AudioGapStreamStats &stats, int clear) const
{
    stats = CSLSMapData::AudioGapStreamStats();
    stats.enabled = is_audio_gap_fill_enabled();
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0)
        return false;
    bool found = m_map_data->get_audio_gap_stats(m_map_data_key, stats, clear);
    stats.enabled = is_audio_gap_fill_enabled();
    return found;
}

bool CSLSRole::get_timecode_stats(CSLSMapData::TimecodeStats &stats, int clear) const
{
    stats = CSLSMapData::TimecodeStats();
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0)
        return false;
    return m_map_data->get_timecode_stats(m_map_data_key, stats, clear);
}

int64_t CSLSRole::get_ring_overrun_count() const
{
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0)
        return -1;
    return m_map_data->get_overrun_count(m_map_data_key);
}

int64_t CSLSRole::get_max_reader_backlog(bool clear) const
{
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0)
        return -1;
    return m_map_data->get_max_reader_backlog(m_map_data_key, clear);
}

int64_t CSLSRole::get_viewer_backpressure_events(bool clear) const
{
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0)
        return -1;
    return m_map_data->get_viewer_backpressure_events(m_map_data_key, clear);
}

int64_t CSLSRole::get_viewer_snd_drops(bool clear) const
{
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0)
        return -1;
    return m_map_data->get_viewer_snd_drops(m_map_data_key, clear);
}

int64_t CSLSRole::get_ingest_discontinuities(bool clear) const
{
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0)
        return -1;
    return m_map_data->get_ingest_discontinuities(m_map_data_key, clear);
}

void CSLSRole::sample_viewer_snd_drops()
{
    if (m_map_data == nullptr || strlen(m_map_data_key) == 0 || m_srt == nullptr)
        return;
    int64_t now_ms = sls_gettime_ms();
    if (now_ms - m_last_snd_drop_sample_ms < 1000)
        return;
    m_last_snd_drop_sample_ms = now_ms;
    // Read monotonic totals without clearing other socket interval counters.
    SRT_TRACEBSTATS stats{};
    if (get_statistics(&stats, 0) != SLS_OK)
        return;
    if (m_player_snapshot)
    {
        m_player_snapshot->rtt_ms.store(stats.msRTT, std::memory_order_relaxed);
        m_player_snapshot->mbps_send_rate.store(stats.mbpsSendRate, std::memory_order_relaxed);
        m_player_snapshot->pkt_snd_drop_total.store(stats.pktSndDropTotal, std::memory_order_relaxed);
        m_player_snapshot->pkt_retrans_total.store(stats.pktRetransTotal, std::memory_order_relaxed);
    }
    int64_t delta = stats.pktSndDropTotal - m_snd_drops_reported;
    if (delta > 0)
    {
        m_map_data->report_viewer_snd_drops(m_map_data_key, delta);
        m_snd_drops_reported = stats.pktSndDropTotal;
    }
}

int CSLSRole::init_bitrate_limiter(int max_bitrate_kbps, int violation_timeout_seconds, float spike_tolerance)
{
    cleanup_bitrate_limiter();
    if (max_bitrate_kbps <= 0)
    {
        spdlog::info("[{}] CSLSRole::init_bitrate_limiter, bitrate limiting disabled (max_bitrate_kbps={:d})",
                     fmt::ptr(this), max_bitrate_kbps);
        return SLS_OK;
    }
    m_bitrate_limiter = new CSLSBitrateLimit();
    int ret = m_bitrate_limiter->init(max_bitrate_kbps, violation_timeout_seconds, 5000, spike_tolerance);
    if (ret != SLS_OK)
    {
        spdlog::error("[{}] CSLSRole::init_bitrate_limiter, failed to initialize bitrate limiter", fmt::ptr(this));
        delete m_bitrate_limiter;
        m_bitrate_limiter = nullptr;
        return ret;
    }
    spdlog::info("[{}] CSLSRole::init_bitrate_limiter, initialized with max_bitrate={:d}kbps, "
                 "violation_timeout={:d}s, spike_tolerance={:.2f}",
                 fmt::ptr(this), max_bitrate_kbps, violation_timeout_seconds, spike_tolerance);
    return SLS_OK;
}

void CSLSRole::cleanup_bitrate_limiter()
{
    if (m_bitrate_limiter)
    {
        delete m_bitrate_limiter;
        m_bitrate_limiter = nullptr;
    }
}

CSLSBitrateLimit::BitrateStats CSLSRole::get_bitrate_stats() const
{
    if (m_bitrate_limiter)
        return m_bitrate_limiter->get_stats();
    CSLSBitrateLimit::BitrateStats empty_stats = {};
    return empty_stats;
}
