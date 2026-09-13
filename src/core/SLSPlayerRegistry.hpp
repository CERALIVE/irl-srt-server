#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Live SRT stats for one player, written by the player's own worker thread
// (CSLSRole::sample_viewer_snd_drops, about once a second) and read by /stats.
// The /stats thread must never touch a player's SRT socket, because the
// worker can free it at any moment. It only reads these atomics.
struct PlayerStatsSnapshot
{
    std::atomic<double> rtt_ms{0};
    std::atomic<double> mbps_send_rate{0};
    std::atomic<int64_t> pkt_snd_drop_total{0};
    std::atomic<int64_t> pkt_retrans_total{0};
    // Set by the worker when it tears the player's socket down, so the
    // registry stops counting the player before the role itself is freed.
    std::atomic<bool> closed{false};
};

struct PlayerRegistration
{
    // The player's publisher map key ("<app_uplive>/<stream>"). This is the
    // key after player-key resolution and uses the configured publisher
    // domain and app, so it matches the publisher's own map key.
    std::string stream_key;
    std::string client_id;
    // Empty when the player connected without a player key.
    std::string player_key_id;
    int64_t connected_at_ms = 0;
    int latency_ms = 0;
    // Expires when the role is freed. Held as void so the registry does not
    // depend on CSLSRole and stays unit-testable.
    std::weak_ptr<void> liveness;
    std::shared_ptr<PlayerStatsSnapshot> snapshot;
};

struct PlayerView
{
    std::string client_id;
    std::string player_key_id;
    int64_t uptime_s = 0;
    int latency_ms = 0;
    double rtt_ms = 0;
    double mbps_send_rate = 0;
    int64_t pkt_snd_drop_total = 0;
    int64_t pkt_retrans_total = 0;
};

// The players currently attached to each stream. Before this registry
// existed, player counts came from CSLSRoleList. That list is only the
// handoff queue between listener and worker, so a player dropped out of it
// as soon as a worker adopted it, and max_players_per_stream only ever
// counted players still waiting to be adopted.
//
// One registry per server block, owned by CSLSManager next to that server's
// CSLSMapPublisher. The listener thread adds players and /stats reads them,
// so every access takes the mutex.
class CSLSPlayerRegistry
{
public:
    void add(PlayerRegistration registration);
    int count(const std::string &stream_key);
    std::vector<PlayerView> list(const std::string &stream_key, int64_t now_ms);

private:
    // Drops closed and freed players. Caller holds m_mutex.
    void prune_locked(const std::string &stream_key);

    std::mutex m_mutex;
    std::unordered_map<std::string, std::vector<PlayerRegistration>> m_players;
};

// A stable identifier for a peer IP that does not reveal the IP. It is an
// HMAC keyed with a random secret generated once per process. The whole IPv4
// space is small enough to brute-force a plain hash, and the secret prevents
// that. The same address gets the same id until SLS restarts. 16 hex chars.
std::string sls_player_client_id(const std::string &peer_ip);

// Identifies which player key a player used, without exposing the key. It is
// an unsalted SHA-256, so the service that issued the keys can compute the
// same id and match a player to the key's owner. Player keys are
// high-entropy tokens, so the hash cannot be reversed. 16 hex chars.
std::string sls_player_key_id(const std::string &player_key);
