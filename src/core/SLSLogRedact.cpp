#include "SLSLogRedact.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>

namespace
{

constexpr std::string_view kMarker = "<redacted>";

// Longest first, so "player_key" wins over "key" and "key" over "k".
constexpr std::string_view kParamNames[] = {"player_key", "token", "key", "k"};
constexpr std::string_view kCredentialPrefixes[] = {"cl_live_", "cl_out_", "opush_", "ipush_", "hob_", "pvw_", "sbr_"};
// A secret follows these markers as one path segment. Matched without regard
// to case so %2f and %2F both count.
constexpr std::string_view kSecretSlots[] = {"play/r/",         "play%2Fr%2F",      "/admission/",
                                             "%2Fadmission%2F", "/sls/player-key/", "%2Fsls%2Fplayer-key%2F"};

std::mutex g_literal_mutex;
std::shared_ptr<const std::vector<std::string>> g_literals = std::make_shared<const std::vector<std::string>>();

std::shared_ptr<const std::vector<std::string>> literals_snapshot()
{
    std::lock_guard<std::mutex> lock(g_literal_mutex);
    return g_literals;
}

bool is_alnum(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

// The character class a credential value is drawn from (URL-safe base64,
// hex, percent-encoding, and the separators keys are built with).
bool is_value_char(char c)
{
    return is_alnum(c) || c == '_' || c == '.' || c == '~' || c == '%' || c == '+' || c == '/' || c == '=' || c == '-';
}

bool is_segment_char(char c)
{
    return c != '/' && is_value_char(c);
}

bool starts_with(std::string_view s, size_t pos, std::string_view lit)
{
    return pos + lit.size() <= s.size() && s.compare(pos, lit.size(), lit) == 0;
}

bool starts_with_ci(std::string_view s, size_t pos, std::string_view lit)
{
    if (pos + lit.size() > s.size())
        return false;
    for (size_t i = 0; i < lit.size(); i++)
    {
        if (std::tolower(static_cast<unsigned char>(s[pos + i])) != std::tolower(static_cast<unsigned char>(lit[i])))
            return false;
    }
    return true;
}

template <class Pred> size_t run_length(std::string_view s, size_t pos, Pred pred)
{
    size_t end = pos;
    while (end < s.size() && pred(s[end]))
        end++;
    return end - pos;
}

// True when a query parameter name may start at pos: the start of the line,
// a separator (including '=' and ':', so "url=token=..." is caught), or a
// URL-encoded '?' / '&'.
bool after_delimiter(std::string_view s, size_t pos)
{
    if (pos == 0)
        return true;
    const char prev = s[pos - 1];
    if (prev != '\0' && std::strchr("?&;=: \t'\"(,[{|<", prev) != nullptr)
        return true;
    return pos >= 3 && (starts_with_ci(s, pos - 3, "%3F") || starts_with_ci(s, pos - 3, "%26"));
}

// Tries each rule at pos. On a match appends the scrubbed text to out,
// advances pos past it and returns true.
bool redact_at(std::string_view line, size_t &pos, const std::vector<std::string> &literals, std::string &out)
{
    for (const auto &literal : literals)
    {
        if (starts_with(line, pos, literal))
        {
            out += kMarker;
            pos += literal.size();
            return true;
        }
    }

    for (const auto slot : kSecretSlots)
    {
        if (!starts_with_ci(line, pos, slot))
            continue;
        const size_t len = run_length(line, pos + slot.size(), is_segment_char);
        if (len == 0)
            continue;
        out.append(line.substr(pos, slot.size()));
        out += kMarker;
        pos += slot.size() + len;
        return true;
    }

    for (const auto prefix : kCredentialPrefixes)
    {
        if (!starts_with(line, pos, prefix))
            continue;
        const size_t len = run_length(line, pos + prefix.size(), is_value_char);
        if (len == 0)
            continue;
        out.append(prefix);
        out += kMarker;
        pos += prefix.size() + len;
        return true;
    }

    if (after_delimiter(line, pos))
    {
        for (const auto name : kParamNames)
        {
            if (!starts_with_ci(line, pos, name))
                continue;
            const size_t sep_at = pos + name.size();
            size_t sep_len = 0;
            if (sep_at < line.size() && line[sep_at] == '=')
                sep_len = 1;
            else if (starts_with_ci(line, sep_at, "%3D"))
                sep_len = 3;
            if (sep_len == 0)
                continue;
            const size_t len = run_length(line, sep_at + sep_len, is_value_char);
            if (len == 0)
                continue;
            // key=<host>/<app>/<stream> names a stream (CSLSMapData's key=...),
            // not a credential; the rules above still scrub any k= inside it.
            const std::string_view value = line.substr(sep_at + sep_len, len);
            if (name == "key" && std::count(value.begin(), value.end(), '/') >= 2)
                continue;
            out.append(line.substr(pos, name.size() + sep_len));
            out += kMarker;
            pos = sep_at + sep_len + len;
            return true;
        }
    }

    // A quoted value introduced by a word ending in "key": key='...',
    // player_key="...", "Player key '...'". A value with two or more '/' is a
    // stream path (CSLSMapData's key='publish/r/<id>?k=...'), not a bare key:
    // it is left to the rules above, which still scrub its credential part.
    if (starts_with_ci(line, pos, "key"))
    {
        size_t at = pos + 3;
        if (at < line.size() && (is_alnum(line[at]) || line[at] == '_'))
            return false;
        while (at < line.size() && line[at] == ' ')
            at++;
        if (at < line.size() && (line[at] == '=' || line[at] == ':'))
            at++;
        while (at < line.size() && line[at] == ' ')
            at++;
        if (at < line.size() && (line[at] == '\'' || line[at] == '"'))
        {
            const char quote = line[at];
            const size_t close = line.find(quote, at + 1);
            const std::string_view value =
                line.substr(at + 1, close == std::string_view::npos ? std::string_view::npos : close - at - 1);
            if (std::count(value.begin(), value.end(), '/') >= 2)
                return false;
            out.append(line.substr(pos, at + 1 - pos));
            out += kMarker;
            pos = close == std::string_view::npos ? line.size() : close;
            return true;
        }
    }
    return false;
}

} // namespace

std::string sls_redact_log(std::string_view line)
{
    const auto literals = literals_snapshot();
    std::string out;
    out.reserve(line.size());
    size_t pos = 0;
    while (pos < line.size())
    {
        if (!redact_at(line, pos, *literals, out))
            out.push_back(line[pos++]);
    }
    return out;
}

void sls_redact_add_literals(const std::vector<std::string> &literals)
{
    std::lock_guard<std::mutex> lock(g_literal_mutex);
    auto next = std::make_shared<std::vector<std::string>>(*g_literals);
    for (const auto &literal : literals)
    {
        if (literal.size() < 4 || std::find(next->begin(), next->end(), literal) != next->end())
            continue;
        next->push_back(literal);
    }
    std::stable_sort(next->begin(), next->end(),
                     [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    g_literals = std::move(next);
}

std::string sls_redact_url_path(const std::string &url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return "";
    const size_t slash = url.find('/', scheme + 3);
    if (slash == std::string::npos)
        return "";
    std::string path = url.substr(slash);
    return path == "/" ? "" : path;
}
