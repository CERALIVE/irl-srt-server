#include "SLSPlayerRegistry.hpp"

#include <algorithm>
#include <array>

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

} // namespace

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
                                 [](const PlayerRegistration &player) {
                                     return player.liveness.expired() ||
                                            (player.snapshot &&
                                             player.snapshot->closed.load(std::memory_order_relaxed));
                                 }),
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
