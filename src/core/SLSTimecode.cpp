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

#include "SLSTimecode.hpp"

#include <cstdio>
#include <cstring>

#include "common.hpp"

namespace
{

// --- MPEG-TS / PSI constants -------------------------------------------------

constexpr int TS_STREAM_TYPE_H264 = 0x1B;
constexpr int TS_STREAM_TYPE_HEVC = 0x24;
constexpr uint8_t PSI_TABLE_ID_PAT = 0x00;
constexpr uint8_t PSI_TABLE_ID_PMT = 0x02;

// --- NAL / SEI constants -----------------------------------------------------

constexpr int H264_NAL_SEI_TYPE = 6;
constexpr int H264_NAL_SPS_TYPE = 7;
constexpr int HEVC_NAL_PREFIX_SEI = 39;
constexpr int HEVC_NAL_SUFFIX_SEI = 40;
constexpr int SEI_PAYLOAD_PIC_TIMING = 1;  // H.264 D.2.3
constexpr int SEI_PAYLOAD_TIME_CODE = 136; // H.265 D.2.27

// --- bit reader --------------------------------------------------------------
//
// Every read is bounds-checked against the buffer. Once a read runs past the
// end the reader latches `err` and returns zeros, so a truncated or malformed
// payload unwinds through the parse without any caller needing per-read
// checks — the result is simply discarded.

struct bitreader
{
    const uint8_t *buf;
    int len;
    int pos; // in bits
    bool err;
};

uint32_t br_u(bitreader &r, int n)
{
    if (r.err)
        return 0;
    if (n <= 0 || n > 32)
    {
        r.err = true;
        return 0;
    }
    if (r.pos + n > r.len * 8)
    {
        r.err = true;
        return 0;
    }
    uint32_t value = 0;
    for (int i = 0; i < n; i++)
    {
        value = (value << 1) | ((r.buf[r.pos >> 3] >> (7 - (r.pos & 7))) & 1);
        r.pos++;
    }
    return value;
}

// Exp-Golomb ue(v). Used only inside parameter sets; SEI message headers are
// byte-coded, not Exp-Golomb.
uint32_t br_ue(bitreader &r)
{
    int zeros = 0;
    while (!r.err)
    {
        if (br_u(r, 1) == 1)
            break;
        if (++zeros > 31)
        {
            r.err = true;
            return 0;
        }
    }
    if (r.err || zeros == 0)
        return 0;
    return ((1u << zeros) - 1) + br_u(r, zeros);
}

int32_t br_se(bitreader &r)
{
    uint32_t k = br_ue(r);
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

// --- RBSP ---------------------------------------------------------------------

// Strip emulation-prevention bytes (00 00 03 -> 00 00). Every NAL payload must
// go through this before it is bit-read: without it a 0x03 inside a timecode
// shifts every following field.
int rbsp_unescape(const uint8_t *src, int len, uint8_t *dst, int dst_cap)
{
    int out = 0;
    int zeros = 0;
    for (int i = 0; i < len && out < dst_cap; i++)
    {
        uint8_t c = src[i];
        if (zeros >= 2 && c == 0x03)
        {
            zeros = 0;
            continue;
        }
        dst[out++] = c;
        zeros = (c == 0x00) ? zeros + 1 : 0;
    }
    return out;
}

// --- decoded timecode ---------------------------------------------------------

struct clock_timestamp
{
    int hours;
    int minutes;
    int seconds;
    int frames;
    bool drop_frame;
};

// hours/minutes/seconds, in either the full_timestamp form or the nested
// optional-flag form. Shared verbatim between H.264 D.2.3 and H.265 D.2.27.
bool read_hms(bitreader &r, bool full_timestamp, clock_timestamp &tc)
{
    tc.hours = 0;
    tc.minutes = 0;
    tc.seconds = 0;
    if (full_timestamp)
    {
        tc.seconds = (int)br_u(r, 6);
        tc.minutes = (int)br_u(r, 6);
        tc.hours = (int)br_u(r, 5);
    }
    else if (br_u(r, 1)) // seconds_flag
    {
        tc.seconds = (int)br_u(r, 6);
        if (br_u(r, 1)) // minutes_flag
        {
            tc.minutes = (int)br_u(r, 6);
            if (br_u(r, 1)) // hours_flag
                tc.hours = (int)br_u(r, 5);
        }
    }
    return !r.err;
}

// A mis-parsed payload usually decodes to nonsense before it runs out of bits,
// so range-check the wall-clock fields rather than publishing garbage.
bool plausible(const clock_timestamp &tc)
{
    return tc.hours < 24 && tc.minutes < 60 && tc.seconds < 60;
}

// --- H.264 SPS ----------------------------------------------------------------

void skip_scaling_list(bitreader &r, int size)
{
    int last_scale = 8;
    int next_scale = 8;
    for (int i = 0; i < size && !r.err; i++)
    {
        if (next_scale != 0)
        {
            int32_t delta = br_se(r);
            int64_t wrapped_scale = (int64_t)last_scale + delta + 256;
            next_scale = (int)(wrapped_scale % 256);
            if (next_scale < 0)
                next_scale += 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

// hrd_parameters(). Only the trailing field widths matter to us: they set how
// many bits of delay precede pic_struct in every pic_timing SEI.
void parse_hrd(bitreader &r, int &cpb_removal_len, int &dpb_output_len, int &time_offset_len)
{
    uint32_t cpb_cnt = br_ue(r) + 1;
    br_u(r, 4); // bit_rate_scale
    br_u(r, 4); // cpb_size_scale
    if (cpb_cnt > 32)
    {
        r.err = true;
        return;
    }
    for (uint32_t i = 0; i < cpb_cnt && !r.err; i++)
    {
        br_ue(r); // bit_rate_value_minus1
        br_ue(r); // cpb_size_value_minus1
        br_u(r, 1);
    }
    br_u(r, 5); // initial_cpb_removal_delay_length_minus1
    cpb_removal_len = (int)br_u(r, 5) + 1;
    dpb_output_len = (int)br_u(r, 5) + 1;
    time_offset_len = (int)br_u(r, 5);
}

bool profile_has_chroma_ext(uint32_t profile_idc)
{
    switch (profile_idc)
    {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135:
        return true;
    default:
        return false;
    }
}

// Walk the SPS for the three facts pic_timing cannot be read without:
// CpbDpbDelaysPresentFlag (+ the two delay widths), pic_struct_present_flag,
// and time_offset_length. Everything before them has to be parsed only because
// Exp-Golomb fields have no fixed width.
void parse_sps_h264(const uint8_t *rbsp, int len, ts_timecode_state *st)
{
    bitreader r{rbsp, len, 0, false};

    uint32_t profile_idc = br_u(r, 8);
    br_u(r, 8); // constraint flags + reserved
    br_u(r, 8); // level_idc
    br_ue(r);   // seq_parameter_set_id

    uint32_t chroma_format_idc = 1;
    if (profile_has_chroma_ext(profile_idc))
    {
        chroma_format_idc = br_ue(r);
        if (chroma_format_idc == 3)
            br_u(r, 1); // separate_colour_plane_flag
        br_ue(r);       // bit_depth_luma_minus8
        br_ue(r);       // bit_depth_chroma_minus8
        br_u(r, 1);     // qpprime_y_zero_transform_bypass_flag
        if (br_u(r, 1)) // seq_scaling_matrix_present_flag
        {
            int lists = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < lists && !r.err; i++)
            {
                if (br_u(r, 1))
                    skip_scaling_list(r, i < 6 ? 16 : 64);
            }
        }
    }

    br_ue(r); // log2_max_frame_num_minus4
    uint32_t poc_type = br_ue(r);
    if (poc_type == 0)
    {
        br_ue(r); // log2_max_pic_order_cnt_lsb_minus4
    }
    else if (poc_type == 1)
    {
        br_u(r, 1); // delta_pic_order_always_zero_flag
        br_se(r);   // offset_for_non_ref_pic
        br_se(r);   // offset_for_top_to_bottom_field
        uint32_t cycle = br_ue(r);
        if (cycle > 255)
        {
            r.err = true;
            return;
        }
        for (uint32_t i = 0; i < cycle && !r.err; i++)
            br_se(r);
    }

    br_ue(r);   // max_num_ref_frames
    br_u(r, 1); // gaps_in_frame_num_value_allowed_flag
    br_ue(r);   // pic_width_in_mbs_minus1
    br_ue(r);   // pic_height_in_map_units_minus1
    if (br_u(r, 1) == 0)
        br_u(r, 1); // !frame_mbs_only_flag -> mb_adaptive_frame_field_flag
    br_u(r, 1);     // direct_8x8_inference_flag
    if (br_u(r, 1)) // frame_cropping_flag
    {
        br_ue(r);
        br_ue(r);
        br_ue(r);
        br_ue(r);
    }

    bool vui_present = br_u(r, 1) != 0;
    if (r.err)
        return;

    // No VUI means no pic_struct, so pic_timing can carry no timecode. Record
    // the SPS as parsed anyway: that is a definitive answer, not a pending one.
    if (!vui_present)
    {
        st->sps_valid = true;
        st->cpb_dpb_delays_present = false;
        st->cpb_removal_delay_length = 0;
        st->dpb_output_delay_length = 0;
        st->pic_struct_present = false;
        st->time_offset_length = 24;
        return;
    }

    if (br_u(r, 1)) // aspect_ratio_info_present_flag
    {
        if (br_u(r, 8) == 255) // aspect_ratio_idc == Extended_SAR
        {
            br_u(r, 16);
            br_u(r, 16);
        }
    }
    if (br_u(r, 1))
        br_u(r, 1); // overscan_info_present_flag -> overscan_appropriate_flag
    if (br_u(r, 1)) // video_signal_type_present_flag
    {
        br_u(r, 3); // video_format
        br_u(r, 1); // video_full_range_flag
        if (br_u(r, 1))
        {
            br_u(r, 8);
            br_u(r, 8);
            br_u(r, 8);
        }
    }
    if (br_u(r, 1)) // chroma_loc_info_present_flag
    {
        br_ue(r);
        br_ue(r);
    }
    if (br_u(r, 1)) // timing_info_present_flag
    {
        br_u(r, 32); // num_units_in_tick
        br_u(r, 32); // time_scale
        br_u(r, 1);  // fixed_frame_rate_flag
    }

    int cpb_removal_len = 0;
    int dpb_output_len = 0;
    int time_offset_len = 24;
    bool nal_hrd = br_u(r, 1) != 0;
    if (nal_hrd)
        parse_hrd(r, cpb_removal_len, dpb_output_len, time_offset_len);
    bool vcl_hrd = br_u(r, 1) != 0;
    if (vcl_hrd)
        parse_hrd(r, cpb_removal_len, dpb_output_len, time_offset_len);
    if (nal_hrd || vcl_hrd)
        br_u(r, 1); // low_delay_hrd_flag
    bool pic_struct_present = br_u(r, 1) != 0;

    if (r.err)
        return;

    st->sps_valid = true;
    st->cpb_dpb_delays_present = nal_hrd || vcl_hrd;
    st->cpb_removal_delay_length = cpb_removal_len;
    st->dpb_output_delay_length = dpb_output_len;
    st->pic_struct_present = pic_struct_present;
    st->time_offset_length = time_offset_len;
}

// --- SEI payloads --------------------------------------------------------------

// H.264 D.2.3 pic_timing. NumClockTS per Table D-1, indexed by pic_struct.
bool parse_pic_timing_h264(const uint8_t *payload, int size, const ts_timecode_state *st, clock_timestamp &tc)
{
    if (!st->sps_valid || !st->pic_struct_present)
        return false;

    static const int num_clock_ts[9] = {1, 1, 1, 2, 2, 3, 3, 2, 3};

    bitreader r{payload, size, 0, false};
    if (st->cpb_dpb_delays_present)
    {
        br_u(r, st->cpb_removal_delay_length);
        br_u(r, st->dpb_output_delay_length);
    }
    uint32_t pic_struct = br_u(r, 4);
    if (r.err || pic_struct > 8)
        return false;

    for (int i = 0; i < num_clock_ts[pic_struct] && !r.err; i++)
    {
        if (br_u(r, 1) == 0) // clock_timestamp_flag
            continue;
        br_u(r, 2); // ct_type
        br_u(r, 1); // nuit_field_based_flag
        br_u(r, 5); // counting_type
        bool full_timestamp = br_u(r, 1) != 0;
        br_u(r, 1); // discontinuity_flag
        tc.drop_frame = br_u(r, 1) != 0;
        tc.frames = (int)br_u(r, 8); // n_frames
        if (!read_hms(r, full_timestamp, tc))
            return false;
        if (st->time_offset_length > 0)
            br_u(r, st->time_offset_length);
        if (r.err)
            return false;
        return plausible(tc);
    }
    return false;
}

// H.265 D.2.27 time_code. Self-describing: no parameter-set context needed.
bool parse_time_code_hevc(const uint8_t *payload, int size, clock_timestamp &tc)
{
    bitreader r{payload, size, 0, false};
    uint32_t num_clock_ts = br_u(r, 2);
    for (uint32_t i = 0; i < num_clock_ts && !r.err; i++)
    {
        if (br_u(r, 1) == 0) // clock_timestamp_flag
            continue;
        br_u(r, 1); // units_field_based_flag
        br_u(r, 5); // counting_type
        bool full_timestamp = br_u(r, 1) != 0;
        br_u(r, 1); // discontinuity_flag
        tc.drop_frame = br_u(r, 1) != 0;
        tc.frames = (int)br_u(r, 9); // n_frames
        if (!read_hms(r, full_timestamp, tc))
            return false;
        uint32_t time_offset_length = br_u(r, 5);
        if (time_offset_length > 0)
            br_u(r, (int)time_offset_length);
        if (r.err)
            return false;
        return plausible(tc);
    }
    return false;
}

// sei_message() loop. payloadType and payloadSize are byte-coded: 0xFF bytes
// each add 255 and the first non-0xFF byte terminates the count. (This is the
// detail most home-grown SEI readers get wrong by reaching for Exp-Golomb.)
bool scan_sei_messages(const uint8_t *rbsp, int len, const ts_timecode_state *st, clock_timestamp &tc)
{
    const int want = (st->codec == SLS_TC_CODEC_HEVC) ? SEI_PAYLOAD_TIME_CODE : SEI_PAYLOAD_PIC_TIMING;
    int pos = 0;
    while (pos < len)
    {
        // rbsp_trailing_bits: a lone stop bit, not another message.
        if (rbsp[pos] == 0x80 && pos + 1 >= len)
            break;

        int payload_type = 0;
        while (pos < len && rbsp[pos] == 0xFF)
        {
            payload_type += 255;
            pos++;
        }
        if (pos >= len)
            break;
        payload_type += rbsp[pos++];

        int payload_size = 0;
        while (pos < len && rbsp[pos] == 0xFF)
        {
            payload_size += 255;
            pos++;
        }
        if (pos >= len)
            break;
        payload_size += rbsp[pos++];

        if (payload_size < 0 || payload_size > len - pos)
            break;

        if (payload_type == want)
        {
            bool ok = (st->codec == SLS_TC_CODEC_HEVC) ? parse_time_code_hevc(rbsp + pos, payload_size, tc)
                                                       : parse_pic_timing_h264(rbsp + pos, payload_size, st, tc);
            if (ok)
                return true;
        }
        pos += payload_size;
    }
    return false;
}

// --- NAL walk -------------------------------------------------------------------

// Returns true when this NAL yielded a timecode.
bool scan_nal(const uint8_t *nal, int len, ts_timecode_state *st, clock_timestamp &tc)
{
    uint8_t rbsp[SLS_TC_AU_PREFIX_MAX];

    if (st->codec == SLS_TC_CODEC_HEVC)
    {
        if (len < 3)
            return false;
        int nal_type = (nal[0] >> 1) & 0x3F;
        if (nal_type != HEVC_NAL_PREFIX_SEI && nal_type != HEVC_NAL_SUFFIX_SEI)
            return false;
        int n = rbsp_unescape(nal + 2, len - 2, rbsp, (int)sizeof(rbsp));
        return scan_sei_messages(rbsp, n, st, tc);
    }

    if (len < 2)
        return false;
    int nal_type = nal[0] & 0x1F;
    if (nal_type == H264_NAL_SPS_TYPE)
    {
        int n = rbsp_unescape(nal + 1, len - 1, rbsp, (int)sizeof(rbsp));
        parse_sps_h264(rbsp, n, st);
        return false;
    }
    if (nal_type != H264_NAL_SEI_TYPE)
        return false;
    int n = rbsp_unescape(nal + 1, len - 1, rbsp, (int)sizeof(rbsp));
    return scan_sei_messages(rbsp, n, st, tc);
}

bool is_start_code(const uint8_t *b, int n, int i, int &nal_start)
{
    if (i + 3 > n || b[i] != 0x00 || b[i + 1] != 0x00)
        return false;
    if (b[i + 2] == 0x01)
    {
        nal_start = i + 3;
        return true;
    }
    if (i + 4 <= n && b[i + 2] == 0x00 && b[i + 3] == 0x01)
    {
        nal_start = i + 4;
        return true;
    }
    return false;
}

// Scan the buffered access-unit prefix. The SPS (H.264) is picked up here too,
// so a stream whose parameter sets only appear at keyframes becomes readable
// from its first keyframe on.
bool scan_au_prefix(ts_timecode_state *st, clock_timestamp &tc)
{
    const uint8_t *b = st->au;
    const int n = st->au_len;
    bool found = false;
    int i = 0;
    while (i + 3 <= n)
    {
        int nal_start = 0;
        if (!is_start_code(b, n, i, nal_start))
        {
            i++;
            continue;
        }
        int end = nal_start;
        int next_start = 0;
        while (end + 3 <= n && !is_start_code(b, n, end, next_start))
            end++;
        if (end + 3 > n)
            end = n;
        // The first timecode in the access unit wins, which is what a decoder
        // attaches to the picture. Later NALs are still walked (an SPS after
        // the SEI has to refresh the pic_timing context either way), their
        // timecode just does not displace the first one.
        clock_timestamp candidate{};
        if (end > nal_start && scan_nal(b + nal_start, end - nal_start, st, candidate) && !found)
        {
            tc = candidate;
            found = true;
        }
        i = end;
    }
    return found;
}

// --- PES / PSI --------------------------------------------------------------------

int64_t read_pts(const uint8_t *b)
{
    int64_t pts = ((int64_t)(b[0] & 0x0E)) << 29;
    pts |= ((int64_t)b[1]) << 22;
    pts |= ((int64_t)(b[2] & 0xFE)) << 14;
    pts |= ((int64_t)b[3]) << 7;
    pts |= ((int64_t)(b[4] & 0xFE)) >> 1;
    return pts;
}

// Locate a PSI section inside a PUSI payload and return its bounds, or false if
// it does not begin and end inside this one packet. Sections that span packets
// are skipped rather than reassembled: PAT/PMT for a live encoder are a few
// dozen bytes and repeat every <=100ms, so the next copy is always along.
bool psi_section(const uint8_t *payload, int len, uint8_t table_id, const uint8_t *&sect, int &sect_len)
{
    if (len < 1)
        return false;
    int offset = 1 + payload[0]; // pointer_field
    if (offset + 8 > len)
        return false;
    const uint8_t *s = payload + offset;
    int avail = len - offset;
    if (s[0] != table_id)
        return false;
    int section_length = ((s[1] & 0x0F) << 8) | s[2];
    if (section_length < 9 || section_length + 3 > avail)
        return false;
    sect = s;
    sect_len = 3 + section_length - 4; // drop the trailing CRC_32
    return true;
}

void parse_pat(const uint8_t *payload, int len, ts_timecode_state *st)
{
    const uint8_t *s = nullptr;
    int n = 0;
    if (!psi_section(payload, len, PSI_TABLE_ID_PAT, s, n))
        return;

    for (int q = 8; q + 4 <= n; q += 4)
    {
        int program_number = (s[q] << 8) | s[q + 1];
        if (program_number == 0)
            continue; // network PID, not a program
        int pmt_pid = ((s[q + 2] & 0x1F) << 8) | s[q + 3];
        if (pmt_pid != st->pmt_pid)
        {
            // A different program map means everything learned about the old
            // one (video PID, codec, SPS context) is stale.
            st->pmt_pid = pmt_pid;
            st->video_pid = INVALID_PID;
            st->codec = SLS_TC_CODEC_NONE;
            st->sps_valid = false;
            st->au_open = false;
            st->au_len = 0;
        }
        return; // first program wins
    }
}

void parse_pmt(const uint8_t *payload, int len, ts_timecode_state *st)
{
    const uint8_t *s = nullptr;
    int n = 0;
    if (!psi_section(payload, len, PSI_TABLE_ID_PMT, s, n))
        return;
    if (n < 12)
        return;

    int program_info_length = ((s[10] & 0x0F) << 8) | s[11];
    int q = 12 + program_info_length;
    while (q + 5 <= n)
    {
        int stream_type = s[q];
        int elementary_pid = ((s[q + 1] & 0x1F) << 8) | s[q + 2];
        int es_info_length = ((s[q + 3] & 0x0F) << 8) | s[q + 4];

        int codec = SLS_TC_CODEC_NONE;
        if (stream_type == TS_STREAM_TYPE_H264)
            codec = SLS_TC_CODEC_H264;
        else if (stream_type == TS_STREAM_TYPE_HEVC)
            codec = SLS_TC_CODEC_HEVC;

        if (codec != SLS_TC_CODEC_NONE)
        {
            if (elementary_pid != st->video_pid || codec != st->codec)
            {
                st->video_pid = elementary_pid;
                st->codec = codec;
                st->sps_valid = false;
                st->au_open = false;
                st->au_len = 0;
            }
            return; // first video ES wins
        }
        q += 5 + es_info_length;
    }
}

void au_append(ts_timecode_state *st, const uint8_t *data, int len)
{
    if (len <= 0)
        return;
    int room = SLS_TC_AU_PREFIX_MAX - st->au_len;
    if (len > room)
        len = room;
    if (len > 0)
    {
        memcpy(st->au + st->au_len, data, (size_t)len);
        st->au_len += len;
    }
}

// Commit whatever the buffered access unit yielded and reset the accumulator.
int au_flush(ts_timecode_state *st)
{
    int decoded = 0;
    if (st->au_len > 0)
    {
        clock_timestamp tc{};
        if (scan_au_prefix(st, tc))
        {
            st->valid = true;
            st->hours = tc.hours;
            st->minutes = tc.minutes;
            st->seconds = tc.seconds;
            st->frames = tc.frames;
            st->drop_frame = tc.drop_frame;
            st->pts = st->au_pts;
            st->updates++;
            decoded = 1;
        }
    }
    st->au_open = false;
    st->au_len = 0;
    return decoded;
}

int feed_video_packet(const uint8_t *payload, int len, bool pusi, ts_timecode_state *st)
{
    int decoded = 0;

    if (pusi)
    {
        // The previous access unit ends where this one starts.
        decoded += au_flush(st);
        st->au_pts = INVALID_DTS_PTS;

        if (len < 9 || payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01)
            return decoded;
        int stream_id = payload[3];
        if (stream_id < 0xE0 || stream_id > 0xEF) // video streams only
            return decoded;
        int pts_dts_flags = payload[7] & 0xC0;
        int header_len = payload[8];
        int es_offset = 9 + header_len;
        if (es_offset > len)
            return decoded;
        if ((pts_dts_flags == 0x80 && header_len >= 5) || (pts_dts_flags == 0xC0 && header_len >= 10))
            st->au_pts = read_pts(payload + 9);
        st->au_open = true;
        au_append(st, payload + es_offset, len - es_offset);
    }
    else if (st->au_open)
    {
        au_append(st, payload, len);
    }

    // Full prefix: scan now and ignore the rest of this access unit. SEI always
    // precedes the first slice, so anything past the prefix is picture data.
    if (st->au_open && st->au_len >= SLS_TC_AU_PREFIX_MAX)
        decoded += au_flush(st);

    return decoded;
}

} // namespace

void sls_init_ts_timecode_state(ts_timecode_state *st)
{
    if (st == nullptr)
        return;

    st->pmt_pid = INVALID_PID;
    st->video_pid = INVALID_PID;
    st->codec = SLS_TC_CODEC_NONE;

    st->sps_valid = false;
    st->cpb_dpb_delays_present = false;
    st->cpb_removal_delay_length = 0;
    st->dpb_output_delay_length = 0;
    st->pic_struct_present = false;
    st->time_offset_length = 24;

    st->au_len = 0;
    st->au_open = false;
    st->au_pts = INVALID_DTS_PTS;

    st->valid = false;
    st->hours = 0;
    st->minutes = 0;
    st->seconds = 0;
    st->frames = 0;
    st->drop_frame = false;
    st->pts = INVALID_DTS_PTS;
    st->updates = 0;
}

int sls_ts_scan_timecode(const uint8_t *data, int len, ts_timecode_state *st)
{
    if (data == nullptr || st == nullptr)
        return 0;

    int decoded = 0;
    for (int i = 0; i + TS_PACK_LEN <= len; i += TS_PACK_LEN)
    {
        const uint8_t *p = data + i;
        if (p[0] != TS_SYNC_BYTE)
            continue;
        if (p[1] & 0x80) // transport_error_indicator: the payload is not trustworthy
            continue;

        int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid == 0x1FFF) // null packets
            continue;

        int afc = (p[3] >> 4) & 0x3;
        if ((afc & 0x1) == 0) // no payload
            continue;
        int pos = 4;
        if (afc & 0x2)
        {
            pos += 1 + p[4]; // adaptation_field_length
            if (pos >= TS_PACK_LEN)
                continue;
        }
        const uint8_t *payload = p + pos;
        int payload_len = TS_PACK_LEN - pos;
        bool pusi = (p[1] & 0x40) != 0;

        if (pid == PAT_PID)
        {
            if (pusi)
                parse_pat(payload, payload_len, st);
        }
        else if (pid == st->pmt_pid)
        {
            if (pusi)
                parse_pmt(payload, payload_len, st);
        }
        else if (pid == st->video_pid && st->codec != SLS_TC_CODEC_NONE)
        {
            decoded += feed_video_packet(payload, payload_len, pusi, st);
        }
    }
    return decoded;
}

void sls_format_timecode(const ts_timecode_state *st, char *buf, int buf_len)
{
    if (buf == nullptr || buf_len <= 0)
        return;
    if (st == nullptr || !st->valid)
    {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, (size_t)buf_len, "%02d:%02d:%02d%c%02d", st->hours, st->minutes, st->seconds,
             st->drop_frame ? ';' : ':', st->frames);
}
