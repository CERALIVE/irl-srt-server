#include "SLSPlayerRegistry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace
{

constexpr size_t ID_HEX_CHARS = 16;

std::string to_hex_prefix(const unsigned char *digest, size_t digest_len)
{
    static const char HEX[] = "0123456789abcdef";
    std::string out;
    out.reserve(ID_HEX_CHARS);
    for (size_t i = 0; i < digest_len && out.size() < ID_HEX_CHARS; i++)
    {
        out.push_back(HEX[digest[i] >> 4]);
        out.push_back(HEX[digest[i] & 0x0f]);
    }
    return out;
}

const std::array<unsigned char, 32> &process_secret()
{
    static const std::array<unsigned char, 32> secret = []
    {
        std::array<unsigned char, 32> bytes{};
        // RAND_bytes only fails when the OpenSSL CSPRNG cannot be seeded, which
        // leaves the zeroed secret. The id is then a plain HMAC under a known
        // key, so the IP is no longer hidden, and nothing else breaks.
        RAND_bytes(bytes.data(), static_cast<int>(bytes.size()));
        return bytes;
    }();
    return secret;
}

std::atomic<uint64_t> g_next_player_session_id{1};

} // namespace

const std::string &sls_boot_epoch()
{
    static const std::string epoch = []
    {
        unsigned char bytes[8] = {0};
        if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1)
        {
            // Without the CSPRNG the epoch still has to differ from the last
            // boot's, and the wall clock in nanoseconds does.
            const uint64_t now = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
            for (size_t i = 0; i < sizeof(bytes); i++)
                bytes[i] = static_cast<unsigned char>(now >> (8 * i));
        }
        return to_hex_prefix(bytes, sizeof(bytes));
    }();
    return epoch;
}

bool sls_valid_boot_epoch(const std::string &text)
{
    return text.size() == ID_HEX_CHARS &&
           std::all_of(text.begin(), text.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

uint64_t sls_next_player_session_id()
{
    return g_next_player_session_id.fetch_add(1, std::memory_order_relaxed);
}

bool sls_is_loopback_peer(const std::string &peer_ip)
{
    std::string ip = peer_ip;
    static const std::string kMapped = "::ffff:";
    if (ip.compare(0, kMapped.size(), kMapped) == 0)
        ip = ip.substr(kMapped.size());
    return ip == "::1" || ip.compare(0, 4, "127.") == 0;
}

bool sls_pending_player_room(bool internal, size_t public_pending, size_t internal_pending)
{
    return internal ? internal_pending < SLS_MAX_INTERNAL_PENDING_PLAYERS
                    : public_pending < SLS_MAX_PUBLIC_PENDING_PLAYERS;
}

const char *sls_player_admission_reason(PlayerAdmission result)
{
    switch (result)
    {
    case PlayerAdmission::ADMITTED:
        return "admitted";
    case PlayerAdmission::PLAYER_LIMIT:
        return "player limit reached";
    case PlayerAdmission::STREAM_PENDING_LIMIT:
        return "pending players per stream at cap";
    case PlayerAdmission::PEER_PENDING_LIMIT:
        return "pending players per peer at cap";
    case PlayerAdmission::GONE:
        return "session no longer registered";
    }
    return "unknown";
}

bool CSLSPlayerRegistry::is_live(const PlayerRegistration &player)
{
    if (player.liveness.expired())
        return false;
    if (!player.snapshot)
        return true;
    return !player.snapshot->closed.load(std::memory_order_relaxed) &&
           !player.snapshot->kick_requested.load(std::memory_order_relaxed);
}

void CSLSPlayerRegistry::add(PlayerRegistration registration)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key = registration.stream_key;
    prune_locked(key);
    m_players[key].push_back(std::move(registration));
}

int CSLSPlayerRegistry::count(const std::string &stream_key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    prune_locked(stream_key);
    auto it = m_players.find(stream_key);
    return it == m_players.end() ? 0 : static_cast<int>(it->second.size());
}

int CSLSPlayerRegistry::count_authorized(const std::string &stream_key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    prune_locked(stream_key);
    auto it = m_players.find(stream_key);
    if (it == m_players.end())
        return 0;
    return static_cast<int>(
        std::count_if(it->second.begin(), it->second.end(), [](const PlayerRegistration &p) { return p.authorized; }));
}

int CSLSPlayerRegistry::count_pending(const std::string &stream_key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    prune_locked(stream_key);
    auto it = m_players.find(stream_key);
    if (it == m_players.end())
        return 0;
    return static_cast<int>(
        std::count_if(it->second.begin(), it->second.end(), [](const PlayerRegistration &p) { return !p.authorized; }));
}

PlayerAdmission CSLSPlayerRegistry::admit(PlayerRegistration registration, const PlayerAdmissionLimits &limits)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key = registration.stream_key;
    prune_locked(key);

    int authorized = 0;
    int pending = 0;
    auto it = m_players.find(key);
    if (it != m_players.end())
    {
        for (const auto &player : it->second)
            (player.authorized ? authorized : pending)++;
    }

    if (limits.max_authorized > 0 && authorized >= limits.max_authorized)
        return PlayerAdmission::PLAYER_LIMIT;

    if (!registration.authorized && !registration.pending_cap_exempt)
    {
        if (limits.max_pending_per_stream > 0 && pending >= limits.max_pending_per_stream)
            return PlayerAdmission::STREAM_PENDING_LIMIT;
        if (limits.max_pending_per_peer > 0)
        {
            // Across every stream of this server block: one peer must not hold
            // more than its share of unauthorized sockets anywhere.
            int peer_pending = 0;
            for (const auto &entry : m_players)
            {
                for (const auto &player : entry.second)
                {
                    if (!player.authorized && !player.pending_cap_exempt && player.peer_ip == registration.peer_ip &&
                        is_live(player))
                        peer_pending++;
                }
            }
            if (peer_pending >= limits.max_pending_per_peer)
                return PlayerAdmission::PEER_PENDING_LIMIT;
        }
    }

    m_players[key].push_back(std::move(registration));
    return PlayerAdmission::ADMITTED;
}

PlayerAdmission CSLSPlayerRegistry::authorize(const std::string &stream_key, uint64_t session_id, int max_authorized)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    prune_locked(stream_key);
    auto it = m_players.find(stream_key);
    if (it == m_players.end())
        return PlayerAdmission::GONE;

    PlayerRegistration *target = nullptr;
    int authorized = 0;
    for (auto &player : it->second)
    {
        if (player.session_id == session_id)
            target = &player;
        else if (player.authorized)
            authorized++;
    }
    if (target == nullptr)
        return PlayerAdmission::GONE;
    if (target->authorized)
        return PlayerAdmission::ADMITTED;
    if (max_authorized > 0 && authorized >= max_authorized)
        return PlayerAdmission::PLAYER_LIMIT;
    target->authorized = true;
    return PlayerAdmission::ADMITTED;
}

bool CSLSPlayerRegistry::request_kick(const std::string &stream_key, uint64_t session_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    prune_locked(stream_key);
    auto it = m_players.find(stream_key);
    if (it == m_players.end())
        return false;
    for (const auto &player : it->second)
    {
        if (player.session_id != session_id || !player.snapshot)
            continue;
        // Release pairs with the acquire load on the owning thread.
        player.snapshot->kick_requested.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

std::vector<PlayerView> CSLSPlayerRegistry::list(const std::string &stream_key, int64_t now_ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    prune_locked(stream_key);
    std::vector<PlayerView> views;
    auto it = m_players.find(stream_key);
    if (it == m_players.end())
        return views;

    views.reserve(it->second.size());
    for (const auto &player : it->second)
    {
        PlayerView view;
        view.session_id = player.session_id;
        view.client_id = player.client_id;
        view.player_key_id = player.player_key_id;
        view.uptime_s = std::max<int64_t>(0, (now_ms - player.connected_at_ms) / 1000);
        view.latency_ms = player.latency_ms;
        if (player.snapshot)
        {
            view.rtt_ms = player.snapshot->rtt_ms.load(std::memory_order_relaxed);
            view.mbps_send_rate = player.snapshot->mbps_send_rate.load(std::memory_order_relaxed);
            view.pkt_snd_drop_total = player.snapshot->pkt_snd_drop_total.load(std::memory_order_relaxed);
            view.pkt_retrans_total = player.snapshot->pkt_retrans_total.load(std::memory_order_relaxed);
        }
        views.push_back(std::move(view));
    }
    return views;
}

void CSLSPlayerRegistry::prune_locked(const std::string &stream_key)
{
    auto it = m_players.find(stream_key);
    if (it == m_players.end())
        return;

    auto &players = it->second;
    players.erase(std::remove_if(players.begin(), players.end(),
                                 [](const PlayerRegistration &player) { return !is_live(player); }),
                  players.end());
    if (players.empty())
        m_players.erase(it);
}

std::string sls_player_client_id(const std::string &peer_ip)
{
    const auto &secret = process_secret();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char *>(peer_ip.data()), peer_ip.size(), digest, &digest_len) == nullptr)
        return "";
    return to_hex_prefix(digest, digest_len);
}

std::string sls_player_key_id(const std::string &player_key)
{
    if (player_key.empty())
        return "";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(player_key.data()), player_key.size(), digest);
    return to_hex_prefix(digest, sizeof(digest));
}
