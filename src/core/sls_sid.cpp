#include "sls_sid.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <string>
#include <vector>

#include "auth_reject_cache.hpp"
#include "common.hpp"
#include "conn_rate_limiter.hpp"

using std::string;

namespace {
// Render an SRT peer sockaddr to its bare IP string (no port), used as the
// per-source key for the connection rate limiter. Returns "" for a null or
// unknown-family address, which the limiter treats as "do not rate limit".
std::string sls_peeraddr_ip(const struct sockaddr *peeraddr)
{
    if (peeraddr == nullptr)
        return std::string();
    char buf[INET6_ADDRSTRLEN] = {0};
    if (peeraddr->sa_family == AF_INET)
    {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(peeraddr)->sin_addr, buf, sizeof(buf));
    }
    else if (peeraddr->sa_family == AF_INET6)
    {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 *>(peeraddr)->sin6_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

// Collapse an IPv4-mapped IPv6 literal (::ffff:a.b.c.d) to its IPv4 text so a
// peer keys identically across the two paths that build the reject-cache key.
std::string normalize_ip(const std::string &ip)
{
    static const std::string v4mapped = "::ffff:";
    if (ip.size() > v4mapped.size() &&
        ip.compare(0, v4mapped.size(), v4mapped) == 0 &&
        ip.find(':', v4mapped.size()) == std::string::npos)
        return ip.substr(v4mapped.size());
    return ip;
}
} // namespace

std::map<std::string, std::string> sls_parse_streamid(const char *sid)
{
    static const char stdhdr[] = "#!::";
    std::map<std::string, std::string> ret;
    if (!sid)
        return ret;

    if (strlen(sid) > 4 && memcmp(sid, stdhdr, 4) == 0)
    {
        std::vector<string> items;
        sls_split_string(sid + 4, ",", items);
        for (auto &i : items)
        {
            std::vector<string> kv;
            sls_split_string(i, "=", kv);
            if (kv.size() == 2)
                ret[sls_trim(kv.at(0))] = sls_trim(kv.at(1));
        }
    }
    else
    {
        std::vector<string> items;
        sls_split_string(sid, "/", items);
        if (items.size() >= 3)
        {
            ret["h"] = sls_trim(items.at(0));
            ret["sls_app"] = sls_trim(items.at(1));
            ret["r"] = sls_trim(items.at(2));
        }
    }
    return ret;
}

std::string sls_canonical_sid_key(const std::string &streamid)
{
    if (streamid.empty())
        return streamid;
    std::map<std::string, std::string> kv = sls_parse_streamid(streamid.c_str());
    auto h = kv.find("h");
    auto a = kv.find("sls_app");
    auto r = kv.find("r");
    if (h == kv.end() || a == kv.end() || r == kv.end())
        return streamid;
    return h->second + "/" + a->second + "/" + r->second;
}

std::string sls_reject_cache_key(const std::string &peer_ip, const char *streamid)
{
    return normalize_ip(peer_ip) + "|" + sls_canonical_sid_key(streamid ? streamid : "");
}

bool sls_validate_sid_format(const char *sid)
{
    if (!sid || sid[0] == '\0')
        return false;

    std::map<std::string, std::string> kv = sls_parse_streamid(sid);
    auto h = kv.find("h");
    auto a = kv.find("sls_app");
    auto r = kv.find("r");
    if (h == kv.end() || a == kv.end() || r == kv.end())
        return false;

    return sls_is_safe_name(h->second.c_str()) &&
           sls_is_safe_name(a->second.c_str()) &&
           sls_is_safe_name(r->second.c_str());
}

int sls_publisher_listen_callback(void *opaque, SRTSOCKET ns, int hsversion,
                                  const struct sockaddr *peeraddr,
                                  const char *streamid)
{
    (void)hsversion;

    if (!sls_validate_sid_format(streamid))
    {
        // Any reject reason serializes on the wire as 1000 + reason, which the
        // upstream relay's is_srt_handshake_reject also counts as defense in
        // depth. ROGUE ("incorrect data in handshake") fits a malformed sid.
        srt_setrejectreason(ns, SRT_REJ_ROGUE);
        return -1;
    }

    SLSListenCallbackCtx *ctx = static_cast<SLSListenCallbackCtx *>(opaque);
    if (ctx == nullptr)
        return 0;

    ConnRateLimiter *limiter = ctx->conn_rate_limiter;
    if (limiter != nullptr && limiter->enabled() && limiter->should_reject(sls_peeraddr_ip(peeraddr)))
    {
        srt_setrejectreason(ns, SRT_REJ_RESOURCE);
        return -1;
    }

    if (ctx->auth_reject_cache != nullptr &&
        ctx->auth_reject_cache->is_blocked(sls_reject_cache_key(sls_peeraddr_ip(peeraddr), streamid)))
    {
        srt_setrejectreason(ns, SRT_REJ_RESOURCE);
        return -1;
    }

    return 0; // accept; the post-accept webhook still authorizes the key
}

int sls_player_listen_callback(void *opaque, SRTSOCKET ns, int hsversion,
                               const struct sockaddr *peeraddr,
                               const char *streamid)
{
    (void)hsversion;

    if (!sls_validate_sid_format(streamid))
    {
        srt_setrejectreason(ns, SRT_REJ_ROGUE);
        return -1;
    }

    SLSListenCallbackCtx *ctx = static_cast<SLSListenCallbackCtx *>(opaque);
    if (ctx != nullptr)
    {
        ConnRateLimiter *limiter = ctx->conn_rate_limiter;
        if (limiter != nullptr && limiter->enabled() && limiter->should_reject(sls_peeraddr_ip(peeraddr)))
        {
            srt_setrejectreason(ns, SRT_REJ_RESOURCE);
            return -1;
        }
    }

    return 0; // accept; the post-accept handler resolves and authorizes
}
