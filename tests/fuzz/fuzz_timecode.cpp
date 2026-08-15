// libFuzzer target for the in-band SMPTE timecode scanner (SLSTimecode.cpp).
//
// The scanner runs on the publisher ingest path over attacker-supplied bytes and
// walks four nested layers of length fields it does not control: TS packets,
// PSI sections (PAT/PMT), the PES header, and then Annex-B NALs whose SEI
// messages and parameter sets are bit-coded. Every one of those is a chance to
// read past the buffer on a crafted stream.
//
// The harness feeds the raw, exact-sized libFuzzer buffer straight into
// sls_ts_scan_timecode. libFuzzer poisons the bytes after data[size-1], so any
// unbounded read trips AddressSanitizer. It adds NO parsing logic of its own.
//
// State is carried across calls within one input (the scanner learns the
// program map from earlier packets and the SPS from earlier access units), so
// feeding the buffer in varying-size slices also exercises the accumulator
// across chunk boundaries — the path the real ingest takes.
#include <climits>
#include <cstddef>
#include <cstdint>

#include "SLSTimecode.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > static_cast<size_t>(INT_MAX))
        return 0;

    ts_timecode_state st;
    sls_init_ts_timecode_state(&st);
    sls_ts_scan_timecode(data, static_cast<int>(size), &st);

    char buf[16];
    sls_format_timecode(&st, buf, sizeof(buf));

    // Same bytes again, but split into chunks whose size is driven by the input
    // itself, so access units that straddle a chunk boundary are covered.
    ts_timecode_state chunked;
    sls_init_ts_timecode_state(&chunked);
    size_t offset = 0;
    while (offset < size)
    {
        size_t chunk = 1 + (data[offset] % 8) * 188;
        if (chunk > size - offset)
            chunk = size - offset;
        sls_ts_scan_timecode(data + offset, static_cast<int>(chunk), &chunked);
        offset += chunk;
    }

    return 0;
}
