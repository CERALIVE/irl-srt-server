#include "doctest.h"

#include <srt/srt.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/ringbuffer_sink.h"

#include "common.hpp"
#include "SLSSrt.hpp"

// Static receive-profile contract. A profile is realized by WHICH listener a
// stream lands on; libsrt_setup applies that profile's fixed option set to the
// listening socket. These tests bind a real loopback SRT listener per profile,
// read the options back with srt_getsockflag, and confirm the emitted startup
// "SRT profile:" log line — proving L1 = freeze+NAK, L2 = freeze+NAK-off,
// L3 = neither, with no per-connection mutation.

namespace {

// RAII libsrt lifetime, scoped to a single TEST_CASE. srt_cleanup() must run
// before process teardown or the SRT GC thread races destroyed globals and the
// test binary segfaults at exit; bracketing it here keeps that fully contained.
struct SrtRuntime {
    SrtRuntime() { srt_startup(); }
    ~SrtRuntime() { srt_cleanup(); }
};

struct ProfileProbe {
    int setup_ret = SLS_ERROR;
    bool nakreport = false;
    int lossmaxttl = -1;
    int rcvlatency = -1;
    int peerlatency = -1;
    bool reorderfreeze = false;
    bool tlpktdrop = false;
    int fc = -1;
    int rcvbuf = -1;
    std::string packetfilter;
    std::vector<std::string> log_lines;
};

bool log_contains(const std::vector<std::string> &lines, const std::string &needle)
{
    for (const auto &l : lines) {
        if (l.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// Bind one listener with `profile` on `port`, capture its startup log, and read
// the negotiated socket options back. ctx.latency is 200ms so the L1/L2 100ms
// RCVLATENCY floor must override latency_min for the assertion to hold. NAK and
// reorder-freeze are bool options: srt_getsockflag writes a single byte, so they
// are read into bool, not int.
ProfileProbe probe_profile(SrtProfile profile, int port)
{
    ProfileProbe p;

    SRTContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.latency = 200;
    ctx.reuse = 1;

    CSLSSrt srt;
    srt.libsrt_set_context(&ctx);

    auto ring = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256);
    auto logger = std::make_shared<spdlog::logger>("srtprofiletest", ring);
    auto prev = spdlog::default_logger();
    spdlog::set_default_logger(logger);

    p.setup_ret = srt.libsrt_setup(port, profile);

    p.log_lines = ring->last_formatted();
    spdlog::set_default_logger(prev);

    if (p.setup_ret != SLS_OK)
        return p;

    int fd = srt.libsrt_get_fd();
    int len;
    len = sizeof(p.nakreport);
    srt_getsockflag(fd, SRTO_NAKREPORT, &p.nakreport, &len);
    len = sizeof(p.lossmaxttl);
    srt_getsockflag(fd, SRTO_LOSSMAXTTL, &p.lossmaxttl, &len);
    len = sizeof(p.rcvlatency);
    srt_getsockflag(fd, SRTO_RCVLATENCY, &p.rcvlatency, &len);
#if defined(SLS_HAVE_SRTO_REORDERFREEZE)
    len = sizeof(p.reorderfreeze);
    srt_getsockflag(fd, SRTO_REORDERFREEZE, &p.reorderfreeze, &len);
#endif
    len = sizeof(p.peerlatency);
    srt_getsockflag(fd, SRTO_PEERLATENCY, &p.peerlatency, &len);
    len = sizeof(p.tlpktdrop);
    srt_getsockflag(fd, SRTO_TLPKTDROP, &p.tlpktdrop, &len);
    len = sizeof(p.fc);
    srt_getsockflag(fd, SRTO_FC, &p.fc, &len);
    len = sizeof(p.rcvbuf);
    srt_getsockflag(fd, SRTO_RCVBUF, &p.rcvbuf, &len);

    // SRTO_PACKETFILTER is a string flag: getOpt copies the configured filter
    // into the buffer and reports its length, leaving it unterminated. Read it
    // into a generous zeroed buffer and bound the std::string by the returned
    // length. Non-empty == fec-accept set (L1); empty == filter-free (L2/L3).
    char pf_buf[512];
    memset(pf_buf, 0, sizeof(pf_buf));
    int pf_len = sizeof(pf_buf);
    if (srt_getsockflag(fd, SRTO_PACKETFILTER, pf_buf, &pf_len) == 0 && pf_len > 0)
        p.packetfilter.assign(pf_buf, static_cast<size_t>(pf_len));

    srt.libsrt_close();
    return p;
}

// Options libsrt_setup applies to every listener regardless of profile/libsrt.
// FC=8192 pkts and RCVBUF~8 MB are the flood caps (8 MB scale, no loaded conf),
// replacing the old 100 MB/128000-pkt sizing. PEERLATENCY stays at latency_min
// (200) — the L1/L2 floor lowers RCVLATENCY but never the peer commitment.
void check_universal_opts(const ProfileProbe &p)
{
    CHECK(p.tlpktdrop == true);
    CHECK(p.fc == 8 * 1024);
    CHECK(p.rcvbuf > 4 * 1024 * 1024);
    CHECK(p.rcvbuf <= 8 * 1024 * 1024);
    CHECK(p.peerlatency == 200);
}

} // namespace

TEST_CASE("SrtProfile table: names match the documented profile tags")
{
    CHECK(std::string(sls_srt_profile_spec(SrtProfile::L1FreezeNak).name) == "L1-freeze-nak");
    CHECK(std::string(sls_srt_profile_spec(SrtProfile::L2Classic).name) == "L2-classic");
    CHECK(std::string(sls_srt_profile_spec(SrtProfile::L3Direct).name) == "L3-direct");
}

TEST_CASE("SRT receive profiles: L1 freeze+NAK, L2 freeze+NAK-off, L3 neither")
{
    SrtRuntime srt_rt;

    SUBCASE("L1: reorder-freeze ON, periodic NAK ON, 100ms rcv floor, LOSSMAXTTL=40")
    {
        ProfileProbe p = probe_profile(SrtProfile::L1FreezeNak, 41001);
        REQUIRE(p.setup_ret == SLS_OK);

        CHECK(p.lossmaxttl == 40); // Task 1 A/B winner (BellaBox parity tie-break)
        CHECK(p.rcvlatency == 100); // 100ms floor wins over the 200ms latency_min

        // NAK is set explicitly on every build except the belabox fork, where
        // SRTO_SRTLAPATCHES fuses NAK-off and would override an L1 NAK-on.
#if !defined(SLS_HAVE_SRTO_SRTLAPATCHES)
        CHECK(p.nakreport == true);
#endif
        // Freeze is directly observable only on CERALIVE/srt (canonical build).
#if defined(SLS_HAVE_SRTO_REORDERFREEZE)
        CHECK(p.reorderfreeze == true);
#endif
        // FEC rides L1: the listener carries the fec-accept filter, so a FEC
        // device negotiates it while a non-FEC caller connects plain.
        CHECK_FALSE(p.packetfilter.empty());
        CHECK(p.packetfilter.find("fec") != std::string::npos);
        CHECK(log_contains(p.log_lines, "SRT profile: L1-freeze-nak"));
        CHECK(log_contains(p.log_lines, "FEC-accept"));
        check_universal_opts(p);
    }

    SUBCASE("L2: reorder-freeze ON, periodic NAK OFF (Classic)")
    {
        ProfileProbe p = probe_profile(SrtProfile::L2Classic, 41002);
        REQUIRE(p.setup_ret == SLS_OK);

        CHECK(p.lossmaxttl == 40); // Task 1 A/B winner (BellaBox parity tie-break)
        CHECK(p.rcvlatency == 100);
        // NAK-off holds on every build (the belabox fork also forces it off).
        CHECK(p.nakreport == false);
#if defined(SLS_HAVE_SRTO_REORDERFREEZE)
        CHECK(p.reorderfreeze == true);
#endif
        CHECK(p.packetfilter.empty()); // L2 is filter-free; FEC rides L1 only
        CHECK(log_contains(p.log_lines, "SRT profile: L2-classic"));
        check_universal_opts(p);
    }

    SUBCASE("L3: neither freeze nor NAK override (stock adaptive direct SRT)")
    {
        ProfileProbe p = probe_profile(SrtProfile::L3Direct, 41003);
        REQUIRE(p.setup_ret == SLS_OK);

        CHECK(p.lossmaxttl == 200); // baseline reorder tolerance, not the L1/L2 cap
        CHECK(p.rcvlatency != 100); // no profile floor; keeps latency_min/default
        CHECK(p.nakreport == true); // libsrt live default (NAK on) left in place
#if defined(SLS_HAVE_SRTO_REORDERFREEZE)
        CHECK(p.reorderfreeze == false);
#endif
        CHECK(p.packetfilter.empty()); // L3 is filter-free; FEC rides L1 only
        CHECK(log_contains(p.log_lines, "SRT profile: L3-direct"));
        check_universal_opts(p);
    }
}

TEST_CASE("SRT profiles: full option set per profile, derived from kSrtProfileTable")
{
    SrtRuntime srt_rt;

    struct ProfilePort
    {
        SrtProfile profile;
        int port;
    };
    const ProfilePort entries[] = {
        {SrtProfile::L1FreezeNak, 41011},
        {SrtProfile::L2Classic, 41012},
        {SrtProfile::L3Direct, 41013},
    };

    for (const auto &e : entries)
    {
        const SrtProfileSpec &spec = sls_srt_profile_spec(e.profile);
        INFO("profile=" << spec.name);
        ProfileProbe p = probe_profile(e.profile, e.port);
        REQUIRE(p.setup_ret == SLS_OK);

        CHECK(p.lossmaxttl == spec.lossmaxttl);

        if (spec.rcvlatency_floor_ms > 0)
            CHECK(p.rcvlatency == spec.rcvlatency_floor_ms);
        else
            CHECK(p.rcvlatency == 200); // no floor: keeps ctx.latency (latency_min)

        CHECK(p.packetfilter.empty() == !spec.fec_accept);
        if (spec.fec_accept)
            CHECK(p.packetfilter.find("fec") != std::string::npos);

        // NAK is set per profile on every build except the belabox fork, where
        // SRTO_SRTLAPATCHES fuses NAK-off and overrides the per-profile choice.
#if !defined(SLS_HAVE_SRTO_SRTLAPATCHES)
        if (spec.set_nakreport)
            CHECK(p.nakreport == spec.nakreport);
        else
            CHECK(p.nakreport == true); // libsrt live default left in place
#endif
        // Freeze is directly observable only on CERALIVE/srt (canonical build).
#if defined(SLS_HAVE_SRTO_REORDERFREEZE)
        CHECK(p.reorderfreeze == spec.freeze);
#endif
        check_universal_opts(p);
        CHECK(log_contains(p.log_lines, std::string("SRT profile: ") + spec.name));
    }
}
