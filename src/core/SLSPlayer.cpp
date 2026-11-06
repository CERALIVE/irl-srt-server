
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

#include <nlohmann/json.hpp>

#include "SLSPlayer.hpp"
#include "SLSLog.hpp"

/**
 * CSLSPlayer class implementation
 */

CSLSPlayer::CSLSPlayer()
{
    m_is_write = 1;

    snprintf(m_role_name, sizeof(m_role_name), "player");

    // Record player connection for summary logging
    sls_get_summary_logger().record_player_connect();
}

CSLSPlayer::~CSLSPlayer()
{
    // Record player disconnection for summary logging
    sls_get_summary_logger().record_player_disconnect();
}

int CSLSPlayer::handler()
{
    if (m_handoff_pending)
        return SLS_OK; // no media before the admission
    sample_viewer_snd_drops();
    return handler_write_data();
}

void CSLSPlayer::hand_off_pending_admission(CSLSPlayerRegistry *registry, const std::string &stream_key,
                                            int max_players, const std::string &log_id)
{
    m_handoff_registry = registry;
    m_handoff_stream_key = stream_key;
    m_handoff_max_players = max_players;
    m_handoff_log_id = log_id;
    m_handoff_pending = true;
}

void CSLSPlayer::on_worker_tick()
{
    if (!m_handoff_pending)
        return;

    if (SLS_OK == check_http_passed())
    {
        m_handoff_pending = false;
        const PlayerAdmission admission =
            m_handoff_registry ? m_handoff_registry->authorize(m_handoff_stream_key, get_session_id(), m_handoff_max_players)
                               : PlayerAdmission::ADMITTED;
        if (admission == PlayerAdmission::ADMITTED)
        {
            spdlog::info("[{}] CSLSPlayer::on_worker_tick, player session={} stream={} authorized after a reload, "
                         "admitted.",
                         fmt::ptr(this), get_session_id(), m_handoff_stream_key);
            return;
        }
        SLS_REFUSAL_LOG(SLSSecurityEvent::ADMISSION_REFUSAL, warn,
                        "[connection:{}] refused authorized player session={} stream={} after a reload: {} (limit "
                        "{:d}).",
                        m_handoff_log_id, get_session_id(), m_handoff_stream_key,
                        sls_player_admission_reason(admission), m_handoff_max_players);
        // Authorized, so the teardown posts on_close.
        mark_invalid();
        return;
    }

    // Refused by the webhook (socket already closed) or past the deadline:
    // nothing was authorized, and get_state() may reap the player now.
    if (is_invalid() || get_fd() == 0)
    {
        m_handoff_pending = false;
        return;
    }

    // A peer that left while the verdict is outstanding keeps raising epoll
    // errors; stop listening to them until get_state() can close it.
    if (!m_handoff_unsubscribed)
    {
        const int state = get_sock_state();
        if (state == SLS_ERROR || state == SRTS_BROKEN || state == SRTS_CLOSED || state == SRTS_NONEXIST)
        {
            remove_from_epoll();
            m_handoff_unsubscribed = true;
        }
    }
}

void CSLSPlayer::on_authorization_response(const std::string &body)
{
    // Plain-text bodies ("OK") stay valid and mean unreserved.
    if (body.empty() || body[0] != '{')
        return;
    try
    {
        auto parsed = nlohmann::json::parse(body);
        auto it = parsed.find("reserved");
        if (it != parsed.end() && it->is_boolean())
            m_dispatch_reserved = it->get<bool>();
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[{}] CSLSPlayer::on_authorization_response, ignoring unparsable body: {}", fmt::ptr(this),
                     e.what());
    }
}
