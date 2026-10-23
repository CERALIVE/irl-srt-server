#include "doctest.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <map>
#include <unistd.h>

#include "SLSPusherManager.hpp"
#include "SLSLog.hpp"
#include "util.hpp"

namespace
{
struct PusherConnection
{
    int udp = -1;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET peer = SRT_INVALID_SOCK;
    std::map<SRTSOCKET, int> polls;
    int port = 0;
    std::atomic<bool> reject{false};
    CSLSMapData data;
    CSLSRoleList roles;
    std::shared_ptr<CSLSPusherManager> manager = std::make_shared<CSLSPusherManager>();

    ~PusherConnection()
    {
        roles.erase();
        if (peer != SRT_INVALID_SOCK)
            srt_close(peer);
        if (listener != SRT_INVALID_SOCK)
            srt_close(listener);
        if (udp != -1)
            ::close(udp);
        for (const auto &poll : polls)
            srt_epoll_release(poll.second);
        srt_cleanup();
    }

    void init(int connect_timeout)
    {
        REQUIRE(initialize_logger() == SLS_OK);
        REQUIRE(srt_startup() == 0);
        udp = ::socket(AF_INET, SOCK_DGRAM, 0);
        REQUIRE(udp != -1);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (int candidate = 40000; candidate <= 40499; ++candidate)
        {
            addr.sin_port = htons(candidate);
            if (::bind(udp, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0)
            {
                port = candidate;
                break;
            }
        }
        REQUIRE(port != 0);
        char key[] = "publish/live/fixture";
        REQUIRE(data.add(key) == SLS_OK);
        auto conf = std::make_shared<SLS_RELAY_INFO>();
        conf->m_mode = SLS_PM_ALL;
        conf->m_reconnect_interval = 1;
        conf->m_idle_streams_timeout = 30;
        conf->m_upstreams.push_back(
            "srt://127.0.0.1:" + std::to_string(port) +
            "?streamid=publish/live/fixture&connect_timeout=" + std::to_string(connect_timeout));
        manager->set_relay_conf(conf);
        manager->set_relay_info("publish/live", "fixture");
        manager->set_map_data(&data);
        manager->set_role_list(&roles);
        REQUIRE(manager->start() == SLS_OK);
    }

    void listen()
    {
        listener = srt_create_socket();
        REQUIRE(listener != SRT_INVALID_SOCK);
        bool async = false;
        REQUIRE(srt_setsockflag(listener, SRTO_RCVSYN, &async, sizeof(async)) == 0);
        REQUIRE(srt_bind_acquire(listener, udp) == 0);
        udp = -1;
        REQUIRE(srt_listen_callback(
                    listener,
                    [](void *context, SRTSOCKET, int, const sockaddr *, const char *)
                    { return static_cast<PusherConnection *>(context)->reject.load() ? -1 : 0; },
                    this) == 0);
        watch(listener, SRT_EPOLL_IN);
        REQUIRE(srt_listen(listener, 1) == 0);
    }

    void watch(SRTSOCKET fd, int events)
    {
        const int eid = srt_epoll_create();
        REQUIRE(eid >= 0);
        polls.emplace(fd, eid);
        REQUIRE(srt_epoll_add_usock(eid, fd, &events) == 0);
    }

    void await(SRTSOCKET fd, int events)
    {
        SRT_EPOLL_EVENT event{};
        const int eid = polls.at(fd);
        const int count = srt_epoll_uwait(eid, &event, 1, 5000);
        INFO("socket=" << fd << " state=" << srt_getsockstate(fd) << " expected events=" << events
                       << " actual events=" << event.events << " listener state=" << srt_getsockstate(listener));
        REQUIRE(count == 1);
        REQUIRE(event.fd == fd);
        REQUIRE((event.events & events) != 0);
        REQUIRE(srt_epoll_remove_usock(eid, fd) == 0);
        REQUIRE(srt_epoll_release(eid) == 0);
        polls.erase(fd);
    }

    void receive_from(const std::shared_ptr<CSLSRole> &pusher)
    {
        await(listener, SRT_EPOLL_IN);
        peer = srt_accept(listener, nullptr, nullptr);
        REQUIRE(peer != SRT_INVALID_SOCK);
        REQUIRE(pusher->get_sock_state() == SRTS_CONNECTED);
        REQUIRE(pusher->handler() == SLS_OK); // Anchor at the live ring head.

        char payload[TS_UDP_LEN]{};
        char key[] = "publish/live/fixture";
        watch(peer, SRT_EPOLL_IN);
        REQUIRE(data.put(key, payload, sizeof(payload)) == sizeof(payload));
        CHECK(pusher->handler() == sizeof(payload));
        await(peer, SRT_EPOLL_IN);
        char received[TS_UDP_LEN]{};
        REQUIRE(srt_recvmsg(peer, received, sizeof(received)) == sizeof(received));
        CHECK(std::memcmp(payload, received, sizeof(payload)) == 0);
    }
};
} // namespace

TEST_CASE("pusher defers buffered media until the asynchronous handshake completes")
{
    PusherConnection connection;
    connection.init(10000);
    auto pusher = connection.roles.pop();
    REQUIRE(pusher != nullptr);
    REQUIRE(pusher->get_sock_state() == SRTS_CONNECTING);
    REQUIRE(pusher->handler() == SLS_OK);
    char payload[TS_UDP_LEN]{};
    char key[] = "publish/live/fixture";
    REQUIRE(connection.data.put(key, payload, sizeof(payload)) == sizeof(payload));
    REQUIRE(pusher->handler() == SLS_OK);
    CHECK(pusher->get_sock_state() == SRTS_CONNECTING);

    connection.watch(pusher->get_fd(), SRT_EPOLL_OUT | SRT_EPOLL_ERR);
    connection.listen(); // Release the handshake gate, without a timed sleep.
    connection.await(pusher->get_fd(), SRT_EPOLL_OUT);
    connection.receive_from(pusher);
}

static void check_failed_handshake(bool rejection)
{
    PusherConnection connection;
    connection.init(1000);
    auto pusher = connection.roles.pop();
    REQUIRE(pusher != nullptr);
    const int failed_fd = pusher->get_fd();
    connection.watch(failed_fd, SRT_EPOLL_ERR);
    if (rejection)
    {
        connection.reject.store(true);
        connection.listen();
    }
    connection.await(failed_fd, SRT_EPOLL_ERR);
    CHECK(pusher->get_sock_state() == SRTS_BROKEN);
    REQUIRE(pusher->handler() == SLS_ERROR);
    CHECK(pusher->get_state() == SLS_RS_INVALID);
    CHECK(pusher->get_fd() == 0);
    REQUIRE(pusher->uninit() == SLS_OK); // Queue the existing reconnect path.

    connection.reject.store(false);
    if (connection.listener == SRT_INVALID_SOCK)
        connection.listen();
    REQUIRE(connection.manager->reconnect(sls_gettime_ms() + 1000) == SLS_OK);
    auto reconnected = connection.roles.pop();
    REQUIRE(reconnected != nullptr);
    CHECK(reconnected->get_fd() != failed_fd);
    connection.watch(reconnected->get_fd(), SRT_EPOLL_OUT | SRT_EPOLL_ERR);
    connection.await(reconnected->get_fd(), SRT_EPOLL_OUT);
    connection.receive_from(reconnected);
}

TEST_CASE("pusher closes a connection timeout and reconnects through its manager")
{
    check_failed_handshake(false);
}

TEST_CASE("pusher closes a listener rejection and reconnects through its manager")
{
    check_failed_handshake(true);
}
