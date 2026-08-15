
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
#include <fcntl.h>
#include <string.h>
#include <string_view>
#include "spdlog/spdlog.h"

#include "SLSMapData.hpp"
#include "SLSLog.hpp"

/**
 * CSLSMapData class implementation
 */

CSLSMapData::CSLSMapData() {}
CSLSMapData::~CSLSMapData()
{
    clear();
}

int CSLSMapData::add(char *key, int max_bitrate_kbps, int latency_ms)
{
    int ret = SLS_OK;
    std::string strKey = std::string(key);

    CSLSLock lock(&m_rwclock, true);

    auto item = m_map_array.find(strKey);
    if (item != m_map_array.end())
    {
        CSLSRecycleArray *array_data = item->second;
        if (array_data)
        {
            spdlog::info("[{}] CSLSMapData::add, failed, key={}, array_data={}, exist.", fmt::ptr(this), key,
                         fmt::ptr(array_data));
            // Idempotent re-add (e.g. the puller's connect-time add hitting an
            // already-allocated ring). Return OK WITHOUT touching the budget
            // counters — the ring was accounted for at its original allocation.
            return ret;
        }
        // m_map_array.erase(item);
    }

    // Global stream-count cap. Checked before any allocation so a flood of
    // authorized publishers is rejected cheaply (no transient ring alloc).
    // 0 == unlimited.
    if (m_max_streams > 0 && m_stream_count.load(std::memory_order_relaxed) >= m_max_streams)
    {
        spdlog::warn("[{}] CSLSMapData::add, refused key='{}': stream-count cap reached"
                     " ({:d}/{:d}).",
                     fmt::ptr(this), key, m_stream_count.load(std::memory_order_relaxed), m_max_streams);
        return SLS_ERROR;
    }

    CSLSRecycleArray *data_array = new CSLSRecycleArray;

    // Decide the ring size up front (without resizing yet) so the memory cap
    // can be enforced before we commit the (potentially large) allocation.
    // The CSLSRecycleArray default (get_data_size()) is the floor; a bitrate +
    // latency hint grows it so a subscriber falling up to one SRT latency window
    // behind is still safe from overruns. Sized at 1x the latency window (was
    // 2x): the ring is a hand-off buffer, NOT the jitter buffer — the viewer's
    // own SRT socket holds its latency window and drops late packets via
    // TLPKTDROP. Oversizing the ring only lets a slow viewer bank multiple
    // seconds of stale data that then replays as a rewind. One latency window is
    // enough to ride out publisher-side jitter without overrun; anything beyond
    // it should be skipped forward at the socket, not stored. Skip the growth if
    // the caller passed no hint — the default is enough for typical bitrates.
    // Watch /stats ringOverruns after changing this: 0 means the ring is still
    // large enough; sustained growth means it was cut too small for the bitrate.
    int64_t intended_size = (int64_t)data_array->get_data_size();
    if (max_bitrate_kbps > 0 && latency_ms > 0)
    {
        // bytes_per_sec is bounded by ~max_bitrate_kbps (config caps in
        // SLSPublisher.hpp limit it to 1_000_000 kbps = 125 MB/s).
        int64_t bytes_per_sec = (int64_t)max_bitrate_kbps * 1000 / 8;
        // 1x the latency window in seconds, clamped to a minimum of 1s.
        int64_t window_secs = (int64_t)latency_ms / 1000;
        if (window_secs < 1)
            window_secs = 1;
        int64_t target_bytes = bytes_per_sec * window_secs;
        // Hard per-stream cap, lowered from 256 MB to 32 MB to bound the
        // worst-case memory a single (authorized) publisher can pin. At the
        // 2048 MB global default this leaves room for ~64 max-size rings, or
        // 256 default (8 MB) rings — matching the max_streams default. An
        // operator opts a high-bitrate stream into a bigger ring purely via
        // its max_input_bitrate_kbps / latency; we'd rather take a recoverable
        // overrun warning than commit unbounded memory.
        const int64_t MAX_RING_BYTES = 32LL * 1024 * 1024;
        if (target_bytes > MAX_RING_BYTES)
            target_bytes = MAX_RING_BYTES;
        if (target_bytes > intended_size)
            intended_size = target_bytes;
    }

    // Global ring-memory cap. Enforced before setSize() so the over-budget
    // path frees only the small default allocation, never the grown ring.
    // 0 == unlimited.
    if (m_max_total_ring_bytes > 0 &&
        m_total_ring_bytes.load(std::memory_order_relaxed) + intended_size > m_max_total_ring_bytes)
    {
        spdlog::warn("[{}] CSLSMapData::add, refused key='{}': total ring-memory cap"
                     " would be exceeded ({:d} + {:d} > {:d} bytes).",
                     fmt::ptr(this), key, m_total_ring_bytes.load(std::memory_order_relaxed), intended_size,
                     m_max_total_ring_bytes);
        delete data_array;
        return SLS_ERROR;
    }

    if (intended_size > (int64_t)data_array->get_data_size())
    {
        data_array->setSize((int)intended_size);
        spdlog::info("[{}] CSLSMapData::add, sized ring for key='{}' to {:d} bytes"
                     " ({:d} kbps * max({:d} ms, 1s) window).",
                     fmt::ptr(this), key, (int)intended_size, max_bitrate_kbps, latency_ms);
    }

    m_map_array[strKey] = data_array;

    // Commit the budget exactly at the allocation point, accounting the ring's
    // real size. remove()/clear() decrement the same way when they free it, so
    // the counters stay alloc-balanced across idempotent re-adds.
    m_stream_count.fetch_add(1, std::memory_order_relaxed);
    m_total_ring_bytes.fetch_add((int64_t)data_array->get_data_size(), std::memory_order_relaxed);

    // Pre-allocate the per-stream parser state so put() never mutates the map
    // structure on the hot path. Previously put() would lazy-create the
    // entry under a write lock; we want put() to take only a read lock
    // (so puts to different keys, and stats reads, can run concurrently).
    if (m_map_cc_state.find(strKey) == m_map_cc_state.end())
    {
        ts_cc_state *cc = new ts_cc_state;
        sls_init_ts_cc_state(cc);
        m_map_cc_state[strKey] = cc;
    }

    spdlog::info("[{}] CSLSMapData::add ok, key='{}', ring_size={:d}, streams={:d}, total_ring_bytes={:d}.",
                 fmt::ptr(this), key, data_array->get_data_size(), m_stream_count.load(std::memory_order_relaxed),
                 m_total_ring_bytes.load(std::memory_order_relaxed));
    return ret;
}

void CSLSMapData::set_caps(int max_streams, int64_t max_total_ring_bytes)
{
    m_max_streams = max_streams;
    m_max_total_ring_bytes = max_total_ring_bytes;
    spdlog::info("[{}] CSLSMapData::set_caps, max_streams={:d}, max_total_ring_bytes={:d}.", fmt::ptr(this),
                 max_streams, max_total_ring_bytes);
}

int64_t CSLSMapData::get_overrun_count(const char *key)
{
    if (!key)
        return -1;
    CSLSLock lock(&m_rwclock, false);
    auto it = m_map_array.find(std::string_view{key});
    if (it == m_map_array.end() || it->second == NULL)
        return -1;
    return it->second->get_overrun_count();
}

int64_t CSLSMapData::get_max_reader_backlog(const char *key, bool clear)
{
    if (!key)
        return -1;
    CSLSLock lock(&m_rwclock, false);
    auto it = m_map_array.find(std::string_view{key});
    if (it == m_map_array.end() || it->second == NULL)
        return -1;
    return it->second->get_max_reader_backlog(clear);
}

void CSLSMapData::report_viewer_backpressure(const char *key)
{
    if (!key)
        return;
    CSLSLock lock(&m_rwclock, false);
    auto it = m_map_array.find(std::string_view{key});
    if (it == m_map_array.end() || it->second == NULL)
        return;
    it->second->report_viewer_backpressure();
}

int64_t CSLSMapData::get_viewer_backpressure_events(const char *key, bool clear)
{
    if (!key)
        return -1;
    CSLSLock lock(&m_rwclock, false);
    auto it = m_map_array.find(std::string_view{key});
    if (it == m_map_array.end() || it->second == NULL)
        return -1;
    return it->second->get_viewer_backpressure_events(clear);
}

void CSLSMapData::report_viewer_snd_drops(const char *key, int64_t count)
{
    if (!key || count <= 0)
        return;
    CSLSLock lock(&m_rwclock, false);
    auto it = m_map_array.find(std::string_view{key});
    if (it == m_map_array.end() || it->second == NULL)
        return;
    it->second->report_viewer_snd_drops(count);
}

int64_t CSLSMapData::get_viewer_snd_drops(const char *key, bool clear)
{
    if (!key)
        return -1;
    CSLSLock lock(&m_rwclock, false);
    auto it = m_map_array.find(std::string_view{key});
    if (it == m_map_array.end() || it->second == NULL)
        return -1;
    return it->second->get_viewer_snd_drops(clear);
}

int64_t CSLSMapData::get_ingest_discontinuities(const char *key, bool clear)
{
    if (!key)
        return -1;
    CSLSLock lock(&m_rwclock, false);
    auto it = m_map_array.find(std::string_view{key});
    if (it == m_map_array.end() || it->second == NULL)
        return -1;
    return it->second->get_ingest_discontinuities(clear);
}

int CSLSMapData::remove(char *key)
{
    int ret = SLS_ERROR;
    std::string strKey = std::string(key);

    CSLSLock lock(&m_rwclock, true);

    auto item_cc = m_map_cc_state.find(strKey);
    if (item_cc != m_map_cc_state.end())
    {
        delete item_cc->second;
        m_map_cc_state.erase(item_cc);
    }

    auto item_tc = m_map_tc_state.find(strKey);
    if (item_tc != m_map_tc_state.end())
    {
        delete item_tc->second;
        m_map_tc_state.erase(item_tc);
    }

    auto item = m_map_array.find(strKey);
    if (item != m_map_array.end())
    {
        CSLSRecycleArray *array_data = item->second;
        spdlog::info("[{}] CSLSMapData::remove, key='{}' delete array_data={}.", fmt::ptr(this), key,
                     fmt::ptr(array_data));
        if (array_data)
        {
            m_stream_count.fetch_sub(1, std::memory_order_relaxed);
            m_total_ring_bytes.fetch_sub((int64_t)array_data->get_data_size(), std::memory_order_relaxed);
            delete array_data;
        }
        m_map_array.erase(item);
        return SLS_OK;
    }
    return ret;
}

bool CSLSMapData::is_exist(char *key)
{

    CSLSLock lock(&m_rwclock, false);

    auto item = m_map_array.find(std::string_view{key});
    if (item != m_map_array.end())
    {
        CSLSRecycleArray *array_data = item->second;
        if (array_data)
        {
            spdlog::trace("[{}] CSLSMapData::is_exist, key={}, exist.", fmt::ptr(this), key);
            return true;
        }
        else
        {
            spdlog::trace("[{}] CSLSMapData::is_exist, is_exist, key={}, data_array is null.", fmt::ptr(this), key);
        }
    }
    else
    {
        spdlog::trace("[{}] CSLSMapData::add, is_exist, key={}, not exist.", fmt::ptr(this), key);
    }
    return false;
}

int CSLSMapData::put(char *key, char *data, int len, int64_t *last_read_time)
{
    int ret = SLS_OK;

    // READ lock on the map structure. The map itself is only mutated by
    // add()/remove() (which take WRITE lock); put() only reads
    // m_map_array and m_map_cc_state to find pre-allocated entries.
    // Per-entry mutation (parser state, CSLSRecycleArray buffer) is
    // synchronised by each entry's own lock — see CSLSRecycleArray::put
    // — or, for the parser state, by the per-publisher single-writer
    // invariant (only one publisher role writes a given key). Concurrent
    // puts to different keys can therefore proceed in parallel, and /stats
    // reads (also read-lock) no longer serialise with the data path.
    CSLSLock lock(&m_rwclock, false);
    std::string_view keyView{key};

    auto item = m_map_array.find(keyView);
    if (item == m_map_array.end())
    {
        spdlog::error("[{}] CSLSMapData::put, key={}, not found data array.", fmt::ptr(this), key);
        return SLS_ERROR;
    }
    CSLSRecycleArray *array_data = item->second;
    if (NULL == array_data)
    {
        spdlog::error("[{}] CSLSMapData::get, key={}, array_data is NULL.", fmt::ptr(this), key);
    }

    if (NULL != last_read_time)
    {
        *last_read_time = array_data->get_last_read_time();
    }

    // Ingest continuity accounting: a break here means TLPKTDROP discarded
    // content upstream of the server, so every viewer of this stream shares
    // the resulting glitch. Counter only — see note_ingest_discontinuity for
    // why delivery is not gated on it.
    auto item_cc = m_map_cc_state.find(keyView);
    if (item_cc != m_map_cc_state.end() && item_cc->second != NULL)
    {
        int breaks = sls_ts_check_continuity((const uint8_t *)data, len, item_cc->second);
        if (breaks > 0)
            array_data->note_ingest_discontinuity();
    }

    // In-band SMPTE timecode, for streams that opted in. Absent entry == not
    // scanning, which is the default and costs one map lookup.
    auto item_tc = m_map_tc_state.find(keyView);
    if (item_tc != m_map_tc_state.end() && item_tc->second != NULL)
    {
        sls_ts_scan_timecode((const uint8_t *)data, len, item_tc->second);
    }

    ret = array_data->put(data, len);
    if (ret != len)
    {
        spdlog::error("[{}] CSLSMapData::put, key={}, array_data->put failed, len={:d}, but ret={:d}.", fmt::ptr(this),
                      key, len, ret);
    }

    return ret;
}

int CSLSMapData::get(char *key, char *data, int len, SLSRecycleArrayID *read_id, int aligned)
{
    int ret = SLS_OK;

    CSLSLock lock(&m_rwclock, false);

    auto item = m_map_array.find(std::string_view{key});
    if (item == m_map_array.end())
    {
        spdlog::trace("[{}] CSLSMapData::get, key={}, not found data array,", fmt::ptr(this), key);
        return SLS_ERROR;
    }
    CSLSRecycleArray *array_data = item->second;
    if (NULL == array_data)
    {
        spdlog::warn("[{}] CSLSMapData::get, key={}, array_data is NULL.", fmt::ptr(this), key);
        return SLS_ERROR;
    }

    // Historically the first get() for a reader injected a synthesized
    // PAT/PMT/SPS-PPS packet, captured ONCE at session start, so a joining
    // decoder could initialise before the stream repeated its tables. On long sessions that packet is minutes-to-hours stale — its
    // capture-time timestamp regresses the demuxer clock, and with adaptive
    // encoders (moblin changes parameters mid-session) its SPS can describe
    // a different resolution than the live stream, flashing corrupt or stale
    // frames on every join and rejoin. Streams repeat their tables inline
    // (PAT/PMT every <=100ms, parameter sets at each keyframe), so the safe
    // behavior is to relay ring bytes untouched and inject nothing.
    return array_data->get(data, len, read_id, aligned);
}

void CSLSMapData::clear()
{
    CSLSLock lock(&m_rwclock, true);
    for (auto it = m_map_array.begin(); it != m_map_array.end();)
    {
        CSLSRecycleArray *array_data = it->second;
        if (array_data)
        {
            m_stream_count.fetch_sub(1, std::memory_order_relaxed);
            m_total_ring_bytes.fetch_sub((int64_t)array_data->get_data_size(), std::memory_order_relaxed);
            delete array_data;
        }
        it++;
    }
    m_map_array.clear();
    for (auto item_cc = m_map_cc_state.begin(); item_cc != m_map_cc_state.end();)
    {
        delete item_cc->second;
        item_cc++;
    }
    m_map_cc_state.clear();
    for (auto item_tc = m_map_tc_state.begin(); item_tc != m_map_tc_state.end();)
    {
        delete item_tc->second;
        item_tc++;
    }
    m_map_tc_state.clear();
}

void CSLSMapData::set_timecode_scan(const char *key, bool enabled)
{
    if (key == NULL)
        return;

    CSLSLock lock(&m_rwclock, true);
    std::string strKey = std::string(key);
    auto item = m_map_tc_state.find(strKey);

    if (!enabled)
    {
        if (item != m_map_tc_state.end())
        {
            delete item->second;
            m_map_tc_state.erase(item);
        }
        return;
    }

    if (item != m_map_tc_state.end())
        return; // already scanning; keep what it has learned

    ts_timecode_state *tc = new ts_timecode_state;
    sls_init_ts_timecode_state(tc);
    m_map_tc_state[strKey] = tc;
    spdlog::info("[{}] CSLSMapData::set_timecode_scan, key='{}', in-band timecode scanning enabled.", fmt::ptr(this),
                 key);
}

bool CSLSMapData::get_timecode_stats(const char *key, TimecodeStats &stats, int clear)
{
    stats = TimecodeStats();

    if (key == NULL)
        return false;

    // WRITE lock, deliberately. put() holds only the read lock, so an exclusive
    // lock here is what keeps the publisher's writes to ts_timecode_state from
    // racing this read. /stats is polled at most a few times a second, so the
    // brief exclusion of the data path is not a throughput concern.
    CSLSLock lock(&m_rwclock, true);
    auto item = m_map_tc_state.find(std::string_view{key});
    if (item == m_map_tc_state.end() || item->second == NULL)
        return false;

    ts_timecode_state *tc = item->second;
    char buf[16] = {0};
    sls_format_timecode(tc, buf, sizeof(buf));

    stats.enabled = true;
    stats.valid = tc->valid;
    stats.codec = tc->codec;
    stats.video_pid = tc->video_pid;
    stats.timecode = buf;
    stats.drop_frame = tc->drop_frame;
    stats.pts = tc->pts;
    stats.updates = tc->updates;

    if (clear)
        tc->updates = 0;

    return true;
}
