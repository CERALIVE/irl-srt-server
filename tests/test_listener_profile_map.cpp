#include "doctest.h"

#include <string>

#include "SLSListener.hpp"
#include "SLSSrt.hpp"

// Locks the CSLSManager directive->profile mapping at the listener level. Each
// listener carries exactly one SrtProfile, set once at creation (the
// create_for_spec call sites in SLSManager.cpp) and read back through the
// getter. A default-constructed, never-started listener is sufficient: no
// socket is bound, so these are pure tag-contract checks, independent of which
// libsrt the leg linked. The per-port startup-log half of the contract is
// asserted by the e2e (tests/e2e/srt_loopback.sh).

TEST_CASE("CSLSListener default receive-profile is L3Direct")
{
    CSLSListener l;
    CHECK(l.get_srt_profile() == SrtProfile::L3Direct);
}

TEST_CASE("CSLSListener set_srt_profile round-trips through get_srt_profile")
{
    const SrtProfile profiles[] = {
        SrtProfile::L1FreezeNak,
        SrtProfile::L2Classic,
        SrtProfile::L3Direct,
    };
    for (SrtProfile p : profiles)
    {
        CSLSListener l;
        l.set_srt_profile(p);
        CHECK(l.get_srt_profile() == p);
    }
}

TEST_CASE("listener directive -> SrtProfile mapping mirrors CSLSManager create_for_spec")
{
    struct DirectiveCase
    {
        const char *directive;
        SrtProfile profile;
        const char *tag;
    };
    const DirectiveCase cases[] = {
        {"listen_publisher", SrtProfile::L3Direct, "L3-direct"},
        {"listen_publisher_srtla", SrtProfile::L1FreezeNak, "L1-freeze-nak"},
        {"listen_publisher_srtla_classic", SrtProfile::L2Classic, "L2-classic"},
        {"listen_player", SrtProfile::L3Direct, "L3-direct"},
    };
    for (const auto &c : cases)
    {
        INFO("directive=" << c.directive);
        CSLSListener l;
        l.set_srt_profile(c.profile);
        CHECK(l.get_srt_profile() == c.profile);
        CHECK(std::string(sls_srt_profile_spec(l.get_srt_profile()).name) == c.tag);
    }
}
