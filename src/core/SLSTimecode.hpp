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

#include <cstdint>

/**
 * In-band SMPTE timecode, read out of the publisher's video elementary stream.
 *
 * Two carriages exist and this reads both:
 *
 *   H.264  pic_timing SEI (payload type 1), clock_timestamp syntax, ITU-T
 *          H.264 D.2.3. It is NOT self-describing: the width of the delay
 *          fields that precede the timestamp, and whether pic_struct is
 *          present at all, live in the active SPS's VUI/HRD. An SPS must be
 *          parsed before a pic_timing payload can be decoded.
 *   HEVC   time_code SEI (payload type 136), ITU-T H.265 D.2.27.
 *          Self-describing; no parameter-set context needed.
 *
 * Note that payload type 136 does not exist in H.264 and pic_timing carries no
 * timecode in HEVC, so the codec (from the PMT stream_type) selects which
 * payload to look at.
 *
 * The scanner is pure: it takes bytes and its own state, touches no socket and
 * no ts_info, and never mutates the stream. Everything it needs (program map,
 * video PID, codec, SPS context) it learns from the stream itself.
 */

// One access unit's leading bytes are buffered so SEI NALs that straddle TS
// packet boundaries are still seen whole. SEI sits at the head of an access
// unit, before the first slice, so a prefix is enough — but it has to be a
// generous one: a keyframe AU leads with AUD, VPS/SPS/PPS and whatever
// informational SEI the encoder emits before the timecode (x265's options
// string alone runs ~2 KB), and a prefix that stops short of the timecode SEI
// silently loses it on exactly the keyframes. 8 KB clears that with room to
// spare and still bounds both the buffer and the per-frame scan.
#define SLS_TC_AU_PREFIX_MAX 8192

enum sls_tc_codec
{
    SLS_TC_CODEC_NONE = 0,
    SLS_TC_CODEC_H264,
    SLS_TC_CODEC_HEVC,
};

struct ts_timecode_state
{
    // --- program structure, learned from PAT/PMT ---
    int pmt_pid;
    int video_pid;
    int codec; // sls_tc_codec

    // --- H.264 pic_timing context, learned from the SPS VUI ---
    bool sps_valid;
    bool cpb_dpb_delays_present;  // CpbDpbDelaysPresentFlag (nal_hrd || vcl_hrd)
    int cpb_removal_delay_length; // bits, valid when cpb_dpb_delays_present
    int dpb_output_delay_length;  // bits, valid when cpb_dpb_delays_present
    bool pic_struct_present;      // pic_struct_present_flag
    int time_offset_length;       // bits; 24 when no HRD is signalled (D.2.2)

    // --- access-unit prefix accumulator ---
    uint8_t au[SLS_TC_AU_PREFIX_MAX];
    int au_len;
    bool au_open;   // an access unit is being accumulated
    int64_t au_pts; // PTS of that access unit (90 kHz), INVALID_DTS_PTS if none

    // --- last decoded timecode ---
    bool valid;
    int hours;
    int minutes;
    int seconds;
    int frames;
    bool drop_frame; // cnt_dropped_flag
    int64_t pts;     // PTS of the access unit the timecode came from
    uint64_t updates;
};

void sls_init_ts_timecode_state(ts_timecode_state *st);

/**
 * Feed one ingest chunk. `data` is scanned as whole 188-byte TS packets; a
 * ragged tail is ignored, exactly like sls_ts_check_continuity. Returns the
 * number of timecodes decoded from this chunk (0 for the overwhelming majority
 * of chunks: at most one per access unit, and only if the encoder emits any).
 */
int sls_ts_scan_timecode(const uint8_t *data, int len, ts_timecode_state *st);

/**
 * Format as SMPTE "HH:MM:SS:FF" (";" before frames when the timecode is
 * drop-frame, per SMPTE convention). Writes at most 12 bytes plus NUL; `buf`
 * must hold at least 13. Empty string if the state carries no timecode.
 */
void sls_format_timecode(const ts_timecode_state *st, char *buf, int buf_len);
