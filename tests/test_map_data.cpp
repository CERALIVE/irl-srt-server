#include "doctest.h"

#include <cstdint>
#include <cstring>

#include "SLSMapData.hpp"
#include "common.hpp"

// CSLSMapData owns the per-stream publisher rings and the global ring budget
// (stream-count + cumulative-bytes caps) that guard against pre-auth ring OOM.
// These tests pin the two invariants the security fix relies on: caps refuse
// allocation past their limit, and the budget counters stay exactly
// alloc-balanced across idempotent re-adds and removals.

TEST_CASE("CSLSMapData::add refuses new streams past the stream-count cap")
{
    CSLSMapData m;
    m.set_caps(2, 0); // 0 = unlimited bytes; cap on count only

    char s1[] = "app/s1";
    char s2[] = "app/s2";
    char s3[] = "app/s3";

    CHECK(m.add(s1) == SLS_OK);
    CHECK(m.add(s2) == SLS_OK);
    CHECK(m.get_stream_count() == 2);

    CHECK(m.add(s3) == SLS_ERROR);    // third stream over the cap of 2
    CHECK(m.get_stream_count() == 2); // a refused add must not change the count
}

TEST_CASE("CSLSMapData::add refuses new streams past the total ring-memory cap")
{
    // Probe the default per-stream ring size (no bitrate/latency hint).
    CSLSMapData probe;
    probe.set_caps(0, 0);
    char pk[] = "app/probe";
    CHECK(probe.add(pk) == SLS_OK);
    int64_t ring = probe.get_total_ring_bytes();
    CHECK(ring > 0);

    // Budget for exactly one default ring (1.5x leaves no room for a second).
    CSLSMapData m;
    m.set_caps(0, ring + ring / 2);
    char a[] = "app/a";
    char b[] = "app/b";

    CHECK(m.add(a) == SLS_OK);
    CHECK(m.get_total_ring_bytes() == ring);

    CHECK(m.add(b) == SLS_ERROR); // second ring would exceed the byte cap
    CHECK(m.get_stream_count() == 1);
    CHECK(m.get_total_ring_bytes() == ring); // refusal leaves the budget intact
}

TEST_CASE("CSLSMapData budget is alloc-balanced across add / dupe-add / remove")
{
    CSLSMapData m;
    m.set_caps(0, 0); // unlimited

    char k[] = "app/dup";

    CHECK(m.add(k) == SLS_OK);
    CHECK(m.get_stream_count() == 1);
    int64_t after_add = m.get_total_ring_bytes();
    CHECK(after_add > 0);

    // Idempotent re-add (the puller's connect-time double-add pattern) must
    // hit the early-return and NOT double-count the budget.
    CHECK(m.add(k) == SLS_OK);
    CHECK(m.get_stream_count() == 1);
    CHECK(m.get_total_ring_bytes() == after_add);

    CHECK(m.remove(k) == SLS_OK);
    CHECK(m.get_stream_count() == 0);
    CHECK(m.get_total_ring_bytes() == 0);
}

TEST_CASE("CSLSMapData::clear zeroes the budget for every freed ring")
{
    CSLSMapData m;
    m.set_caps(0, 0);

    char k1[] = "app/c1";
    char k2[] = "app/c2";
    CHECK(m.add(k1) == SLS_OK);
    CHECK(m.add(k2) == SLS_OK);
    CHECK(m.get_stream_count() == 2);
    CHECK(m.get_total_ring_bytes() > 0);

    m.clear();
    CHECK(m.get_stream_count() == 0);
    CHECK(m.get_total_ring_bytes() == 0);
}

TEST_CASE("CSLSMapData: audio-gap-fill flag survives a hinted (lazy-style) alloc")
{
    CSLSMapData m;
    m.set_caps(0, 0);

    // Mirror the lazy publisher path: add() with a bitrate + latency hint, then
    // apply the gap-fill flag (as handler_read_data does via on_map_data_set).
    char k[] = "app/gap";
    CHECK(m.add(k, 8000 /*kbps*/, 2000 /*ms*/) == SLS_OK);
    m.set_audio_gap_fill(k, true);

    CSLSMapData::AudioGapStreamStats stats;
    bool found = m.get_audio_gap_stats(k, stats);
    CHECK(found);
    CHECK(stats.enabled);
}

TEST_CASE("CSLSMapData: timecode opt-in coexists with audio gap filling and resets on removal")
{
    CSLSMapData m;
    char key[] = "app/audio-timecode";
    REQUIRE(m.add(key) == SLS_OK);
    CSLSMapData::TimecodeStats timecode;
    CHECK_FALSE(m.get_timecode_stats(key, timecode));
    m.set_audio_gap_fill(key, true);
    m.set_timecode_scan(key, true);

    REQUIRE(m.get_timecode_stats(key, timecode));
    CHECK(timecode.enabled);
    CHECK_FALSE(timecode.valid);
    CSLSMapData::AudioGapStreamStats audio;
    REQUIRE(m.get_audio_gap_stats(key, audio));
    CHECK(audio.enabled);

    REQUIRE(m.add(key, 8000, 2000) == SLS_OK);
    REQUIRE(m.get_timecode_stats(key, timecode));
    m.set_timecode_scan(key, false);
    CHECK_FALSE(m.get_timecode_stats(key, timecode));
    REQUIRE(m.get_audio_gap_stats(key, audio));
    CHECK(audio.enabled);

    m.set_timecode_scan(key, true);
    REQUIRE(m.remove(key) == SLS_OK);
    CHECK_FALSE(m.get_audio_gap_stats(key, audio));
    CHECK_FALSE(m.get_timecode_stats(key, timecode));
    REQUIRE(m.add(key) == SLS_OK);
    REQUIRE(m.get_audio_gap_stats(key, audio));
    CHECK_FALSE(audio.enabled);
    CHECK_FALSE(m.get_timecode_stats(key, timecode));
}

TEST_CASE("CSLSMapData: viewer diagnostics accumulate independently and clear per interval")
{
    CSLSMapData m;
    char key[] = "app/viewers";
    REQUIRE(m.add(key) == SLS_OK);
    m.report_viewer_backpressure(key);
    m.report_viewer_backpressure(key);
    m.report_viewer_snd_drops(key, 3);
    m.report_viewer_snd_drops(key, -1);
    CHECK(m.get_viewer_backpressure_events(key, true) == 2);
    CHECK(m.get_viewer_snd_drops(key, true) == 3);
    CHECK(m.get_viewer_backpressure_events(key) == 0);
    CHECK(m.get_viewer_snd_drops(key) == 0);
    CHECK(m.get_ingest_discontinuities(key) == 0);
    CHECK(m.get_max_reader_backlog(key) == 0);
    CHECK(m.get_viewer_snd_drops("missing") == -1);
}

TEST_CASE("CSLSMapData: joins and reconnects relay only live ring bytes without cached headers")
{
    CSLSMapData m;
    char key[] = "app/reconnect";
    REQUIRE(m.add(key) == SLS_OK);
    SLSRecycleArrayID reader{};
    reader.bFirst = true;
    char out[TS_UDP_LEN]{};
    CHECK(m.get(key, out, sizeof(out), &reader) == SLS_OK);

    char old[] = "old-session";
    REQUIRE(m.put(key, old, sizeof(old)) == (int)sizeof(old));
    CHECK(m.get(key, out, sizeof(out), &reader) == (int)sizeof(old));
    REQUIRE(m.remove(key) == SLS_OK);
    REQUIRE(m.add(key) == SLS_OK);
    char before_join[] = "before-rejoin";
    REQUIRE(m.put(key, before_join, sizeof(before_join)) == (int)sizeof(before_join));
    CHECK(m.get(key, out, sizeof(out), &reader) == SLS_OK);

    char live[] = "live";
    REQUIRE(m.put(key, live, sizeof(live)) == (int)sizeof(live));
    REQUIRE(m.get(key, out, sizeof(out), &reader) == (int)sizeof(live));
    CHECK(std::memcmp(out, live, sizeof(live)) == 0);
}
