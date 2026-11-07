#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

// The live CSLSManager generations, shared between the main loop and the HTTP
// control-plane threads.
//
// A SIGHUP reload does not stop the old manager: it keeps serving the readers
// it already has until they leave, while a new manager takes new
// connections. Both are live, so the HTTP side must be able to reach either.
// The main thread publishes and retires generations here; an HTTP handler
// takes a snapshot (shared_ptr copies) under the lock and works on it without
// the lock. A snapshot keeps every manager it names alive, so a request in
// flight across a reload, or across the sweep that removes a drained
// manager, can never dereference a freed one. Whichever thread drops the last
// reference destroys the manager.
template <class T> class CSLSGenerationSet
{
public:
    void publish(std::shared_ptr<T> generation)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_current = std::move(generation);
    }

    // Moves the current generation to the retiring list. Until publish() runs
    // again there is no current generation.
    void retire_current()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_current)
            m_retiring.push_back(std::move(m_current));
        m_current.reset();
    }

    void remove(const std::shared_ptr<T> &generation)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_current == generation)
            m_current.reset();
        m_retiring.erase(std::remove(m_retiring.begin(), m_retiring.end(), generation), m_retiring.end());
    }

    std::shared_ptr<T> current() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_current;
    }

    // The current generation first (when there is one), then the retiring
    // ones from newest to oldest.
    std::vector<std::shared_ptr<T>> all() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::shared_ptr<T>> out;
        out.reserve(m_retiring.size() + 1);
        if (m_current)
            out.push_back(m_current);
        out.insert(out.end(), m_retiring.rbegin(), m_retiring.rend());
        return out;
    }

    std::vector<std::shared_ptr<T>> retiring() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_retiring;
    }

    // Empties the set and hands every generation to the caller (shutdown).
    std::vector<std::shared_ptr<T>> clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::shared_ptr<T>> out;
        if (m_current)
            out.push_back(std::move(m_current));
        out.insert(out.end(), m_retiring.begin(), m_retiring.end());
        m_current.reset();
        m_retiring.clear();
        return out;
    }

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<T> m_current;
    std::vector<std::shared_ptr<T>> m_retiring;
};
