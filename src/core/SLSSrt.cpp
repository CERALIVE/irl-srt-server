
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

#include <errno.h>
#include <string.h>
#include <map>
#include <string>
#include <memory.h>
#include <memory>
#include "spdlog/spdlog.h"

#include "SLSSrt.hpp"
#include "SLSHandle.hpp"
#include "SLSLog.hpp"
#include "SLSLock.hpp"
#include "util.hpp"
#include "sls_sid.hpp"
#include "SLSManager.hpp"

/**
 * CSLSSrt class implementation
 */
extern const struct in6_addr in6addr_any;        /* :: */
extern const struct in6_addr in6addr_loopback;   /* ::1 */
#define IN6ADDR_ANY_INIT { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } } }

bool CSLSSrt::m_inited = false;

CSLSSrt::CSLSSrt()
{
    memset(&m_sc, 0x0, sizeof(m_sc));
    m_sc.port = 8000; //for test
    m_sc.fd = 0;
    m_sc.eid = 0;
    m_sc.latency = 20;

    m_sc.backlog = 128;
    memset(m_passphrase, 0, sizeof(m_passphrase));
    m_pbkeylen = 0;
    memset(m_peer_name, 0, sizeof(m_peer_name));
    m_peer_port = 0;
    m_peer_addr_raw = 0;
    m_peer_addr6_raw = in6addr_any;
    m_is_ipv6 = false;
}
CSLSSrt::~CSLSSrt()
{
}

int CSLSSrt::libsrt_init()
{
    if (m_inited)
        return SLS_OK;

    if (srt_startup() < 0)
    {
        return SLSERROR_UNKNOWN;
    }
    m_inited = true;

    uint32_t libsrt_version = srt_getversion();
    spdlog::info("Initialized libsrt v{:d}.{:d}.{:d}",
                 (libsrt_version >> 16) & 0xff, (libsrt_version >> 8) & 0xff, libsrt_version & 0xff);

    return SLS_OK;
}

int CSLSSrt::libsrt_uninit()
{
    if (!m_inited)
        return SLS_OK;
    srt_cleanup();
    return SLS_OK;
}

int CSLSSrt::libsrt_epoll_create()
{
    return srt_epoll_create();
}

void CSLSSrt::libsrt_epoll_release(int eid)
{
    srt_epoll_release(eid);
}

void CSLSSrt::libsrt_print_error_info()
{
    /**
SRTS_BROKEN: The socket was connected, but the connection was broken
SRTS_CLOSING: The socket may still be open and active, but closing is requested, so no further operations will be accepted (active operations will be completed before closing)
SRTS_CLOSED: The socket has been closed, but not yet removed by the GC thread
SRTS_NONEXIST:
     */

#define set_error_map(k) \
    map_error[k] = std::string(#k);

    char szBuf[1024] = {0};
    std::map<int, std::string> map_error;

    set_error_map(SRTS_INIT);
    set_error_map(SRTS_OPENED);
    set_error_map(SRTS_LISTENING);
    set_error_map(SRTS_CONNECTING);
    set_error_map(SRTS_CONNECTED);
    set_error_map(SRTS_BROKEN);
    set_error_map(SRTS_CLOSING);
    set_error_map(SRTS_CLOSED);
    set_error_map(SRTS_NONEXIST);

    set_error_map(SRT_ENOCONN);
    set_error_map(SRT_ECONNLOST);
    set_error_map(SRT_EINVALMSGAPI);
    set_error_map(SRT_EINVALBUFFERAPI);
    set_error_map(SRT_EASYNCSND);
    set_error_map(SRT_ETIMEOUT);
    set_error_map(SRT_EPEERERR);

    spdlog::error("--------srt error--------");
    std::map<int, std::string>::iterator it;
    for (it = map_error.begin(); it != map_error.end(); ++it)
    {
        snprintf(szBuf, sizeof(szBuf), "%d: %s", it->first, it->second.c_str());
        spdlog::error(szBuf);
    }
    spdlog::error("----------end------------");
    map_error.clear();
}

int CSLSSrt::libsrt_neterrno()
{
    int err = srt_getlasterror(NULL);
    spdlog::error("CSLSSrt::libsrt_neterrno, err={:d}, {}.", err, srt_getlasterror_str());
    return err;
}

int CSLSSrt::libsrt_lasterror()
{
    return srt_getlasterror(NULL);
}

void CSLSSrt::libsrt_set_context(SRTContext *sc)
{
    m_sc = *sc;
}

void CSLSSrt::libsrt_set_latency(int latency)
{
    m_sc.latency = latency;
}

void CSLSSrt::libsrt_set_peer_idle_timeout(int timeout_ms)
{
    m_sc.peer_idle_timeout = timeout_ms;
}

void CSLSSrt::libsrt_set_passphrase(const char *passphrase, int pbkeylen)
{
    if (passphrase != NULL)
    {
        strlcpy(m_passphrase, passphrase, sizeof(m_passphrase));
    }
    else
    {
        m_passphrase[0] = '\0';
    }
    m_pbkeylen = pbkeylen;
}

// Static profile -> option-set table. Index matches the SrtProfile enum value.
// LOSSMAXTTL cap=30 on L1/L2 is the recommendation from the Todo 6 A/B
// reorder-stress matrix; L3 keeps the 200 baseline of direct SRT. The 100ms
// RCVLATENCY floor on L1/L2 keeps the receiver from clamping the publisher
// upward, so the device's PEERLATENCY wins the max(device,listener) handshake.
// fec_accept=true only on L1: FEC rides the bonded default listener; L2/L3 are
// filter-free (a plain caller still connects to L1 unmodified — see libsrt_setup).
static const SrtProfileSpec kSrtProfileTable[] = {
    {"L1-freeze-nak", true, true, true, 30, 100, true},
    {"L2-classic", true, true, false, 30, 100, false},
    {"L3-direct", false, false, false, 200, 0, false},
};

const SrtProfileSpec &sls_srt_profile_spec(SrtProfile profile)
{
    size_t idx = static_cast<size_t>(profile);
    if (idx >= sizeof(kSrtProfileTable) / sizeof(kSrtProfileTable[0]))
        idx = static_cast<size_t>(SrtProfile::L3Direct);
    return kSrtProfileTable[idx];
}

int CSLSSrt::libsrt_setup(int port, SrtProfile profile)
{
    struct addrinfo hints = {0}, *ai;
    int fd = -1;
    int ret;
    char portstr[10];
    SRTContext *s = &m_sc;

    m_sc.port = port;

    hints.ai_family = AF_INET6;//AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portstr, sizeof(portstr), "%d", s->port);
    hints.ai_flags |= AI_PASSIVE;
    ret = getaddrinfo(s->hostname[0] ? s->hostname : NULL, portstr, &hints, &ai);
    if (ret)
    {
        spdlog::error("[{}] CSLSSrt::libsrt_setup, Failed to resolve hostname {}: {}.",
                      fmt::ptr(this), s->hostname, gai_strerror(ret));
        return ret;
    }
    // RAII owner for the resolved addrinfo: freed on every exit path. The manual
    // freeaddrinfo on each error branch was the leak fixed in 169e16c; owning it
    // here means a future early-return cannot reintroduce it.
    std::unique_ptr<struct addrinfo, decltype(&freeaddrinfo)> ai_guard(ai, &freeaddrinfo);

    fd = srt_create_socket();
    if (fd < 0)
    {
        return libsrt_neterrno();
    }
    // RAII for the SRT socket: closed on any early return before a successful
    // bind hands it to s->fd (at which point release() disarms the guard so the
    // live listener socket survives). SrtSocketHandle (SLSHandle.hpp) is the
    // shared, move-only owner; this future-proofs the same leak on the fd side
    // (the 169e16c fix) without changing the fd value that lands in s->fd.
    SrtSocketHandle sock(fd);

    /*
    if (libsrt_setsockopt(h, fd, SRTO_STREAMID, "SRTO_STREAMID", sc->streamid, strlen(s->streamid)) < 0) {
        ret = libsrt_neterrno();
        freeaddrinfo(ai);
        return ret;
    }
*/
    int ipv6Only = 0;
    const SrtProfileSpec &prof = sls_srt_profile_spec(profile);
    [[maybe_unused]] int freezeValue = prof.freeze ? 1 : 0;
    // LOSSMAXTTL is the reorder-tolerance ceiling, applied to every listener
    // here and inherited by accepted sockets. The value comes from the profile
    // (30 for L1/L2 per the Todo 6 A/B verdict, 200 baseline for L3); on stock
    // libsrt this same clamp is how the profile's `freeze` intent is realized.
    int lossmaxttlvalue = prof.lossmaxttl;

    // SRTO_RCVBUF is bytes; SRTO_FC is the in-flight window in PACKETS. The old
    // hardcoded 100 MB / 128000-packet sizing let a single (even pre-auth)
    // connection reserve ~100 MB of receive buffer, a flood amplifier. Cap the
    // buffer at a config-tunable ceiling (rcv_buf_mb, default 8 MB) and scale FC
    // at 1024 packets/MB (~1.5 MB of window per MB of buffer) so the buffer, not
    // FC, stays the binding memory cap while in-flight packets drop to the same
    // ~8 MB scale. root_conf is NULL in unit tests with no loaded config.
    sls_conf_srt_t *root_conf = (sls_conf_srt_t *)sls_conf_get_root_conf();
    int rcv_buf_mb = (root_conf && root_conf->rcv_buf_mb > 0) ? root_conf->rcv_buf_mb : 8;
    int fc = rcv_buf_mb * 1024;
    int rcv_buf = rcv_buf_mb * 1024 * 1024;

    // Every sockopt-failure exit between here and srt_bind just returns
    // SLS_ERROR; ai_guard and fd_guard above release the addrinfo and socket.
    int status = srt_setsockopt(fd, SOL_SOCKET, SRTO_IPV6ONLY, &ipv6Only, sizeof(ipv6Only));
    if (status < 0) {
        spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_IPV6ONLY failure. err={}.", fmt::ptr(this), srt_getlasterror_str());
        return SLS_ERROR;
    }

    status = srt_setsockopt(fd, SOL_SOCKET, SRTO_LOSSMAXTTL, &lossmaxttlvalue, sizeof(lossmaxttlvalue));
    if (status < 0) {
        spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_LOSSMAXTTL failure. err={}.", fmt::ptr(this), srt_getlasterror_str());
        return SLS_ERROR;
    }

    status = srt_setsockopt(fd, SOL_SOCKET, SRTO_FC, &fc, sizeof(fc));
    if (status < 0) {
        spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_FC failure. err={}.", fmt::ptr(this), srt_getlasterror_str());
        return SLS_ERROR;
    }
    status = srt_setsockopt(fd, SOL_SOCKET, SRTO_RCVBUF, &rcv_buf, sizeof(rcv_buf));
    if (status < 0) {
        spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_RCVBUF failure. err={}.", fmt::ptr(this), srt_getlasterror_str());
        return SLS_ERROR;
    }

    // Realize the profile's `freeze` intent with whichever mechanism the
    // compiled-against libsrt offers; NAK policy is layered on per profile below.
#if defined(SLS_HAVE_SRTO_REORDERFREEZE)
    // CERALIVE/srt (reorderfreeze-1.5.5): freeze via the dedicated
    // SRTO_REORDERFREEZE option, decoupled from NAK reporting. The CMake probe
    // sets SLS_HAVE_SRTO_REORDERFREEZE because SRTO_REORDERFREEZE is an enum
    // value, invisible to #ifdef.
    status = srt_setsockopt(fd, SOL_SOCKET, SRTO_REORDERFREEZE, &freezeValue, sizeof(freezeValue));
    if (status < 0) {
        spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_REORDERFREEZE failure. err={}.", fmt::ptr(this), srt_getlasterror_str());
        return SLS_ERROR;
    }
    spdlog::info("[{}] CSLSSrt::libsrt_setup, SRT compat mode: reorderfreeze (CERALIVE/srt).", fmt::ptr(this));
#elif defined(SLS_HAVE_SRTO_SRTLAPATCHES)
    // Patched libsrt (irlserver/srt belabox fork): the custom SRTO_SRTLAPATCHES
    // fuses freeze + NAK-off, so the per-profile NAK set below is best-effort on
    // this legacy path (the fork's internal NAK-off may win for L1).
    status = srt_setsockopt(fd, SOL_SOCKET, SRTO_SRTLAPATCHES, &freezeValue, sizeof(freezeValue));
    if (status < 0) {
        spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_SRTLAPATCHES failure. err={}.", fmt::ptr(this), srt_getlasterror_str());
        return SLS_ERROR;
    }
    spdlog::info("[{}] CSLSSrt::libsrt_setup, SRT compat mode: srtlapatches (patched libsrt).", fmt::ptr(this));
#else
    // Stock libsrt (no freeze option): the profile's freeze intent is realized
    // by the SRTO_LOSSMAXTTL clamp already applied above (lossmaxttlvalue =
    // prof.lossmaxttl: 30 freezes decay for L1/L2, 200 leaves it for L3).
    // Authorized by ADR-002 ("SRT patch necessity").
    spdlog::info("[{}] CSLSSrt::libsrt_setup, SRT compat mode: standard-options (stock libsrt).", fmt::ptr(this));
#endif

    // Per-profile periodic NAK policy (L1 on, L2 off, L3 leaves the libsrt
    // default). Independent of freeze above, which is why the profile table can
    // pair the same freeze with either NAK setting.
    if (prof.set_nakreport) {
        int nakreport = prof.nakreport ? 1 : 0;
        status = srt_setsockopt(fd, SOL_SOCKET, SRTO_NAKREPORT, &nakreport, sizeof(nakreport));
        if (status < 0) {
            spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_NAKREPORT failure. err={}.", fmt::ptr(this), srt_getlasterror_str());
            return SLS_ERROR;
        }
    }

    // FEC rides L1: set SRTO_PACKETFILTER to the bare type "fec" (receiver-accept
    // form, no params), inherited by every accepted socket. A full-config FEC
    // device negotiates the merged filter; a non-FEC caller is NOT rejected — the
    // responder clears the filter for that one connection and connects plain
    // (srtla/docs/COMPATIBILITY.md §6 case b). One listener serves both senders, so
    // there is no separate FEC port. L2/L3 leave fec_accept false and stay filter-free.
    if (prof.fec_accept) {
        static const char kFecAcceptFilter[] = "fec";
        status = srt_setsockopt(fd, SOL_SOCKET, SRTO_PACKETFILTER, kFecAcceptFilter, sizeof(kFecAcceptFilter) - 1);
        if (status < 0) {
            spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_PACKETFILTER=fec failure. err={}.", fmt::ptr(this), srt_getlasterror_str());
            return SLS_ERROR;
        }
        spdlog::info("[{}] CSLSSrt::libsrt_setup, SRT profile FEC-accept: SRTO_PACKETFILTER=fec (accepts non-FEC callers plain).", fmt::ptr(this));
    }

    spdlog::info("[{}] CSLSSrt::libsrt_setup, SRT profile: {} (freeze={}, nakreport={}, lossmaxttl={}).",
                 fmt::ptr(this), prof.name, prof.freeze ? "on" : "off",
                 prof.set_nakreport ? (prof.nakreport ? "on" : "off") : "default", prof.lossmaxttl);

    // Explicitly enable too-late packet drop (TLPKTDROP) on the listener.
    // Libsrt defaults this on for SRTT_LIVE which is what we use, but
    // setting it explicitly removes any ambiguity from future libsrt
    // default changes or non-live transtype regressions. Critical for
    // the egress write path: when a viewer cannot keep up, TLPKTDROP
    // lets libsrt silently discard packets whose TSBPD time has expired
    // from the send queue, freeing send-buffer space so EASYNCSND does
    // not pile up forever and so handler_write_data's stuck-viewer
    // detection only fires for genuinely broken links.
    int tlpktdrop = 1;
    status = srt_setsockopt(fd, SOL_SOCKET, SRTO_TLPKTDROP, &tlpktdrop, sizeof(tlpktdrop));
    if (status < 0) {
        spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_TLPKTDROP failure. err={}.", fmt::ptr(this), srt_getlasterror_str());
        return SLS_ERROR;
    }

    /* Set the socket's send or receive buffer sizes, if specified.
       If unspecified or setting fails, system default is used. */
    if (s->latency > 0)
    {
        // SRTO_RCVLATENCY is the receive-direction TSBPD floor and clamps
        // publishers (who send to us); SRTO_PEERLATENCY is what we, as
        // sender, commit to for players (the handshake negotiates the
        // player's effective receive latency to
        //   max(player.RCVLATENCY, our.PEERLATENCY)).
        // SRTO_LATENCY sets both at once, but set RCVLATENCY explicitly
        // too — and check each return — because a publisher was observed
        // negotiating 120ms with latency_min=500, i.e. the floor was not
        // landing on the socket and the failure was being swallowed.
        //
        // SRTO_LATENCY / SRTO_PEERLATENCY / SRTO_RCVLATENCY are int32 options.
        // s->latency is int64_t, so passing &s->latency with its 8-byte size
        // made libsrt reject EVERY call with SRT_EINVPARAM ("Bad parameters") —
        // that IS the swallowed failure noted above: the floor never landed and
        // the socket kept libsrt's 120ms default. Pass a 4-byte int so the sets
        // actually take (RCVLATENCY then gets lowered per-profile below; the
        // PEERLATENCY commitment stays at latency_min).
        int latency_ms = static_cast<int>(s->latency);
        if (srt_setsockopt(fd, SOL_SOCKET, SRTO_LATENCY, &latency_ms, sizeof(latency_ms)) < 0)
            spdlog::warn("[{}] CSLSSrt::libsrt_setup, SRTO_LATENCY={} failed: {}.",
                         fmt::ptr(this), latency_ms, srt_getlasterror_str());
        if (srt_setsockopt(fd, SOL_SOCKET, SRTO_PEERLATENCY, &latency_ms, sizeof(latency_ms)) < 0)
            spdlog::warn("[{}] CSLSSrt::libsrt_setup, SRTO_PEERLATENCY={} failed: {}.",
                         fmt::ptr(this), latency_ms, srt_getlasterror_str());
        if (srt_setsockopt(fd, SOL_SOCKET, SRTO_RCVLATENCY, &latency_ms, sizeof(latency_ms)) < 0)
            spdlog::warn("[{}] CSLSSrt::libsrt_setup, SRTO_RCVLATENCY={} failed: {}.",
                         fmt::ptr(this), latency_ms, srt_getlasterror_str());
    }

    // L1/L2 publisher listeners pin a LOW receive-latency floor (100ms),
    // applied last so it wins over latency_min above. The device sets its own
    // per-profile SRTO_PEERLATENCY; the handshake negotiates to
    // max(device.PEERLATENCY, listener.RCVLATENCY), so a low listener floor lets
    // the device's choice win instead of the receiver clamping it upward.
    if (prof.rcvlatency_floor_ms > 0)
    {
        int rcvlat = prof.rcvlatency_floor_ms;
        if (srt_setsockopt(fd, SOL_SOCKET, SRTO_RCVLATENCY, &rcvlat, sizeof(rcvlat)) < 0)
            spdlog::warn("[{}] CSLSSrt::libsrt_setup, profile SRTO_RCVLATENCY={} failed: {}.",
                         fmt::ptr(this), rcvlat, srt_getlasterror_str());
        else
            spdlog::info("[{}] CSLSSrt::libsrt_setup, SRT profile RCVLATENCY floor={}ms.",
                         fmt::ptr(this), rcvlat);
    }

    // SRTO_PEERIDLETIMEO bounds how long a connected peer may go fully silent
    // (no data, no keepalive) before libsrt declares the link broken. The
    // belabox SRT fork raises the default so bonded-cellular gaps on the
    // encoder<->server SRTLA leg don't kill a healthy publisher. The cost is
    // that when an encoder abandons a connection (SRT session reset / link
    // flap) its SHUTDOWN frequently never reaches us over that same flapping
    // path, so the stale server-side socket squats the stream key until
    // idle_streams_timeout. Set as a listener pre-option here, it is inherited
    // by every accepted socket. Publisher takeover already handles the
    // reconnect case; this is the backstop for an encoder that dies without
    // reconnecting, and is most useful where publishers reach SLS over a
    // stable hop (e.g. a local srtla_rec terminating the bond). 0 leaves the
    // fork/libsrt default untouched so bonded direct-SRTLA deploys keep their
    // tolerance.
    if (s->peer_idle_timeout > 0)
    {
        int peer_idle = s->peer_idle_timeout;
        if (srt_setsockopt(fd, SOL_SOCKET, SRTO_PEERIDLETIMEO, &peer_idle, sizeof(peer_idle)) < 0)
            spdlog::warn("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_PEERIDLETIMEO={} failed: {}.",
                         fmt::ptr(this), peer_idle, srt_getlasterror_str());
        else
            spdlog::info("[{}] CSLSSrt::libsrt_setup, SRTO_PEERIDLETIMEO set to {}ms.", fmt::ptr(this), peer_idle);
    }
    // Kernel UDP socket buffers. Defaults on Linux are ~200KB-1MB which
    // is too tight for SRT live streaming under bursty traffic: when
    // the kernel queue fills, sendto() returns EAGAIN and libsrt
    // surfaces it as EASYNCSND, AND outgoing SRT control packets
    // (keepalives, ACK responses) get dropped — which in turn lets the
    // peer's libsrt declare the connection broken even when the network
    // is healthy. 8MB matches what most production SRT receivers run.
    // If the per-server config sets an explicit size, honour it.
    int udp_rcvbuf = s->recv_buffer_size > 0 ? s->recv_buffer_size : 8 * 1024 * 1024;
    int udp_sndbuf = s->send_buffer_size > 0 ? s->send_buffer_size : 8 * 1024 * 1024;
    srt_setsockopt(fd, SOL_SOCKET, SRTO_UDP_RCVBUF, &udp_rcvbuf, sizeof(udp_rcvbuf));
    srt_setsockopt(fd, SOL_SOCKET, SRTO_UDP_SNDBUF, &udp_sndbuf, sizeof(udp_sndbuf));
    if (s->reuse)
    {
        if (srt_setsockopt(fd, SOL_SOCKET, SRTO_REUSEADDR, &s->reuse, sizeof(s->reuse)))
            spdlog::warn("[{}] CSLSSrt::libsrt_setup, setsockopt(SRTO_REUSEADDR) failed.", fmt::ptr(this));
    }

    if (m_pbkeylen > 0)
    {
        if (srt_setsockopt(fd, SOL_SOCKET, SRTO_PBKEYLEN, &m_pbkeylen, sizeof(m_pbkeylen)) < 0)
        {
            spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_PBKEYLEN={} failed: {}.",
                          fmt::ptr(this), m_pbkeylen, srt_getlasterror_str());
            return SLS_ERROR;
        }
    }
    if (m_passphrase[0] != '\0')
    {
        if (srt_setsockopt(fd, SOL_SOCKET, SRTO_PASSPHRASE, m_passphrase, strlen(m_passphrase)) < 0)
        {
            spdlog::error("[{}] CSLSSrt::libsrt_setup, srt_setsockopt SRTO_PASSPHRASE failed: {}.",
                          fmt::ptr(this), srt_getlasterror_str());
            return SLS_ERROR;
        }
    }

    ret = srt_bind(fd, ai->ai_addr, ai->ai_addrlen);
    if (ret)
    {
        return libsrt_neterrno();
    }

    s->fd = fd;
    sock.release(); // socket now owned by s->fd; keep it open

    spdlog::info("[{}] CSLSSrt::libsrt_setup, fd={:d}.", fmt::ptr(this), fd);

    return SLS_OK;
}

int CSLSSrt::libsrt_listen(int backlog)
{
    m_sc.backlog = backlog;
    int ret = srt_listen(m_sc.fd, backlog);
    if (ret)
        return libsrt_neterrno();

    spdlog::info("[{}] CSLSSrt::libsrt_listen, ok, fd={:d}, at port={:d}.", fmt::ptr(this), m_sc.fd, m_sc.port);
    return SLS_OK;
}

int CSLSSrt::libsrt_set_listen_callback(srt_listen_callback_fn * listen_callback_fn, void *opaque) {
    int ret = srt_listen_callback(m_sc.fd, listen_callback_fn, opaque);
    if (ret) {
        return SLS_ERROR;
    }
    return SLS_OK;
}

int CSLSSrt::libsrt_accept()
{
    struct sockaddr_in6  scl;
    int sclen = sizeof(scl);
    char ip[INET6_ADDRSTRLEN] = {0};
    struct sockaddr_in6  *addrtmp;

    int new_sock = srt_accept(m_sc.fd, (struct sockaddr *)&scl, &sclen); //NULL, NULL);//(sockaddr*)&scl, &sclen);
    if (new_sock == SRT_INVALID_SOCK)
    {
        int err_no = libsrt_neterrno();
        spdlog::info("[{}] CSLSSrt::libsrt_accept failed, sock={:d}, error_no={:d}.",
                     fmt::ptr(this), m_sc.fd, err_no);
        return SLS_ERROR;
    } 
    addrtmp = (struct sockaddr_in6 *)&scl;
    inet_ntop(AF_INET6, &addrtmp->sin6_addr, ip, INET6_ADDRSTRLEN);
    return new_sock;
}

int CSLSSrt::libsrt_close()
{
    if (m_sc.fd)
    {
        srt_close(m_sc.fd);
        m_sc.fd = 0;
    }
    return SLS_OK;
}

int CSLSSrt::libsrt_set_fd(int fd)
{
    libsrt_close();
    m_sc.fd = fd;
    return SLS_OK;
}

int CSLSSrt::libsrt_get_fd()
{
    return m_sc.fd;
}

int CSLSSrt::libsrt_set_eid(int eid)
{
    m_sc.eid = eid;
    return SLS_OK;
}

int CSLSSrt::libsrt_getsockopt(SRT_SOCKOPT optname, const char *optnamestr, void *optval, int *optlen)
{
    if (srt_getsockopt(m_sc.fd, 0, optname, optval, optlen) < 0)
    {
        spdlog::error("[{}] CSLSSrt::libsrt_getsockopt, failed to get option {} on socket: {}", fmt::ptr(this), optnamestr, srt_getlasterror_str());
        return SLSERROR(EIO);
    }
    return 0;
}

int CSLSSrt::libsrt_setsockopt(SRT_SOCKOPT optname, const char *optnamestr, const void *optval, int optlen)
{
    if (srt_setsockopt(m_sc.fd, 0, optname, optval, optlen) < 0)
    {
        spdlog::error("[{}] CSLSSrt::libsrt_setsockopt, failed to set option {} on socket: {}", fmt::ptr(this), optnamestr, srt_getlasterror_str());
        return SLSERROR(EIO);
    }
    return 0;
}

int CSLSSrt::libsrt_socket_nonblock(int enable)
{
    int ret = srt_setsockopt(m_sc.fd, 0, SRTO_SNDSYN, &enable, sizeof(enable));
    if (ret < 0)
        return ret;
    return srt_setsockopt(m_sc.fd, 0, SRTO_RCVSYN, &enable, sizeof(enable));
}

std::map<std::string, std::string> CSLSSrt::libsrt_parse_sid(char *sid)
{
    // Delegates to the free function in sls_sid so the handshake callback
    // (no CSLSSrt instance yet) and this post-accept path parse identically.
    return sls_parse_streamid(sid);
}

int CSLSSrt::libsrt_read(char *buf, int size)
{
    int ret;
    ret = srt_recvmsg(m_sc.fd, buf, size);
    if (ret < 0)
    {
        int err_no = libsrt_neterrno();
        spdlog::warn("[{}] CSLSSrt::libsrt_read failed, sock={:d}, ret={:d}, err_no={:d}.",
                     fmt::ptr(this), m_sc.fd, ret, err_no);
    }
    return ret;
}

int CSLSSrt::libsrt_write(const char *buf, int size)
{
    int ret;
    ret = srt_sendmsg(m_sc.fd, buf, size, -1, 0);
    if (ret < 0)
    {
        // EASYNCSND is transient backpressure (SRT send buffer full).
        // Callers must distinguish it from real failures via
        // libsrt_lasterror(); we log it at trace so high-rate
        // backpressure under viewer congestion doesn't flood the log.
        // Everything else stays at warn — it indicates a broken or
        // unrecoverable socket.
        int err_no = srt_getlasterror(NULL);
        if (err_no == SRT_EASYNCSND)
        {
            spdlog::trace("[{}] CSLSSrt::libsrt_write backpressure, sock={:d}, size={:d}.",
                         fmt::ptr(this), m_sc.fd, size);
        }
        else
        {
            spdlog::warn("[{}] CSLSSrt::libsrt_write failed, sock={:d}, ret={:d}, errno={:d}, {}.",
                         fmt::ptr(this), m_sc.fd, ret, err_no, srt_getlasterror_str());
        }
    }
    return ret;
}

int CSLSSrt::libsrt_add_to_epoll(int eid, bool write)
{
    int ret = SLS_OK;
    int fd = m_sc.fd;

    if (!eid)
    {
        spdlog::error("[{}] CSLSSrt::libsrt_add_to_epoll failed, m_eid={:d}.", fmt::ptr(this), eid);
        return SLS_ERROR;
    }

    // Readable roles (publisher/puller) watch IN. Writable roles
    // (player/pusher) are NOT armed for OUT at rest: SRT_EPOLL_OUT is
    // level-triggered and a drained SRT socket is always writable, so a
    // permanently-armed OUT makes srt_epoll_wait return on every single
    // iteration and turns the worker into a busy-loop. Egress is instead
    // driven by the worker's periodic pass over the publisher ring, and
    // OUT is armed on demand (libsrt_arm_epoll_out) only while a write is
    // backpressured. ERR is always watched so broken sockets surface.
    int modes = SRT_EPOLL_ERR;
    if (!write)
        modes |= SRT_EPOLL_IN;

    ret = srt_epoll_add_usock(eid, fd, &modes);
    if (ret < 0)
    {
        spdlog::error("[{}] CSLSSrt::libsrt_add_to_epoll, srt_epoll_add_usock failed, m_eid={:d}, fd={:d}, modes={:d}.",
                      fmt::ptr(this), eid, fd, modes);
        return libsrt_neterrno();
    }
    return ret;
}

int CSLSSrt::libsrt_arm_epoll_out(bool enable)
{
    if (!m_sc.eid)
    {
        spdlog::error("[{}] CSLSSrt::libsrt_arm_epoll_out failed, eid not set.", fmt::ptr(this));
        return SLS_ERROR;
    }
    int modes = SRT_EPOLL_ERR;
    if (enable)
        modes |= SRT_EPOLL_OUT;
    int ret = srt_epoll_update_usock(m_sc.eid, m_sc.fd, &modes);
    if (ret < 0)
    {
        spdlog::warn("[{}] CSLSSrt::libsrt_arm_epoll_out, srt_epoll_update_usock failed, eid={:d}, fd={:d}, enable={}.",
                     fmt::ptr(this), m_sc.eid, m_sc.fd, enable);
        return libsrt_neterrno();
    }
    return SLS_OK;
}

int CSLSSrt::libsrt_remove_from_epoll()
{
    int ret = SLS_OK;
    int fd = m_sc.fd;
    int eid = m_sc.eid;

    if (!eid)
    {
        spdlog::error("[{}] CSLSSrt::remove_from_epoll failed, m_eid={:d}.", fmt::ptr(this), eid);
        return SLS_ERROR;
    }

    ret = srt_epoll_remove_usock(eid, fd);
    if (ret < 0)
    {
        spdlog::error("[{}] CSLSSrt::remove_from_epoll, srt_epoll_remove_usock failed, m_eid={:d}, fd={:d}.",
                      fmt::ptr(this), eid, fd);
        return libsrt_neterrno();
    }
    return ret;
}

int CSLSSrt::libsrt_getsockstate()
{
    return srt_getsockstate(m_sc.fd);
}

int CSLSSrt::libsrt_getpeeraddr(char *peer_name, int &port)
{
    int ret = SLS_ERROR;
    struct sockaddr_in6 peer_addr;
    int peer_addr_len = sizeof(peer_addr);

    if (strlen(m_peer_name) == 0 || m_peer_port == 0)
    {
        ret = srt_getpeername(m_sc.fd, (struct sockaddr *)&peer_addr, &peer_addr_len);
        if (0 == ret)
        {
            inet_ntop(AF_INET6, &peer_addr.sin6_addr, m_peer_name, sizeof(m_peer_name));
            m_peer_port = ntohs(peer_addr.sin6_port);

            strlcpy(peer_name, m_peer_name, IP_MAX_LEN);
            port = m_peer_port;
            ret = SLS_OK;
        }
    }
    else
    {
        strlcpy(peer_name, m_peer_name, IP_MAX_LEN);
        port = m_peer_port;
        ret = SLS_OK;
    }
    return ret;
}


int CSLSSrt::libsrt_getpeeraddr_raw(unsigned long &address, struct in6_addr &address6) {
    int ret = SLS_ERROR;
    struct sockaddr_storage peer_addr; // Use sockaddr_storage
    int peer_addr_len = sizeof(peer_addr);

    if (0 == m_peer_addr_raw && !m_is_ipv6) { // Check if no address is stored yet
        ret = srt_getpeername(m_sc.fd, (struct sockaddr *)&peer_addr, &peer_addr_len);
        if (0 == ret) {
            if (peer_addr.ss_family == AF_INET) {
                // IPv4
                struct sockaddr_in *addr_in = (struct sockaddr_in *)&peer_addr;
                m_peer_addr_raw = ntohl(addr_in->sin_addr.s_addr);
                address = m_peer_addr_raw;
                m_is_ipv6 = false;
                ret = SLS_OK;
            } else if (peer_addr.ss_family == AF_INET6) {
                struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)&peer_addr;
                if (IN6_IS_ADDR_V4MAPPED(&addr_in6->sin6_addr)) {
                    // The dual-stack (IPV6ONLY=0) listener delivers IPv4 peers
                    // as ::ffff:a.b.c.d. Unwrap to the embedded IPv4 so the
                    // IPv4 ACL applies and an IPv4 deny cannot be bypassed via
                    // the mapped form. Keeps IPv4 matching on the same
                    // host-order path.
                    uint32_t v4_net;
                    memcpy(&v4_net, &addr_in6->sin6_addr.s6_addr[12], sizeof(v4_net));
                    m_peer_addr_raw = ntohl(v4_net);
                    address = m_peer_addr_raw;
                    m_is_ipv6 = false;
                } else {
                    m_peer_addr6_raw = addr_in6->sin6_addr;
                    address6 = m_peer_addr6_raw;
                    m_is_ipv6 = true;
                }
                ret = SLS_OK;
            } else {
                spdlog::error("[{}] SLSSrt::libsrt_getpeeraddr_raw failed: unsupported address family", fmt::ptr(this));
            }
        } else {
            spdlog::error("[{}] SLSSrt::libsrt_getpeeraddr_raw failed: not get peer IP address [ret={:d}]", fmt::ptr(this), ret);
        }
    } else {
        // Return the stored address based on the flag
        if (m_is_ipv6) {
            address6 = m_peer_addr6_raw;
        } else {
            address = m_peer_addr_raw;
        }
        ret = SLS_OK;
    }

    return ret;
}

int CSLSSrt::libsrt_get_statistics(SRT_TRACEBSTATS *currentStats, int clear) {
    int result = srt_bistats(m_sc.fd, currentStats, clear, 1);
    if (result == SLS_ERROR) {
        return SLS_ERROR;
    }
    return SLS_OK;
}
