#include "doctest.h"
#include "SLSSrt.hpp"
#include "common.hpp"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/spdlog.h"
#include <cstdlib>
#include <iostream>
#include <tuple>

#if SLS_TEST_WRAP_GATE && SLS_HAVE_SRTO_PERIODICNAKGATE
static bool fail_gate = false;
static int gate_calls = 0;
extern "C" int __real_srt_setsockflag(SRTSOCKET, SRT_SOCKOPT, const void *, int);
extern "C" int __wrap_srt_setsockflag(SRTSOCKET fd, SRT_SOCKOPT opt, const void *value, int len)
{
    if (opt == SRTO_PERIODICNAKGATE)
    {
        ++gate_calls;
        if (fail_gate)
            return SRT_ERROR;
    }
    return __real_srt_setsockflag(fd, opt, value, len);
}
#endif

namespace {
const std::string startup_override = [] {
    const char *value = std::getenv("SLS_BONDED_PROFILE_OVERRIDE");
    return value ? value : "converged";
}();

auto policy(const SrtProfileSpec &spec)
{
    return std::make_tuple(spec.freeze, spec.set_nakreport, spec.nakreport, spec.lossmaxttl,
                           spec.rcvlatency_floor_ms, spec.fec_accept, spec.periodic_nak_gate);
}

struct Probe
{
    CSLSSrt socket;
    std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> logs =
        std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256);
    Probe()
    {
        REQUIRE(srt_startup() == 0);
        socket.libsrt_set_latency(200);
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("profiles", logs));
    }
    ~Probe()
    {
        socket.libsrt_close();
        spdlog::set_default_logger(previous);
        srt_cleanup();
    }
    bool logged(const std::string &text) const
    {
        for (const auto &line : logs->last_formatted())
            if (line.find(text) != std::string::npos)
                return true;
        return false;
    }
    template <typename T> T option(SRT_SOCKOPT opt)
    {
        T value{};
        int len = sizeof(value);
        REQUIRE(srt_getsockflag(socket.libsrt_get_fd(), opt, &value, &len) == 0);
        return value;
    }
};

constexpr const char *gate_error = "bonded profile requires SRTO_PERIODICNAKGATE (libsrt >= "
                                   "1.5.7+ceralive.1); refusing to start listener";
}

TEST_CASE("bonded policy structs are equal when either listener alias is selected")
{
    // Given two distinct listener identities; When resolving; Then every policy field agrees.
    const auto &l1 = sls_srt_profile_spec(SrtProfile::L1FreezeNak);
    const auto &l2 = sls_srt_profile_spec(SrtProfile::L2Classic);
    CHECK(policy(l1) == policy(l2));
    CHECK(std::string(l1.name) == "L1-bonded");
    CHECK(std::string(l2.name) == "L2-bonded-alias");
}

TEST_CASE("bonded override selects the literal policy when read at startup")
{
    // Given a fresh process environment; When resolving; Then compare to independent literals.
    auto expected = std::make_tuple(true, true, true, 200, 100, true, true);
    if (startup_override == "legacy-l1")
        expected = std::make_tuple(true, true, true, 40, 100, true, false);
    else if (startup_override == "legacy-l2")
        expected = std::make_tuple(true, true, false, 40, 100, false, false);
    CHECK(policy(sls_srt_profile_spec(SrtProfile::L1FreezeNak)) == expected);
    CHECK(policy(sls_srt_profile_spec(SrtProfile::L2Classic)) == expected);
}

TEST_CASE("direct policy stays frozen when any bonded override is selected")
{
    // Given the original L3 literal; When resolving; Then all fields and its name stay unchanged.
    const SrtProfileSpec frozen{"L3-direct", false, false, false, 200, 0, false, false};
    const auto &actual = sls_srt_profile_spec(SrtProfile::L3Direct);
    CHECK(policy(actual) == policy(frozen));
    CHECK(std::string(actual.name) == frozen.name);
    CHECK(policy(sls_srt_profile_spec(static_cast<SrtProfile>(99))) == policy(frozen));
}

TEST_CASE("bonded override remains fixed when environment changes after first resolution")
{
    // Given a resolved startup policy; When the environment changes; Then both aliases retain it.
    const auto before = policy(sls_srt_profile_spec(SrtProfile::L1FreezeNak));
    REQUIRE(setenv("SLS_BONDED_PROFILE_OVERRIDE", startup_override == "legacy-l2" ? "legacy-l1" : "legacy-l2", 1) == 0);
    CHECK(policy(sls_srt_profile_spec(SrtProfile::L1FreezeNak)) == before);
    CHECK(policy(sls_srt_profile_spec(SrtProfile::L2Classic)) == before);
    REQUIRE(setenv("SLS_BONDED_PROFILE_OVERRIDE", startup_override.c_str(), 1) == 0);
}

TEST_CASE("listener socket options match the policy when setup succeeds or explicitly refuses")
{
    for (auto profile : {SrtProfile::L1FreezeNak, SrtProfile::L2Classic, SrtProfile::L3Direct})
    {
        // Given a real ephemeral socket; When setup runs; Then assert refusal or the full option set.
        Probe probe;
        const auto &spec = sls_srt_profile_spec(profile);
        const int result = probe.socket.libsrt_setup(0, profile);
#if !SLS_HAVE_SRTO_PERIODICNAKGATE
        if (spec.periodic_nak_gate)
        {
            CHECK(result == SLS_ERROR);
            CHECK(probe.logged(gate_error));
            CHECK(probe.logged("[error]"));
            CHECK(probe.socket.libsrt_get_fd() == 0);
            std::cout << "SKIP: no SRTO_PERIODICNAKGATE: " << spec.name << " success path; refusal asserted\n";
            continue;
        }
#endif
        REQUIRE(result == SLS_OK);
        CHECK(probe.option<int>(SRTO_LOSSMAXTTL) == spec.lossmaxttl);
        CHECK(probe.option<int>(SRTO_RCVLATENCY) == (spec.rcvlatency_floor_ms ? spec.rcvlatency_floor_ms : 200));
        CHECK(probe.option<int>(SRTO_PEERLATENCY) == 200);
        CHECK(probe.option<bool>(SRTO_TLPKTDROP));
        CHECK(probe.option<int>(SRTO_FC) == 8 * 1024);
        CHECK(probe.option<int>(SRTO_RCVBUF) > 4 * 1024 * 1024);
        CHECK(probe.option<int>(SRTO_RCVBUF) <= 8 * 1024 * 1024);
#if !defined(SLS_HAVE_SRTO_SRTLAPATCHES)
        CHECK(probe.option<bool>(SRTO_NAKREPORT) == (spec.set_nakreport ? spec.nakreport : true));
#endif
#if defined(SLS_HAVE_SRTO_REORDERFREEZE)
        CHECK(probe.option<bool>(SRTO_REORDERFREEZE) == spec.freeze);
#endif
#if SLS_HAVE_SRTO_PERIODICNAKGATE
        CHECK(probe.option<bool>(SRTO_PERIODICNAKGATE) == spec.periodic_nak_gate);
#endif
        char filter[512]{};
        int len = sizeof(filter);
        REQUIRE(srt_getsockflag(probe.socket.libsrt_get_fd(), SRTO_PACKETFILTER, filter, &len) == 0);
        CHECK((len > 0) == spec.fec_accept);
        if (spec.fec_accept)
            CHECK(std::string(filter, len).find("fec") != std::string::npos);
        CHECK(probe.logged(std::string("profile=") + spec.name));
        if (spec.periodic_nak_gate)
            CHECK(probe.logged("freeze=1 nakreport=1 periodic_nak_gate=1 lossmaxttl=200 floor=100 fec_accept=1"));
    }
}

TEST_CASE("bonded setup refuses when the runtime rejects the gate")
{
#if SLS_TEST_WRAP_GATE && SLS_HAVE_SRTO_PERIODICNAKGATE
    // Given only the gate syscall fails; When setting up either alias; Then fail closed, without binding.
    for (auto profile : {SrtProfile::L1FreezeNak, SrtProfile::L2Classic})
    {
        Probe probe;
        fail_gate = true;
        gate_calls = 0;
        const int result = probe.socket.libsrt_setup(0, profile);
        fail_gate = false;
        if (sls_srt_profile_spec(profile).periodic_nak_gate)
        {
            CHECK(gate_calls == 1);
            CHECK(result == SLS_ERROR);
            CHECK(probe.logged(gate_error));
            CHECK(probe.logged("[error]"));
            CHECK(probe.socket.libsrt_get_fd() == 0);
        }
        else
        {
            CHECK(gate_calls == 0);
            CHECK(result == SLS_OK);
        }
    }
#else
    std::cout << "SKIP: no SRTO_PERIODICNAKGATE: runtime fault injection; compile-time refusal asserted separately\n";
#endif
}

TEST_CASE("direct listener still starts when the gate syscall would fail")
{
    // Given a gate-failing runtime; When setting up L3; Then its untouched listener starts.
    Probe probe;
#if SLS_TEST_WRAP_GATE && SLS_HAVE_SRTO_PERIODICNAKGATE
    fail_gate = true;
    gate_calls = 0;
#endif
    const int result = probe.socket.libsrt_setup(0, SrtProfile::L3Direct);
#if SLS_TEST_WRAP_GATE && SLS_HAVE_SRTO_PERIODICNAKGATE
    fail_gate = false;
    CHECK(gate_calls == 0);
#endif
    REQUIRE(result == SLS_OK);
    CHECK(probe.socket.libsrt_listen(1) == SLS_OK);
}
