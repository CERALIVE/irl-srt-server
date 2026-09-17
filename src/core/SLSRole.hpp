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

#include <map>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <future>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include "SLSSrt.hpp"
#include "SLSMapData.hpp"
#include "SLSPlayerRegistry.hpp"
#include "conf.hpp"
#include "SLSLock.hpp"
#include "common.hpp"
#include "AsyncHttpClient.hpp"
#include "SLSBitrateLimit.hpp"

class AuthRejectCache;

enum SLS_ROLE_STATE
{
    SLS_RS_UNINIT = 0,
    SLS_RS_INITED = 1,
    SLS_RS_INVALID = 2,
};

// Per-role egress staging buffer, in bytes. Each handler_write_data
// call drains up to this much from the publisher ring and then issues
// one srt_sendmsg per TS_UDP_LEN chunk in a tight loop. 100 packets
// (~131 KB) was historical: it produced fairness problems (one slow
// viewer monopolised the worker for 100 sendmsg calls before yielding)
// and made a single epoll wake routinely overshoot the SRT send-buffer
// budget of a low-latency viewer, triggering EASYNCSND. 16 packets
// (~21 KB) is roughly the per-cycle drain budget at common bitrates
// and lets epoll re-arm sooner across roles. Net memory saved on a
// 100-viewer node is ~11 MB (was ~13 MB of static role buffer space).
const int DATA_BUFF_SIZE = 16 * 1316;
const int UNLIMITED_TIMEOUT = -1;

// Max publisher-ring batches drained per handler_write_data() call.
// Egress is driven by the worker's periodic pass (players are no longer
// permanently armed for SRT_EPOLL_OUT), so a small inner drain loop keeps
// throughput from being capped at one DATA_BUFF_SIZE per worker wakeup at
// high bitrate. Deliberately small (2 * ~21 KB = ~42 KB per call, ~2x
// realtime headroom at 20 Mbps): a LIVE viewer must not "catch up" by
// bursting a large backlog out — that is exactly what a viewer perceives as
// a replay/rewind. When a viewer falls behind, we want it to stay behind and
// let the SRT socket's TLPKTDROP skip it forward per-packet, not to fire a
// ~20x burst of stale frames (the old value of 8 = ~168 KB/call did this).
// If high-bitrate viewers under-drain (ring backlog grows in /stats without
// a slow-link cause), nudge this up; do not raise it to "help" a slow viewer.
const int MAX_EGRESS_BATCHES = 2;

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class CSLSRole
{
public:
    CSLSRole();
    virtual ~CSLSRole();

    virtual int init();
    virtual int uninit();
    virtual int handler();
    // Periodic hook run by the worker on every role in its map once per loop
    // pass (~POLLING_TIME), independent of socket events. Default no-op;
    // CSLSListener overrides it to advance work that must progress without a
    // new connection event (draining async player-key validations and
    // completing deferred player accepts). Runs on the owning worker thread.
    virtual void on_worker_tick() {}

    int open(char *url);
    int close();
    int get_fd();
    int set_eid(int eid);
    bool is_write()
    {
        return m_is_write;
    }
    int set_srt(CSLSSrt *srt);
    int invalid_srt();
    // Worker-only: mark dead as well as closing, so no handler is driven while
    // the role waits for the periodic map reap. Cross-thread callers use kick.
    void mark_invalid();
    bool is_invalid() const
    {
        return SLS_RS_INVALID == m_state.load(std::memory_order_acquire);
    }
    int write(const char *buf, int size);
    int add_to_epoll(int eid);
    int remove_from_epoll();
    // Arm OUT only for staged data that could not be sent; idle sockets must
    // not busy-return. Called on the owning worker after every egress attempt.
    void update_egress_arming();
    int get_state(int64_t cur_time_ms = 0);
    int get_sock_state();
    char *get_role_name();
    int64_t get_stat_start_time()
    {
        return m_stat_start_time;
    }
    // Cross-thread teardown requests only flip the atomic; the worker closes.
    void request_kick();
    void set_conf(sls_conf_base_t *conf);
    void set_map_data(const char *map_key, CSLSMapData *map_data);
    void set_idle_streams_timeout(int timeout);
    // Publisher-only no-media probation in ms; zero disables it.
    void set_first_data_timeout(int timeout_ms);
    bool check_idle_streams_duration(int64_t cur_time_ms = 0);
    bool has_recent_recv_data(int64_t now_ms, int64_t within_ms) const;
    // Only external publishers override this; pullers remain evictable.
    virtual bool is_takeover_protected() const
    {
        return false;
    }
    char *get_streamid();
    void set_streamid(const char *sid);
    bool is_reconnect();
    char *get_map_data_key();
    void set_stat_info_base(stat_info_t &v);
    virtual stat_info_t get_stat_info();
    void update_stat_info();
    virtual int get_peer_info(char *peer_name, int &peer_port);
    void set_http_url(const char *http_url);
    void set_auth_reject_cache(std::shared_ptr<AuthRejectCache> cache);
    int on_connect();
    int on_close();
    const std::vector<std::string> &get_push_urls() const
    {
        return m_push_urls;
    }
    int get_statistics(SRT_TRACEBSTATS *currentStats, int clear);
    int get_bitrate();
    int get_uptime();
    int get_latency()
    {
        return m_latency;
    }
    void set_latency(int latency)
    {
        m_latency = latency;
    }
    bool get_audio_gap_stats(CSLSMapData::AudioGapStreamStats &stats, int clear = 0) const;
    int64_t get_ring_overrun_count() const;
    int64_t get_max_reader_backlog(bool clear = false) const;
    int64_t get_viewer_backpressure_events(bool clear = false) const;
    int64_t get_viewer_snd_drops(bool clear = false) const;
    int64_t get_ingest_discontinuities(bool clear = false) const;
    bool get_timecode_stats(CSLSMapData::TimecodeStats &stats, int clear = 0) const;
    // Player-side aggregation onto the publisher ring, at most once a second.
    void sample_viewer_snd_drops();

    // Player-side: the snapshot this player's entry in CSLSPlayerRegistry
    // reads. sample_viewer_snd_drops() refreshes it, and invalid_srt() marks
    // it closed. Set once by the listener before the role reaches a worker.
    void set_player_snapshot(std::shared_ptr<PlayerStatsSnapshot> snapshot)
    {
        m_player_snapshot = std::move(snapshot);
    }

    // Count of times handler_write_data() hit SRT send-buffer
    // backpressure (errno EASYNCSND) on this role. Each event means a
    // viewer egress write was deferred to the next epoll cycle rather
    // than killing the connection. Surfaced via /stats so operators can
    // see when viewers are falling behind.
    uint64_t get_send_backpressure_count() const
    {
        return m_send_backpressure_count.load(std::memory_order_relaxed);
    }
    int check_http_client();
    int check_http_passed();
    int init_bitrate_limiter(int max_bitrate_kbps, int violation_timeout_seconds = 30, float spike_tolerance = 2.0f);
    void cleanup_bitrate_limiter();
    CSLSBitrateLimit::BitrateStats get_bitrate_stats() const;
    // Retain the public local hook: audio gap filling and timecode setup both
    // run here, at binding and again after the authorized lazy ring add.
    virtual void on_map_data_set();
    virtual bool is_audio_gap_fill_enabled() const;

protected:
    CSLSSrt *m_srt;
    bool m_is_write;
    int64_t m_stat_start_time;
    int64_t m_invalid_begin_tm;
    int64_t m_stat_bitrate_last_tm;
    int m_stat_bitrate_interval;
    int64_t m_stat_bitrate_datacount;
    std::atomic<int> m_kbitrate{0};
    int m_idle_streams_timeout;
    int m_first_data_timeout_ms{0};
    // Written by the owning worker, read by the listener at publisher takeover.
    // Relaxed snapshots suffice: no socket or media state is published here.
    std::atomic<int64_t> m_last_recv_data_tm{0};
    int m_latency;
    std::atomic<int> m_state{SLS_RS_UNINIT};
    std::mutex m_srt_lifetime_mutex;
    std::atomic<bool> m_kick_requested{false};
#ifndef NDEBUG
    // Single-owner teardown tripwire (Todo 19 — verify, don't blanket-lock).
    // Roles are owned by a std::shared_ptr<CSLSRole> (see CSLSRoleList) and their
    // SRT socket is torn down by exactly one thread at a time: the owning worker
    // serialises handler()/get_state()/invalid_srt() in its single-threaded loop;
    // a cross-thread kick only flips m_kick_requested (release/acquire) and the
    // owner performs the actual invalid_srt() in get_state(); and at shutdown the
    // worker threads are joined before the main thread drains the rest, so
    // ownership transfers but the teardown never overlaps. This atomic records
    // which thread (if any) is currently inside invalid_srt()'s delete path for
    // this role; a second thread entering concurrently fails the assert there —
    // the exact double-free of m_srt a broken single-owner model would cause.
    // The complementary read/write-vs-delete race is covered by the task-19 TSan
    // storm. Wholly compiled out in release (NDEBUG) — zero production footprint.
    std::atomic<std::thread::id> m_invalidating_tid{std::thread::id{}};
#endif
    int m_back_log;
    int m_port;
    char m_peer_ip[IP_MAX_LEN];
    int m_peer_port;
    char m_role_name[STR_MAX_LEN];
    char m_streamid[URL_MAX_LEN];
    char m_http_url[URL_MAX_LEN];
    // Auth setup is on the listener worker, completion on the role's worker.
    std::atomic<bool> m_http_passed{true};
    sls_conf_base_t *m_conf;
    CSLSMapData *m_map_data;
    char m_map_data_key[URL_MAX_LEN];
    SLSRecycleArrayID m_map_data_id;
    // Lazy authorized ring allocation; also read by the accept-time media hook.
    std::atomic<bool> m_ring_added{false};
    char m_data[DATA_BUFF_SIZE];
    // Egress cursors are worker-confined, not accessed by the stats thread.
    int m_data_len;
    int m_data_pos;
    std::atomic<bool> m_need_reconnect{false};
    std::atomic<uint64_t> m_send_backpressure_count{0};
    // Worker-only bookkeeping for cumulative socket-drop deltas.
    int64_t m_snd_drops_reported{0};
    int64_t m_last_snd_drop_sample_ms{0};
    std::shared_ptr<PlayerStatsSnapshot> m_player_snapshot;


    // Wall-clock (sls_gettime_ms) of the first EASYNCSND-with-no-progress
    // event in the current stuck streak. Cleared back to 0 on any
    // successful write byte. handler_write_data uses this to break out
    // of a permanently-backpressured viewer (link too slow for the
    // stream) instead of holding their publisher-ring read position
    // open indefinitely.
    int64_t m_backpressure_stuck_since_ms{0};
    bool m_epoll_out_armed{false};
    int set_epoll_out(bool enable);
    static constexpr int64_t kBackpressureStuckFloorMs = 500;
    static constexpr int64_t kBackpressureStuckLatencyMultiplier = 3;
    int64_t backpressure_stuck_timeout_ms() const
    {
        int64_t scaled = (int64_t)m_latency * kBackpressureStuckLatencyMultiplier;
        return scaled > kBackpressureStuckFloorMs ? scaled : kBackpressureStuckFloorMs;
    }
    stat_info_t m_stat_info_base;
    std::shared_ptr<std::shared_future<AsyncHttpResponse>> m_http_future;
    std::atomic<int64_t> m_http_auth_deadline_ms{0};
    CSLSBitrateLimit *m_bitrate_limiter;
    std::vector<std::string> m_push_urls;
    // Index-aligned vetted IPs: pushers dial these without DNS re-resolution.
    std::vector<sockaddr_storage> m_push_vetted_addrs;
    std::shared_ptr<AuthRejectCache> m_auth_reject_cache;
    int handler_write_data();
    int handler_read_data(int64_t *last_read_time = nullptr);
};
