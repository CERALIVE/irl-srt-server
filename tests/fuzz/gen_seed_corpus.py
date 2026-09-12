#!/usr/bin/env python3
"""Regenerate the libFuzzer seed corpora for the boundary parsers.

The seeds are committed under tests/fuzz/corpus/{ts,timecode,streamid,conf}/ so a short
`./fuzz_* -max_total_time=30 corpus/<x>/` run starts from real, valid inputs
(coverage-guided fuzzing converges far faster from a representative corpus than
from an empty one). This script is the reproducible source of those bytes.

The TS packets mirror the make_pat / make_pmt / make_audio_overrun builders in
tests/test_ts_parser.cpp byte-for-byte; the streamid and conf seeds mirror the
fixtures in tests/test_sls_sid.cpp and src/tests/test_conf_validation.cpp. Run
from anywhere: it writes relative to this file.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "corpus")

TS_PACK_LEN = 188
TS_SYNC_BYTE = 0x47


# ---- MPEG-TS packet builders (mirror tests/test_ts_parser.cpp) --------------
def make_packet(b1, b2, b3):
    pkt = bytearray(TS_PACK_LEN)
    pkt[0] = TS_SYNC_BYTE
    pkt[1] = b1
    pkt[2] = b2
    pkt[3] = b3
    return pkt


def make_pat(pmt_pid):
    pkt = make_packet(0x40, 0x00, 0x10)  # PUSI, PID 0, payload
    pkt[4] = 0x00
    pkt[5] = 0x00
    pkt[6] = 0xB0
    pkt[7] = 0x0D
    pkt[8] = 0x00
    pkt[9] = 0x01
    pkt[10] = 0xC1
    pkt[11] = 0x00
    pkt[12] = 0x00
    pkt[13] = 0x00
    pkt[14] = 0x01
    pkt[15] = 0xE0 | ((pmt_pid >> 8) & 0x1F)
    pkt[16] = pmt_pid & 0xFF
    return pkt


def make_pmt(pmt_pid, audio_pid):
    pkt = make_packet(0x40 | ((pmt_pid >> 8) & 0x1F), pmt_pid & 0xFF, 0x10)
    pkt[4] = 0x00
    pkt[5] = 0x02
    pkt[6] = 0xB0
    pkt[7] = 0x12
    pkt[8] = 0x00
    pkt[9] = 0x01
    pkt[10] = 0xC1
    pkt[11] = 0x00
    pkt[12] = 0x00
    pkt[13] = 0xE0
    pkt[14] = 0x00
    pkt[15] = 0xF0
    pkt[16] = 0x00
    pkt[17] = 0x0F  # stream_type = AAC (audio)
    pkt[18] = 0xE0 | ((audio_pid >> 8) & 0x1F)
    pkt[19] = audio_pid & 0xFF
    pkt[20] = 0xF0
    pkt[21] = 0x00
    return pkt


def make_audio_overrun(audio_pid):
    pkt = make_packet(0x40 | ((audio_pid >> 8) & 0x1F), audio_pid & 0xFF, 0x30)
    pkt[4] = 173  # adaptation_field_length -> pos = 178
    pos = 178
    pkt[pos + 0] = 0x00
    pkt[pos + 1] = 0x00
    pkt[pos + 2] = 0x01  # PES start code
    pkt[pos + 3] = 0xC0  # audio stream_id
    pkt[pos + 4] = 0x00
    pkt[pos + 5] = 0x00
    pkt[pos + 6] = 0x80
    pkt[pos + 7] = 0x80  # PTS present
    pkt[pos + 8] = 0x05
    pkt[pos + 9] = 0x21
    return pkt


def make_video_pes(es_pid):
    # A clean PES start packet (payload-only) that routes through sls_pes2es.
    pkt = make_packet(0x40 | ((es_pid >> 8) & 0x1F), es_pid & 0xFF, 0x10)
    pos = 4
    pkt[pos + 0] = 0x00
    pkt[pos + 1] = 0x00
    pkt[pos + 2] = 0x01  # PES start code
    pkt[pos + 3] = 0xE0  # video stream_id
    pkt[pos + 4] = 0x00
    pkt[pos + 5] = 0x00
    pkt[pos + 6] = 0x80
    pkt[pos + 7] = 0x80  # PTS present
    pkt[pos + 8] = 0x05
    pkt[pos + 9] = 0x21
    pkt[pos + 10] = 0x00
    pkt[pos + 11] = 0x01
    pkt[pos + 12] = 0x00
    pkt[pos + 13] = 0x01
    return pkt


def write(rel, data):
    path = os.path.join(CORPUS, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, (bytes, bytearray)) else data.encode())
    return path


def gen_ts():
    pat = make_pat(0x100)
    pmt = make_pmt(0x100, 0x101)
    write("ts/pat.bin", bytes(pat))
    write("ts/pmt.bin", bytes(pmt))
    write("ts/pat_pmt.bin", bytes(pat) + bytes(pmt))
    write("ts/video_pes.bin", bytes(make_video_pes(0x100)))
    # PAT + PMT + 4 null packets + the MEM-1 audio-overrun packet (the 7-packet
    # buffer shape from the check_audio_gap test).
    null_pkt = make_packet(0x1F, 0xFF, 0x00)
    multi = bytes(pat) + bytes(pmt)
    multi += bytes(null_pkt) * 4
    multi += bytes(make_audio_overrun(0x101))
    write("ts/pat_pmt_audio.bin", multi)


# ---- in-band timecode seeds (mirror tests/test_timecode.cpp) ----------------
#
# The timecode scanner walks TS -> PSI -> PES -> Annex-B NAL -> SEI, so a seed
# has to be a whole little transport stream for the fuzzer to reach the bit
# readers at the bottom. These build exactly the streams the unit tests assert
# on: an H.264 program whose SPS/VUI declares pic_struct (making pic_timing
# decodable) and an HEVC program carrying a time_code SEI.
class BitWriter:
    def __init__(self):
        self.bits = []

    def u(self, n, value):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def ue(self, value):
        code = value + 1
        n = code.bit_length()
        self.bits.extend([0] * (n - 1))
        self.u(n, code)

    def rbsp_trailing(self):
        self.bits.append(1)
        while len(self.bits) % 8:
            self.bits.append(0)

    def bytes(self):
        while len(self.bits) % 8:
            self.bits.append(0)
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            byte = 0
            for bit in self.bits[i:i + 8]:
                byte = (byte << 1) | bit
            out.append(byte)
        return bytes(out)


def sei_message(payload_type, payload):
    """sei_message(): byte-coded payloadType / payloadSize (0xFF chains)."""
    out = bytearray()
    t = payload_type
    while t >= 255:
        out.append(0xFF)
        t -= 255
    out.append(t)
    n = len(payload)
    while n >= 255:
        out.append(0xFF)
        n -= 255
    out.append(n)
    out += payload
    return bytes(out)


def escape_rbsp(rbsp):
    out = bytearray()
    zeros = 0
    for c in rbsp:
        if zeros >= 2 and c <= 0x03:
            out.append(0x03)
            zeros = 0
        out.append(c)
        zeros = zeros + 1 if c == 0x00 else 0
    return bytes(out)


def nal(header, rbsp):
    return b"\x00\x00\x00\x01" + bytes(header) + escape_rbsp(bytes(rbsp) + b"\x80")


def pic_timing_h264(h, m, s, f):
    w = BitWriter()
    w.u(4, 0)  # pic_struct = 0 -> NumClockTS 1
    w.u(1, 1)  # clock_timestamp_flag
    w.u(2, 0)  # ct_type
    w.u(1, 0)  # nuit_field_based_flag
    w.u(5, 0)  # counting_type
    w.u(1, 1)  # full_timestamp_flag
    w.u(1, 0)  # discontinuity_flag
    w.u(1, 0)  # cnt_dropped_flag
    w.u(8, f)  # n_frames
    w.u(6, s)
    w.u(6, m)
    w.u(5, h)
    w.u(24, 0)  # time_offset, i(24)
    return w.bytes()


def time_code_hevc(h, m, s, f):
    w = BitWriter()
    w.u(2, 1)  # num_clock_ts
    w.u(1, 1)  # clock_timestamp_flag
    w.u(1, 0)  # units_field_based_flag
    w.u(5, 0)  # counting_type
    w.u(1, 1)  # full_timestamp_flag
    w.u(1, 0)  # discontinuity_flag
    w.u(1, 0)  # cnt_dropped_flag
    w.u(9, f)  # n_frames
    w.u(6, s)
    w.u(6, m)
    w.u(5, h)
    w.u(5, 0)  # time_offset_length
    return w.bytes()


def sps_h264():
    w = BitWriter()
    w.u(8, 66)  # profile_idc (baseline)
    w.u(8, 0)
    w.u(8, 30)  # level_idc
    w.ue(0)     # seq_parameter_set_id
    w.ue(0)     # log2_max_frame_num_minus4
    w.ue(0)     # pic_order_cnt_type
    w.ue(0)     # log2_max_pic_order_cnt_lsb_minus4
    w.ue(1)     # max_num_ref_frames
    w.u(1, 0)   # gaps_in_frame_num_value_allowed_flag
    w.ue(39)    # pic_width_in_mbs_minus1
    w.ue(22)    # pic_height_in_map_units_minus1
    w.u(1, 1)   # frame_mbs_only_flag
    w.u(1, 1)   # direct_8x8_inference_flag
    w.u(1, 0)   # frame_cropping_flag
    w.u(1, 1)   # vui_parameters_present_flag
    for _ in range(7):
        w.u(1, 0)  # aspect/overscan/video-signal/chroma-loc/timing/nal-hrd/vcl-hrd
    w.u(1, 1)   # pic_struct_present_flag
    w.u(1, 0)   # bitstream_restriction_flag
    w.rbsp_trailing()
    return nal([0x67], w.bytes())


def psi_packet(pid, section):
    pkt = bytearray([0x47, 0x40 | ((pid >> 8) & 0x1F), pid & 0xFF, 0x10, 0x00])
    pkt += section
    pkt += b"\xFF" * (TS_PACK_LEN - len(pkt))
    return bytes(pkt)


def finish_section(section):
    body = len(section) - 3 + 4
    section[1] = 0xB0 | ((body >> 8) & 0x0F)
    section[2] = body & 0xFF
    section += b"\xDE\xAD\xBE\xEF"  # CRC_32 (not verified by the scanner)
    return section


def tc_pat(pmt_pid):
    s = bytearray([0x00, 0x00, 0x00, 0x00, 0x01, 0xC1, 0x00, 0x00,
                   0x00, 0x01, 0xE0 | ((pmt_pid >> 8) & 0x1F), pmt_pid & 0xFF])
    return psi_packet(0x00, finish_section(s))


def tc_pmt(pmt_pid, stream_type, video_pid):
    s = bytearray([0x02, 0x00, 0x00, 0x00, 0x01, 0xC1, 0x00, 0x00,
                   0xE0, 0x00, 0xF0, 0x00,
                   stream_type, 0xE0 | ((video_pid >> 8) & 0x1F), video_pid & 0xFF, 0xF0, 0x00])
    return psi_packet(pmt_pid, finish_section(s))


def video_au_packets(video_pid, au, pts, cc):
    pes = bytearray([0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x80, 0x80, 0x05])
    pes.append(0x21 | (((pts >> 30) & 0x07) << 1))
    pes.append((pts >> 22) & 0xFF)
    pes.append(0x01 | (((pts >> 15) & 0x7F) << 1))
    pes.append((pts >> 7) & 0xFF)
    pes.append(0x01 | ((pts & 0x7F) << 1))
    pes += au

    out = bytearray()
    offset = 0
    first = True
    while offset < len(pes):
        pkt = bytearray([0x47,
                         (0x40 if first else 0x00) | ((video_pid >> 8) & 0x1F),
                         video_pid & 0xFF,
                         0x10 | (cc & 0x0F)])
        cc = (cc + 1) & 0x0F
        take = min(TS_PACK_LEN - 4, len(pes) - offset)
        pkt += pes[offset:offset + take]
        offset += take
        first = False
        pkt += b"\xFF" * (TS_PACK_LEN - len(pkt))
        out += pkt
    return bytes(out), cc


def gen_timecode():
    pmt_pid, video_pid = 0x1000, 0x0100

    h264_au = sps_h264() + nal([0x06], sei_message(1, pic_timing_h264(1, 2, 3, 4))) + nal([0x65], b"\x88\x84\x00")
    stream, cc = video_au_packets(video_pid, h264_au, 90000, 0)
    stream2, _ = video_au_packets(video_pid, h264_au, 93000, cc)
    write("timecode/h264_pic_timing.bin",
          tc_pat(pmt_pid) + tc_pmt(pmt_pid, 0x1B, video_pid) + stream + stream2)

    hevc_au = nal([0x4E, 0x01], sei_message(136, time_code_hevc(23, 59, 58, 29))) + nal([0x26, 0x01], b"\xAF\x00\x01")
    stream, cc = video_au_packets(video_pid, hevc_au, 123456, 0)
    stream2, _ = video_au_packets(video_pid, hevc_au, 126456, cc)
    write("timecode/hevc_time_code.bin",
          tc_pat(pmt_pid) + tc_pmt(pmt_pid, 0x24, video_pid) + stream + stream2)

    # Program tables only: the scanner learns the map but never sees a payload.
    write("timecode/tables_only.bin", tc_pat(pmt_pid) + tc_pmt(pmt_pid, 0x1B, video_pid))


def gen_streamid():
    seeds = {
        "valid_std": "#!::h=example.com,sls_app=live,r=feed1",
        "valid_bare": "example.com/live/feed1",
        "valid_dotnames": "example.com/live/Feed.01",
        "whitespace": "#!::h= example.com , sls_app= live , r= feed1 ",
        "reordered": "#!::sls_app=live,r=feed1,h=example.com",
        "traversal_std": "#!::h=..,sls_app=live,r=feed1",
        "traversal_bare": "../../etc/passwd",
        "url_inject": "example.com/live/feed1?evil=1",
        "amp_inject": "example.com/live/feed&1",
        "missing_key": "#!::h=example.com,sls_app=live",
        "not_a_streamid": "not-a-streamid",
        "control_byte": b"#!::h=example.com,sls_app=li\x01ve,r=feed1",
        "tab_byte": b"#!::h=example.com,sls_app=li\tve,r=feed1",
        "empty": b"",
    }
    for name, val in seeds.items():
        write("streamid/" + name, val)


def gen_conf():
    seeds = {
        "single": "4000",
        "list": "4000,4010,5000-5005",
        "range": "5000-5010",
        "single_range": "5005-5005",
        "dedupe": "5000-5002,5001",
        "whitespace": " 4000 , 4001 ",
        "reversed_range": "5005-5000",
        "trailing_comma": "4000,4001,",
        "port_zero": "0",
        "port_overflow": "65536",
        "non_numeric": "abc",
        "dash_only": "40-",
        "leading_dash": "-40",
        "bool_true": "true",
        "bool_false": "false",
        "int_val": "12345",
        "double_val": "3.14",
        "quoted": '"hello world"',
        "upstreams": "a:1 b:2 c:3",
        "empty": "",
    }
    for name, val in seeds.items():
        write("conf/" + name, val)


def main():
    gen_ts()
    gen_timecode()
    gen_streamid()
    gen_conf()
    total = sum(len(files) for _, _, files in os.walk(CORPUS))
    print("wrote %d seed files under %s" % (total, CORPUS))


if __name__ == "__main__":
    main()
