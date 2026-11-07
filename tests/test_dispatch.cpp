#include "doctest.h"

#include <memory>
#include <string>

#include "SLSGroup.hpp"
#include "SLSPlayer.hpp"
#include "SLSRoleList.hpp"

namespace
{

// A role that never touches a socket; it reports whichever capacity class the
// test asks for.
class FakeRole final : public CSLSRole
{
public:
    FakeRole(int id_, bool unreserved_) : id(id_), unreserved(unreserved_) {}
    bool uses_unreserved_capacity() const override
    {
        return unreserved;
    }
    int id;
    bool unreserved;
};

// Stands in for a player a reload handed off: its teardown waits for a
// verdict that the given number of worker ticks brings.
class DeferringRole final : public CSLSRole
{
public:
    explicit DeferringRole(int ticks, int64_t start_ms = 0) : ticks_left(ticks)
    {
        m_stat_start_time = start_ms; // 0: long past the backlog TTL
    }
    bool defers_teardown() const override
    {
        return ticks_left > 0;
    }
    void on_worker_tick() override
    {
        if (ticks_left > 0)
            ticks_left--;
    }
    int uninit() override
    {
        uninits++;
        return CSLSRole::uninit();
    }
    int ticks_left;
    int uninits = 0;
};

int popped_id(const std::shared_ptr<CSLSRole> &role)
{
    auto *fake = dynamic_cast<FakeRole *>(role.get());
    return fake ? fake->id : -1;
}

} // namespace

TEST_CASE("dispatch: the reserved band only opens to roles that may use it")
{
    CHECK(sls_worker_dispatch_band(0, 10, 0) == SLSDispatchBand::OPEN);
    CHECK(sls_worker_dispatch_band(9, 10, 0) == SLSDispatchBand::OPEN);
    CHECK(sls_worker_dispatch_band(10, 10, 0) == SLSDispatchBand::FULL);
    CHECK(sls_worker_dispatch_band(6, 10, 3) == SLSDispatchBand::OPEN);
    CHECK(sls_worker_dispatch_band(7, 10, 3) == SLSDispatchBand::RESERVED_ONLY);
    CHECK(sls_worker_dispatch_band(9, 10, 3) == SLSDispatchBand::RESERVED_ONLY);
    CHECK(sls_worker_dispatch_band(10, 10, 3) == SLSDispatchBand::FULL);
}

TEST_CASE("dispatch: pop_if skips unreserved roles and keeps the queue order")
{
    CSLSRoleList list;
    list.push(std::make_shared<FakeRole>(1, true));
    list.push(std::make_shared<FakeRole>(2, true));
    list.push(std::make_shared<FakeRole>(3, false));
    list.push(std::make_shared<FakeRole>(4, false));

    auto reserved_only = [](const CSLSRole &r) { return !r.uses_unreserved_capacity(); };
    CHECK(popped_id(list.pop_if(reserved_only)) == 3);
    CHECK(popped_id(list.pop_if(reserved_only)) == 4);
    CHECK_FALSE(static_cast<bool>(list.pop_if(reserved_only)));
    CHECK(list.size() == 2);
    CHECK(popped_id(list.pop()) == 1);
    CHECK(popped_id(list.pop()) == 2);
}

TEST_CASE("dispatch: a player is unreserved unless its authorization says otherwise")
{
    CSLSPlayer plain;
    CHECK(plain.uses_unreserved_capacity());
    plain.on_authorization_response("OK");
    CHECK(plain.uses_unreserved_capacity());

    CSLSPlayer reserved;
    reserved.on_authorization_response("{\"reserved\": true}");
    CHECK_FALSE(reserved.uses_unreserved_capacity());

    CSLSPlayer explicit_false;
    explicit_false.on_authorization_response("{\"reserved\": false, \"other\": 1}");
    CHECK(explicit_false.uses_unreserved_capacity());

    CSLSPlayer wrong_type;
    wrong_type.on_authorization_response("{\"reserved\": \"yes\"}");
    CHECK(wrong_type.uses_unreserved_capacity());

    CSLSPlayer garbage;
    garbage.on_authorization_response("{not json");
    CHECK(garbage.uses_unreserved_capacity());
}

TEST_CASE("dispatch: the hook query carries the session id and boot epoch only when there is a session")
{
    CSLSPlayer player;
    CHECK(player.session_id_param().empty());
    player.set_session_id(4242);
    CHECK(player.session_id_param() == "&session_id=4242&boot_epoch=" + sls_boot_epoch());
    CHECK(player.get_session_id() == 4242);
}

TEST_CASE("dispatch: a registry kick reaches the role that owns the snapshot")
{
    CSLSPlayer player;
    auto snapshot = std::make_shared<PlayerStatsSnapshot>();
    player.set_player_snapshot(snapshot);
    CHECK_FALSE(player.is_kick_requested());
    snapshot->kick_requested.store(true);
    CHECK(player.is_kick_requested());

    CSLSPlayer other;
    CHECK_FALSE(other.is_kick_requested());
    other.request_kick();
    CHECK(other.is_kick_requested());
}

TEST_CASE("dispatch: the backlog reaper waits for a handed-off player's verdict")
{
    // Reaping it before the verdict would drop the on_close an authorized
    // player is owed.
    CSLSRoleList list;
    auto role = std::make_shared<DeferringRole>(2);
    list.push(role);
    const int64_t now = sls_gettime_ms();
    CHECK(list.reap_unadopted(now, 5000) == 0);
    CHECK(role->uninits == 0);
    CHECK(list.size() == 1);
    CHECK(list.reap_unadopted(now, 5000) == 1);
    CHECK(role->uninits == 1);
    CHECK(list.size() == 0);
}

TEST_CASE("dispatch: a reload keeps a verified internal player's worker reservation")
{
    CSLSPlayer internal_player;
    internal_player.hand_off_pending_admission(nullptr, "publish/live/s", 0, "log", true);
    CHECK_FALSE(internal_player.uses_unreserved_capacity());

    CSLSPlayer public_player;
    public_player.hand_off_pending_admission(nullptr, "publish/live/s", 0, "log", false);
    CHECK(public_player.uses_unreserved_capacity());
}

TEST_CASE("dispatch: the backlog reaper checks a handed-off player's verdict before its TTL")
{
    // The authorization deadline is shorter than the backlog TTL, so the
    // verdict must be read while the player waits, not only when it is reaped.
    CSLSRoleList list;
    const int64_t now = sls_gettime_ms();
    auto role = std::make_shared<DeferringRole>(1, now);
    list.push(role);
    CHECK(list.reap_unadopted(now, 5000) == 0);
    CHECK(role->ticks_left == 0);
    CHECK(role->uninits == 0);
    CHECK(list.size() == 1); // still adoptable
}
