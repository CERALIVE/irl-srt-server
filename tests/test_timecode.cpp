#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "SLSMapData.hpp"
#include "SLSTimecode.hpp"
#include "common.hpp"

// The timecode scanner never touches a socket: it takes TS bytes and its own
// state. These cases build real transport streams around synthetic SEI NALs —
// H.264 pic_timing (payload type 1, which needs SPS/VUI context) and HEVC
// time_code (payload type 136) — and assert what comes back out.

namespace
{

// --- bit writer, for building the SEI payloads bit-exactly ---

class BitWriter
{
public:
    void u(int n, uint32_t value)
    {
        for (int i = n - 1; i >= 0; i--)
            m_bits.push_back((value >> i) & 1);
    }
    // Exp-Golomb ue(v), as parameter sets code their variable-width fields.
    void ue(uint32_t value)
    {
        uint64_t code = (uint64_t)value + 1;
        int bits = 0;
        while ((code >> bits) != 0)
            bits++;
        for (int i = 0; i < bits - 1; i++)
            m_bits.push_back(0);
        u(bits, (uint32_t)code);
    }
    void rbsp_trailing()
    {
        m_bits.push_back(1);
        while (m_bits.size() % 8 != 0)
            m_bits.push_back(0);
    }
    std::vector<uint8_t> bytes() const
    {
        std::vector<uint8_t> out((m_bits.size() + 7) / 8, 0);
        for (size_t i = 0; i < m_bits.size(); i++)
        {
            if (m_bits[i])
                out[i / 8] |= (uint8_t)(0x80 >> (i % 8));
        }
        return out;
    }
    size_t bit_count() const
    {
        return m_bits.size();
    }

private:
    std::vector<uint8_t> m_bits;
};

// --- SEI construction ---

// sei_message(): byte-coded payloadType / payloadSize (0xFF chains), NOT ue(v).
void append_sei_message(std::vector<uint8_t> &out, int payload_type, const std::vector<uint8_t> &payload)
{
    int t = payload_type;
    while (t >= 255)
    {
        out.push_back(0xFF);
        t -= 255;
    }
    out.push_back((uint8_t)t);
    size_t n = payload.size();
    while (n >= 255)
    {
        out.push_back(0xFF);
        n -= 255;
    }
    out.push_back((uint8_t)n);
    out.insert(out.end(), payload.begin(), payload.end());
}

// H.264 D.2.3 pic_timing, for an SPS with no HRD (no delay fields) and
// pic_struct_present_flag = 1, time_offset_length = 24.
std::vector<uint8_t> pic_timing_payload_h264(int h, int m, int s, int f, bool drop_frame = false,
                                             bool full_timestamp = true)
{
    BitWriter w;
    w.u(4, 0); // pic_struct = 0 (frame) -> NumClockTS = 1
    w.u(1, 1); // clock_timestamp_flag
    w.u(2, 0); // ct_type
    w.u(1, 0); // nuit_field_based_flag
    w.u(5, 0); // counting_type
    w.u(1, full_timestamp ? 1 : 0);
    w.u(1, 0); // discontinuity_flag
    w.u(1, drop_frame ? 1 : 0);
    w.u(8, (uint32_t)f); // n_frames
    if (full_timestamp)
    {
        w.u(6, (uint32_t)s);
        w.u(6, (uint32_t)m);
        w.u(5, (uint32_t)h);
    }
    else
    {
        w.u(1, 1);
        w.u(6, (uint32_t)s);
        w.u(1, 1);
        w.u(6, (uint32_t)m);
        w.u(1, 1);
        w.u(5, (uint32_t)h);
    }
    w.u(24, 0); // time_offset, i(time_offset_length = 24)
    while (w.bit_count() % 8 != 0)
        w.u(1, 0);
    return w.bytes();
}

// H.265 D.2.27 time_code.
std::vector<uint8_t> time_code_payload_hevc(int h, int m, int s, int f, bool drop_frame = false,
                                            int time_offset_length = 0, uint32_t time_offset = 0)
{
    BitWriter w;
    w.u(2, 1); // num_clock_ts
    w.u(1, 1); // clock_timestamp_flag
    w.u(1, 0); // units_field_based_flag
    w.u(5, 0); // counting_type
    w.u(1, 1); // full_timestamp_flag
    w.u(1, 0); // discontinuity_flag
    w.u(1, drop_frame ? 1 : 0);
    w.u(9, (uint32_t)f); // n_frames
    w.u(6, (uint32_t)s);
    w.u(6, (uint32_t)m);
    w.u(5, (uint32_t)h);
    w.u(5, (uint32_t)time_offset_length);
    if (time_offset_length > 0)
        w.u(time_offset_length, time_offset);
    while (w.bit_count() % 8 != 0)
        w.u(1, 0);
    return w.bytes();
}

// --- NAL construction ---

// Insert emulation-prevention bytes, the way a real encoder writes a NAL.
std::vector<uint8_t> escape_rbsp(const std::vector<uint8_t> &rbsp)
{
    std::vector<uint8_t> out;
    int zeros = 0;
    for (uint8_t c : rbsp)
    {
        if (zeros >= 2 && c <= 0x03)
        {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(c);
        zeros = (c == 0x00) ? zeros + 1 : 0;
    }
    return out;
}

std::vector<uint8_t> make_nal(const std::vector<uint8_t> &header, const std::vector<uint8_t> &rbsp)
{
    std::vector<uint8_t> nal = {0x00, 0x00, 0x00, 0x01};
    nal.insert(nal.end(), header.begin(), header.end());
    std::vector<uint8_t> escaped = escape_rbsp(rbsp);
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return nal;
}

std::vector<uint8_t> sei_nal_h264(const std::vector<uint8_t> &messages)
{
    std::vector<uint8_t> rbsp = messages;
    rbsp.push_back(0x80); // rbsp_trailing_bits
    return make_nal({0x06}, rbsp);
}

std::vector<uint8_t> sei_nal_hevc(const std::vector<uint8_t> &messages)
{
    std::vector<uint8_t> rbsp = messages;
    rbsp.push_back(0x80);
    return make_nal({0x4E, 0x01}, rbsp); // nal_unit_type 39 (PREFIX_SEI_NUT)
}

// A minimal baseline SPS: no HRD, VUI present with pic_struct_present_flag = 1.
// This is what makes an H.264 pic_timing payload decodable at all.
std::vector<uint8_t> sps_nal_h264(bool pic_struct_present = true, bool vui_present = true)
{
    BitWriter w;
    w.u(8, 66); // profile_idc = baseline (no chroma extension fields)
    w.u(8, 0);  // constraint flags
    w.u(8, 30); // level_idc
    w.ue(0);    // seq_parameter_set_id
    w.ue(0);    // log2_max_frame_num_minus4
    w.ue(0);    // pic_order_cnt_type
    w.ue(0);    // log2_max_pic_order_cnt_lsb_minus4
    w.ue(1);    // max_num_ref_frames
    w.u(1, 0);  // gaps_in_frame_num_value_allowed_flag
    w.ue(39);   // pic_width_in_mbs_minus1  (640 px)
    w.ue(22);   // pic_height_in_map_units_minus1 (368 px)
    w.u(1, 1);  // frame_mbs_only_flag
    w.u(1, 1);  // direct_8x8_inference_flag
    w.u(1, 0);  // frame_cropping_flag
    w.u(1, vui_present ? 1 : 0);
    if (vui_present)
    {
        w.u(1, 0); // aspect_ratio_info_present_flag
        w.u(1, 0); // overscan_info_present_flag
        w.u(1, 0); // video_signal_type_present_flag
        w.u(1, 0); // chroma_loc_info_present_flag
        w.u(1, 0); // timing_info_present_flag
        w.u(1, 0); // nal_hrd_parameters_present_flag
        w.u(1, 0); // vcl_hrd_parameters_present_flag
        w.u(1, pic_struct_present ? 1 : 0);
        w.u(1, 0); // bitstream_restriction_flag
    }
    w.rbsp_trailing();
    return make_nal({0x67}, w.bytes()); // nal_ref_idc = 3, type 7
}

std::vector<uint8_t> sps_nal_h264_extreme_scaling_delta()
{
    BitWriter w;
    w.u(8, 100);
    w.u(8, 0);
    w.u(8, 30);
    w.ue(0);
    w.ue(1);
    w.ue(0);
    w.ue(0);
    w.u(1, 0);
    w.u(1, 1);
    w.u(1, 1);
    w.ue(UINT32_MAX - 2);
    for (int i = 1; i < 16; ++i)
        w.ue(0);
    for (int i = 1; i < 8; ++i)
        w.u(1, 0);
    w.ue(0);
    w.ue(0);
    w.ue(0);
    w.ue(1);
    w.u(1, 0);
    w.ue(39);
    w.ue(22);
    w.u(1, 1);
    w.u(1, 1);
    w.u(1, 0);
    w.u(1, 0);
    w.rbsp_trailing();
    return make_nal({0x67}, w.bytes());
}

// --- transport stream construction ---

constexpr int PMT_PID = 0x1000;
constexpr int VIDEO_PID = 0x0100;

void push_packet(std::vector<uint8_t> &ts, std::vector<uint8_t> pkt)
{
    REQUIRE(pkt.size() <= TS_PACK_LEN);
    pkt.resize(TS_PACK_LEN, 0xFF);
    ts.insert(ts.end(), pkt.begin(), pkt.end());
}

std::vector<uint8_t> psi_packet(int pid, const std::vector<uint8_t> &section)
{
    std::vector<uint8_t> pkt = {0x47, (uint8_t)(0x40 | ((pid >> 8) & 0x1F)), (uint8_t)(pid & 0xFF), 0x10};
    pkt.push_back(0x00); // pointer_field
    pkt.insert(pkt.end(), section.begin(), section.end());
    return pkt;
}

// section_length covers everything after it, CRC_32 included.
void finish_section(std::vector<uint8_t> &section)
{
    size_t body = section.size() - 3 + 4;
    section[1] = (uint8_t)(0xB0 | ((body >> 8) & 0x0F));
    section[2] = (uint8_t)(body & 0xFF);
    section.insert(section.end(), {0xDE, 0xAD, 0xBE, 0xEF}); // CRC_32 (not verified)
}

std::vector<uint8_t> pat_packet(int pmt_pid = PMT_PID)
{
    std::vector<uint8_t> s = {0x00, 0x00, 0x00, 0x00, 0x01, 0xC1, 0x00, 0x00};
    s.push_back(0x00);
    s.push_back(0x01); // program_number = 1
    s.push_back((uint8_t)(0xE0 | ((pmt_pid >> 8) & 0x1F)));
    s.push_back((uint8_t)(pmt_pid & 0xFF));
    finish_section(s);
    return psi_packet(PAT_PID, s);
}

std::vector<uint8_t> pmt_packet(int stream_type, int video_pid = VIDEO_PID, int pmt_pid = PMT_PID)
{
    std::vector<uint8_t> s = {0x02, 0x00, 0x00, 0x00, 0x01, 0xC1, 0x00, 0x00};
    s.push_back(0xE0);
    s.push_back(0x00); // PCR_PID
    s.push_back(0xF0);
    s.push_back(0x00); // program_info_length = 0
    s.push_back((uint8_t)stream_type);
    s.push_back((uint8_t)(0xE0 | ((video_pid >> 8) & 0x1F)));
    s.push_back((uint8_t)(video_pid & 0xFF));
    s.push_back(0xF0);
    s.push_back(0x00); // es_info_length = 0
    finish_section(s);
    return psi_packet(pmt_pid, s);
}

// Wrap an access unit in a PES and split it across as many TS packets as it
// needs, so SEI that straddles a packet boundary is exercised for real.
void push_video_au(std::vector<uint8_t> &ts, const std::vector<uint8_t> &au, int64_t pts, uint8_t &cc,
                   int video_pid = VIDEO_PID)
{
    std::vector<uint8_t> pes = {0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x80, 0x80, 0x05};
    pes.push_back((uint8_t)(0x21 | (((pts >> 30) & 0x07) << 1)));
    pes.push_back((uint8_t)((pts >> 22) & 0xFF));
    pes.push_back((uint8_t)(0x01 | (((pts >> 15) & 0x7F) << 1)));
    pes.push_back((uint8_t)((pts >> 7) & 0xFF));
    pes.push_back((uint8_t)(0x01 | ((pts & 0x7F) << 1)));
    pes.insert(pes.end(), au.begin(), au.end());

    size_t offset = 0;
    bool first = true;
    while (offset < pes.size())
    {
        std::vector<uint8_t> pkt = {0x47, (uint8_t)((first ? 0x40 : 0x00) | ((video_pid >> 8) & 0x1F)),
                                    (uint8_t)(video_pid & 0xFF), (uint8_t)(0x10 | (cc & 0x0F))};
        cc = (uint8_t)((cc + 1) & 0x0F);
        size_t take = std::min<size_t>(TS_PACK_LEN - 4, pes.size() - offset);
        pkt.insert(pkt.end(), pes.begin() + offset, pes.begin() + offset + take);
        offset += take;
        first = false;
        push_packet(ts, pkt);
    }
}

std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts)
{
    std::vector<uint8_t> out;
    for (const auto &p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::string formatted(const ts_timecode_state &st)
{
    char buf[16] = {0};
    sls_format_timecode(&st, buf, sizeof(buf));
    return std::string(buf);
}

} // namespace

TEST_CASE("timecode: h.264 pic_timing SEI is decoded once SPS/VUI is seen")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x1B));

    std::vector<uint8_t> messages;
    append_sei_message(messages, 1, pic_timing_payload_h264(1, 2, 3, 4));
    std::vector<uint8_t> au = concat({sps_nal_h264(), sei_nal_h264(messages), make_nal({0x65}, {0x88, 0x84, 0x00})});

    uint8_t cc = 0;
    push_video_au(ts, au, 90000, cc);
    // The scan of an access unit is committed when the next one starts.
    push_video_au(ts, au, 93000, cc);

    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 1);
    CHECK(st.video_pid == VIDEO_PID);
    CHECK(st.codec == SLS_TC_CODEC_H264);
    CHECK(st.sps_valid);
    CHECK(st.pic_struct_present);
    CHECK(st.valid);
    CHECK(st.hours == 1);
    CHECK(st.minutes == 2);
    CHECK(st.seconds == 3);
    CHECK(st.frames == 4);
    CHECK(st.drop_frame == false);
    CHECK(st.pts == 90000);
    CHECK(st.updates == 1);
    CHECK(formatted(st) == "01:02:03:04");
}

TEST_CASE("timecode: hevc time_code SEI is decoded without parameter sets")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x24));

    std::vector<uint8_t> messages;
    append_sei_message(messages, 136, time_code_payload_hevc(23, 59, 58, 29));
    std::vector<uint8_t> au = concat({sei_nal_hevc(messages), make_nal({0x26, 0x01}, {0xAF, 0x00, 0x01})});

    uint8_t cc = 0;
    push_video_au(ts, au, 123456, cc);
    push_video_au(ts, au, 126456, cc);

    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 1);
    CHECK(st.codec == SLS_TC_CODEC_HEVC);
    CHECK(st.valid);
    CHECK(st.hours == 23);
    CHECK(st.minutes == 59);
    CHECK(st.seconds == 58);
    CHECK(st.frames == 29);
    CHECK(st.pts == 123456);
    CHECK(formatted(st) == "23:59:58:29");
}

TEST_CASE("timecode: 31-bit HEVC time offset is consumed without signed overflow")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);
    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x24));
    std::vector<uint8_t> messages;
    append_sei_message(messages, 136,
                       time_code_payload_hevc(1, 2, 3, 4, false, 31, 0x40000000));
    std::vector<uint8_t> au = sei_nal_hevc(messages);
    uint8_t cc = 0;
    push_video_au(ts, au, 1000, cc);
    push_video_au(ts, au, 4000, cc);
    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 1);
    CHECK(formatted(st) == "01:02:03:04");
}

TEST_CASE("timecode: extreme H.264 scaling delta is parsed without signed overflow")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);
    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x1B));
    std::vector<uint8_t> au = sps_nal_h264_extreme_scaling_delta();
    uint8_t cc = 0;
    push_video_au(ts, au, 1000, cc);
    push_video_au(ts, au, 4000, cc);
    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 0);
    CHECK(st.sps_valid);
}

TEST_CASE("timecode: drop-frame is reported and formatted with a semicolon")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x24));

    std::vector<uint8_t> messages;
    append_sei_message(messages, 136, time_code_payload_hevc(10, 20, 30, 15, /*drop_frame=*/true));
    std::vector<uint8_t> au = sei_nal_hevc(messages);

    uint8_t cc = 0;
    push_video_au(ts, au, 1000, cc);
    push_video_au(ts, au, 4000, cc);

    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 1);
    CHECK(st.drop_frame);
    CHECK(formatted(st) == "10:20:30;15");
}

TEST_CASE("timecode: the non-full_timestamp form is decoded")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x1B));

    std::vector<uint8_t> messages;
    append_sei_message(messages, 1, pic_timing_payload_h264(5, 6, 7, 8, false, /*full_timestamp=*/false));
    std::vector<uint8_t> au = concat({sps_nal_h264(), sei_nal_h264(messages)});

    uint8_t cc = 0;
    push_video_au(ts, au, 500, cc);
    push_video_au(ts, au, 3500, cc);

    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 1);
    CHECK(formatted(st) == "05:06:07:08");
}

TEST_CASE("timecode: emulation-prevention bytes inside the payload are stripped")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x24));

    // 00:00:00:00 encodes as a run of zero bytes, so the escaped NAL really
    // does carry 00 00 03 sequences: a scanner that skips the unescape step
    // reads shifted garbage here.
    std::vector<uint8_t> messages;
    append_sei_message(messages, 136, time_code_payload_hevc(0, 0, 0, 0));
    std::vector<uint8_t> nal = sei_nal_hevc(messages);
    bool has_escape = false;
    for (size_t i = 4; i + 2 < nal.size(); i++)
    {
        if (nal[i] == 0x00 && nal[i + 1] == 0x00 && nal[i + 2] == 0x03)
            has_escape = true;
    }
    REQUIRE(has_escape);

    uint8_t cc = 0;
    push_video_au(ts, nal, 7, cc);
    push_video_au(ts, nal, 3007, cc);

    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 1);
    CHECK(formatted(st) == "00:00:00:00");
}

TEST_CASE("timecode: an SEI carrying only other payload types yields nothing")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x1B));

    std::vector<uint8_t> messages;
    append_sei_message(messages, 5, std::vector<uint8_t>(20, 0x5A));  // user_data_unregistered
    append_sei_message(messages, 137, std::vector<uint8_t>(24, 0x11)); // mastering display
    std::vector<uint8_t> au = concat({sps_nal_h264(), sei_nal_h264(messages)});

    uint8_t cc = 0;
    push_video_au(ts, au, 10, cc);
    push_video_au(ts, au, 3010, cc);

    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 0);
    CHECK(st.valid == false);
    CHECK(formatted(st).empty());
}

TEST_CASE("timecode: h.264 pic_timing is held back until an SPS explains it")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x1B));

    std::vector<uint8_t> messages;
    append_sei_message(messages, 1, pic_timing_payload_h264(9, 8, 7, 6));

    uint8_t cc = 0;
    // No SPS yet: pic_timing is unparseable, and guessing is what produces
    // plausible-looking nonsense.
    push_video_au(ts, sei_nal_h264(messages), 100, cc);
    push_video_au(ts, sei_nal_h264(messages), 3100, cc);
    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 0);
    CHECK(st.valid == false);

    // SPS arrives with the next keyframe; from here the same SEI decodes.
    std::vector<uint8_t> ts2;
    push_video_au(ts2, concat({sps_nal_h264(), sei_nal_h264(messages)}), 6100, cc);
    push_video_au(ts2, sei_nal_h264(messages), 9100, cc);
    CHECK(sls_ts_scan_timecode(ts2.data(), (int)ts2.size(), &st) == 1);
    CHECK(formatted(st) == "09:08:07:06");
}

TEST_CASE("timecode: a stream whose SPS has no pic_struct never reports one")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x1B));

    std::vector<uint8_t> messages;
    append_sei_message(messages, 1, pic_timing_payload_h264(1, 1, 1, 1));
    std::vector<uint8_t> au = concat({sps_nal_h264(/*pic_struct_present=*/false), sei_nal_h264(messages)});

    uint8_t cc = 0;
    push_video_au(ts, au, 1, cc);
    push_video_au(ts, au, 3001, cc);

    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 0);
    CHECK(st.sps_valid);
    CHECK(st.pic_struct_present == false);
    CHECK(st.valid == false);
}

TEST_CASE("timecode: an audio-only program is left alone")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x0F)); // AAC, no video ES

    uint8_t cc = 0;
    push_video_au(ts, sei_nal_hevc({}), 1, cc);

    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 0);
    CHECK(st.video_pid == INVALID_PID);
    CHECK(st.codec == SLS_TC_CODEC_NONE);
}

TEST_CASE("timecode: hevc time_code is not read out of an h.264 stream")
{
    // Payload type 136 means something else entirely in H.264, so a scanner
    // that matches on the number alone invents timecodes for AVC streams.
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x1B));

    std::vector<uint8_t> messages;
    append_sei_message(messages, 136, time_code_payload_hevc(4, 5, 6, 7));
    std::vector<uint8_t> au = concat({sps_nal_h264(), sei_nal_h264(messages)});

    uint8_t cc = 0;
    push_video_au(ts, au, 1, cc);
    push_video_au(ts, au, 3001, cc);

    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 0);
    CHECK(st.valid == false);
}

TEST_CASE("timecode: successive access units keep updating the timecode")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x24));

    uint8_t cc = 0;
    for (int frame = 0; frame < 5; frame++)
    {
        std::vector<uint8_t> messages;
        append_sei_message(messages, 136, time_code_payload_hevc(0, 0, 1, frame));
        push_video_au(ts, sei_nal_hevc(messages), 90000 + frame * 3000, cc);
    }

    // Four access units are closed by the arrival of the next one; the fifth
    // is still open when the chunk ends.
    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 4);
    CHECK(st.updates == 4);
    CHECK(formatted(st) == "00:00:01:03");
    CHECK(st.pts == 90000 + 3 * 3000);
}

TEST_CASE("timecode: a stream split across chunks at arbitrary offsets still decodes")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x24));
    std::vector<uint8_t> messages;
    append_sei_message(messages, 136, time_code_payload_hevc(12, 34, 56, 7));
    uint8_t cc = 0;
    push_video_au(ts, sei_nal_hevc(messages), 42, cc);
    push_video_au(ts, sei_nal_hevc(messages), 3042, cc);

    // Feed it one packet at a time, the way a 1316-byte SRT payload arrives
    // relative to access-unit boundaries.
    int decoded = 0;
    for (size_t off = 0; off + TS_PACK_LEN <= ts.size(); off += TS_PACK_LEN)
        decoded += sls_ts_scan_timecode(ts.data() + off, TS_PACK_LEN, &st);

    CHECK(decoded == 1);
    CHECK(formatted(st) == "12:34:56:07");
}

TEST_CASE("timecode: truncated and malformed input is rejected, not parsed")
{
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x24));
    CHECK(sls_ts_scan_timecode(ts.data(), (int)ts.size(), &st) == 0);

    SUBCASE("short buffers")
    {
        std::vector<uint8_t> buf(TS_PACK_LEN, 0x47);
        for (int len : {0, 1, 4, 100, 187})
            CHECK(sls_ts_scan_timecode(buf.data(), len, &st) == 0);
        CHECK(sls_ts_scan_timecode(nullptr, 188, &st) == 0);
        CHECK(sls_ts_scan_timecode(buf.data(), 188, nullptr) == 0);
    }

    SUBCASE("a truncated SEI payload")
    {
        std::vector<uint8_t> messages;
        std::vector<uint8_t> payload = time_code_payload_hevc(1, 2, 3, 4);
        payload.resize(1); // declared size still fits, contents do not
        append_sei_message(messages, 136, payload);
        std::vector<uint8_t> chunk;
        uint8_t cc = 0;
        push_video_au(chunk, sei_nal_hevc(messages), 1, cc);
        push_video_au(chunk, sei_nal_hevc(messages), 3001, cc);
        CHECK(sls_ts_scan_timecode(chunk.data(), (int)chunk.size(), &st) == 0);
        CHECK(st.valid == false);
    }

    SUBCASE("an SEI whose declared payload size runs past the NAL")
    {
        std::vector<uint8_t> messages = {136, 200}; // size 200 in a ~10 byte NAL
        std::vector<uint8_t> tail = time_code_payload_hevc(1, 2, 3, 4);
        messages.insert(messages.end(), tail.begin(), tail.end());
        std::vector<uint8_t> chunk;
        uint8_t cc = 0;
        push_video_au(chunk, sei_nal_hevc(messages), 1, cc);
        push_video_au(chunk, sei_nal_hevc(messages), 3001, cc);
        CHECK(sls_ts_scan_timecode(chunk.data(), (int)chunk.size(), &st) == 0);
    }

    SUBCASE("out-of-range field values are not published")
    {
        BitWriter w;
        w.u(2, 1);
        w.u(1, 1);
        w.u(1, 0);
        w.u(5, 0);
        w.u(1, 1);
        w.u(1, 0);
        w.u(1, 0);
        w.u(9, 3);
        w.u(6, 61); // seconds = 61
        w.u(6, 2);
        w.u(5, 1);
        w.u(5, 0);
        while (w.bit_count() % 8 != 0)
            w.u(1, 0);
        std::vector<uint8_t> messages;
        append_sei_message(messages, 136, w.bytes());
        std::vector<uint8_t> chunk;
        uint8_t cc = 0;
        push_video_au(chunk, sei_nal_hevc(messages), 1, cc);
        push_video_au(chunk, sei_nal_hevc(messages), 3001, cc);
        CHECK(sls_ts_scan_timecode(chunk.data(), (int)chunk.size(), &st) == 0);
        CHECK(st.valid == false);
    }
}

TEST_CASE("timecode: CSLSMapData only scans streams that opted in")
{
    std::vector<uint8_t> ts;
    push_packet(ts, pat_packet());
    push_packet(ts, pmt_packet(0x24));
    std::vector<uint8_t> messages;
    append_sei_message(messages, 136, time_code_payload_hevc(3, 4, 5, 6));
    uint8_t cc = 0;
    push_video_au(ts, sei_nal_hevc(messages), 4242, cc);
    push_video_au(ts, sei_nal_hevc(messages), 7242, cc);

    SUBCASE("off by default")
    {
        CSLSMapData m;
        m.set_caps(0, 0);
        char key[] = "app/quiet";
        REQUIRE(m.add(key) == SLS_OK);
        REQUIRE(m.put(key, (char *)ts.data(), (int)ts.size()) >= 0);

        CSLSMapData::TimecodeStats stats;
        CHECK(m.get_timecode_stats(key, stats) == false);
        CHECK(stats.enabled == false);
        CHECK(stats.valid == false);
    }

    SUBCASE("enabled")
    {
        CSLSMapData m;
        m.set_caps(0, 0);
        char key[] = "app/timed";
        REQUIRE(m.add(key) == SLS_OK);
        m.set_timecode_scan(key, true);
        REQUIRE(m.put(key, (char *)ts.data(), (int)ts.size()) >= 0);

        CSLSMapData::TimecodeStats stats;
        REQUIRE(m.get_timecode_stats(key, stats));
        CHECK(stats.enabled);
        CHECK(stats.valid);
        CHECK(stats.codec == SLS_TC_CODEC_HEVC);
        CHECK(stats.timecode == "03:04:05:06");
        CHECK(stats.pts == 4242);
        CHECK(stats.updates == 1);

        // `clear` resets the per-interval counter, not the timecode itself.
        CSLSMapData::TimecodeStats after;
        REQUIRE(m.get_timecode_stats(key, after, 1));
        CHECK(after.updates == 1);
        REQUIRE(m.get_timecode_stats(key, after));
        CHECK(after.updates == 0);
        CHECK(after.timecode == "03:04:05:06");

        // Removing the stream frees the scanner with it.
        REQUIRE(m.remove(key) == SLS_OK);
        CHECK(m.get_timecode_stats(key, after) == false);
    }
}

TEST_CASE("timecode: random bytes never produce a timecode or an overrun")
{
    // Deterministic pseudo-random garbage shaped like TS packets. The assertion
    // that matters runs under -DSLS_SANITIZE=ON: no read may leave a buffer.
    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);

    uint32_t seed = 0x12345678;
    auto next = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return (uint8_t)((seed >> 16) & 0xFF);
    };

    for (int round = 0; round < 200; round++)
    {
        std::vector<uint8_t> chunk(TS_PACK_LEN * 7);
        for (size_t i = 0; i < chunk.size(); i++)
            chunk[i] = next();
        for (size_t i = 0; i < chunk.size(); i += TS_PACK_LEN)
            chunk[i] = 0x47; // keep them looking like packets
        sls_ts_scan_timecode(chunk.data(), (int)chunk.size(), &st);
    }
    CHECK(true);
}
