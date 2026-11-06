#pragma once

#include <string>
#include <string_view>
#include <vector>

// Credential scrubbing for rendered log lines. Every line SLS logs passes
// through sls_redact_log() inside the root log sink (SLSLogSink.hpp), so a
// call site that prints a stream id, resolved SID, key_stream_name or webhook
// URL without scrubbing it cannot leak a credential to stdout or a log file.
//
// What is replaced with "<redacted>" (the surrounding text is kept, so relay
// ids, app names, stream names and peer addresses stay readable):
//   - values of the query parameters k, key, player_key and token, after a
//     delimiter, including their URL-encoded forms (%3Fk%3D..., %26k%3D...);
//   - the value after the credential prefixes cl_live_, cl_out_, hob_, pvw_,
//     sbr_, opush_ and ipush_ (the prefix itself is kept);
//   - the reader credential slot in play/r/<credential> and play%2Fr%2F...;
//   - the secret path segment in /admission/<secret> and
//     /sls/player-key/<secret>, plain or URL-encoded;
//   - a quoted value introduced by a word ending in "key", such as
//     key='...', player_key="..." or "Player key '...'";
//   - every literal registered with sls_redact_add_literals().
std::string sls_redact_log(std::string_view line);

// Registers literal secrets that carry no recognisable shape (configured hook
// URL paths, API keys, SRT passphrases). The set only grows: a value stays
// registered for the life of the process, so lines logged by a manager that is
// still draining after a reload are scrubbed too. Values shorter than four
// characters are ignored. Thread-safe.
void sls_redact_add_literals(const std::vector<std::string> &literals);

// The path and query of an http(s) URL ("/admission/abc?x=1"), or "" when the
// URL has no path beyond "/". Used to register configured hook paths.
std::string sls_redact_url_path(const std::string &url);
