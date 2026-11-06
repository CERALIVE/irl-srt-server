
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

private:
    bool m_dispatch_reserved = false;
};
