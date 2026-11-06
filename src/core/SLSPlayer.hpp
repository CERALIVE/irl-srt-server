
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

#pragma once

#include "SLSRole.hpp"

/**
 * CSLSPlayer
 */
class CSLSPlayer final : public CSLSRole
{
public:
    CSLSPlayer();
    ~CSLSPlayer() override;

    virtual int handler() override;

    // An unreserved player may not take the worker slots held back by
    // worker_reserved_connections. The authorization webhook reserves a
    // player by answering 200 with {"reserved": true}.
    bool uses_unreserved_capacity() const override
    {
        return !m_dispatch_reserved;
    }
    bool is_dispatch_reserved() const
    {
        return m_dispatch_reserved;
    }

    void on_authorization_response(const std::string &body) override;

    // A reload closes the listener that holds this player while its
    // authorization is pending. The listener hands the player to the workers
    // instead, and the adopting worker finishes the admission in
    // on_worker_tick(): the same registry authorize() the listener would run.
    void hand_off_pending_admission(CSLSPlayerRegistry *registry, const std::string &stream_key, int max_players,
                                    const std::string &log_id);
    void on_worker_tick() override;
    bool defers_teardown() const override
    {
        return m_handoff_pending;
    }

private:
    bool m_dispatch_reserved = false;
    // Admission state of a handed-off player. Set before the player enters
    // the role list, then used only by the worker that adopts it.
    bool m_handoff_pending = false;
    bool m_handoff_unsubscribed = false;
    CSLSPlayerRegistry *m_handoff_registry = nullptr;
    std::string m_handoff_stream_key;
    int m_handoff_max_players = 0;
    std::string m_handoff_log_id;
};
