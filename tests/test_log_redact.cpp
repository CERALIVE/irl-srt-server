#include "doctest.h"

#include <string>
#include <vector>

#include "SLSLogRedact.hpp"

namespace
{

bool leaks(const std::string &line, const std::string &secret)
{
    return sls_redact_log(line).find(secret) != std::string::npos;
}

} // namespace

TEST_CASE("log redaction: query credentials are scrubbed in plain and URL-encoded form")
{
    CHECK(sls_redact_log("stream='publish/r/relay1?k=s3cr3tValue'") == "stream='publish/r/relay1?k=<redacted>'");
    CHECK(sls_redact_log("srt_url=publish%2Fr%2Frelay1%3Fk%3Ds3cr3tValue&remote_ip=127.0.0.1") ==
          "srt_url=publish%2Fr%2Frelay1%3Fk%3D<redacted>&remote_ip=127.0.0.1");
    CHECK(sls_redact_log("GET /check?player_key=abcdef123456 completed") == "GET /check?player_key=<redacted> completed");
    CHECK(sls_redact_log("url=/x?a=1&token=legacy123 done") == "url=/x?a=1&token=<redacted> done");
    CHECK(sls_redact_log("x%3Ftoken%3Dlegacy123") == "x%3Ftoken%3D<redacted>");
    CHECK(sls_redact_log("x%26k%3Dabc123") == "x%26k%3D<redacted>");
    CHECK(sls_redact_log("x%3fk%3dabc123") == "x%3fk%3d<redacted>");
}

TEST_CASE("log redaction: a bare k is scrubbed only after a delimiter")
{
    CHECK(sls_redact_log("track=video1") == "track=video1");
    CHECK(sls_redact_log("pk=1 k=secret") == "pk=1 k=<redacted>");
}

TEST_CASE("log redaction: credential prefixes keep the class but lose the value")
{
    for (const char *prefix : {"cl_live_", "cl_out_", "hob_", "pvw_", "sbr_", "opush_", "ipush_"})
    {
        const std::string secret = "Zx9-Qw_8.aB~cD+ef/gh=";
        const std::string line = std::string("key ") + prefix + secret + " end";
        CAPTURE(prefix);
        CHECK(sls_redact_log(line) == std::string("key ") + prefix + "<redacted> end");
        // URL-encoded neighbours do not shield the value.
        CHECK_FALSE(leaks(std::string("srt_url=play%2Fx%2F") + prefix + secret, secret));
    }
}

TEST_CASE("log redaction: the reader slot and the secret hook paths are scrubbed")
{
    CHECK(sls_redact_log("Received stream_id 'play/r/rawreaderkey123' from 127.0.0.1:5000") ==
          "Received stream_id 'play/r/<redacted>' from 127.0.0.1:5000");
    CHECK(sls_redact_log("srt_url=play%2Fr%2Frawreaderkey123") == "srt_url=play%2Fr%2F<redacted>");
    CHECK(sls_redact_log("http_url='http://127.0.0.1:8090/admission/n0desecret', status=403") ==
          "http_url='http://127.0.0.1:8090/admission/<redacted>', status=403");
    CHECK(sls_redact_log("GET http://127.0.0.1:8090/sls/player-key/n0desecret?player_key=x failed") ==
          "GET http://127.0.0.1:8090/sls/player-key/<redacted>?player_key=<redacted> failed");
    CHECK(sls_redact_log("u=%2Fadmission%2Fn0desecret") == "u=%2Fadmission%2F<redacted>");
}

TEST_CASE("log redaction: quoted values after a key word are scrubbed")
{
    CHECK(sls_redact_log("validation FAILED for key='rawkey12...'") == "validation FAILED for key='<redacted>'");
    CHECK(sls_redact_log("Player key 'rawkey1234' from 10.0.0.1:4000 - offline") ==
          "Player key '<redacted>' from 10.0.0.1:4000 - offline");
    CHECK(sls_redact_log("cache hit for player_key=\"rawkey1234\"") == "cache hit for player_key=\"<redacted>\"");
    CHECK(sls_redact_log("keyframe='I'") == "keyframe='I'");
}

TEST_CASE("log redaction: registered literals are replaced wherever they appear")
{
    sls_redact_add_literals({"/hooks/unshapedSecretPath", "api-key-literal-0001", "abc", ""});
    CHECK(sls_redact_log("POST http://127.0.0.1:9/hooks/unshapedSecretPath?on_event=on_connect") ==
          "POST http://127.0.0.1:9<redacted>?on_event=on_connect");
    CHECK(sls_redact_log("Authorization: api-key-literal-0001") == "Authorization: <redacted>");
    // Values under four characters are ignored, so short words survive.
    CHECK(sls_redact_log("abc def") == "abc def");
}

TEST_CASE("log redaction: non-secret fields survive")
{
    const std::string line = "[connection:a1b2] Player connected app='r' stream='relay42' latency=200ms from "
                             "198.51.100.7:4000 publisher=publish/r/relay42 kbitrate=1500";
    CHECK(sls_redact_log(line) == line);
    const std::string mixed = "new publisher[198.51.100.7:4000], stream='publish/r/relay42?k=cl_live_SeCrEt'.";
    CHECK(sls_redact_log(mixed) == "new publisher[198.51.100.7:4000], stream='publish/r/relay42?k=<redacted>'.");
}

TEST_CASE("log redaction: hook URL paths are extracted for literal registration")
{
    CHECK(sls_redact_url_path("http://127.0.0.1:8090/admission/abc") == "/admission/abc");
    CHECK(sls_redact_url_path("https://hooks.example/x?y=1") == "/x?y=1");
    CHECK(sls_redact_url_path("http://127.0.0.1:8090/") == "");
    CHECK(sls_redact_url_path("http://127.0.0.1:8090") == "");
    CHECK(sls_redact_url_path("") == "");
}

TEST_CASE("log redaction: none of the planted credentials survive a mixed line")
{
    const std::vector<std::string> secrets = {"SECRETaaaa1", "SECRETbbbb2", "SECRETcccc3", "SECRETdddd4",
                                              "SECRETeeee5", "SECRETffff6", "SECRETgggg7", "SECRETiiii9"};
    const std::string line = "a?k=SECRETaaaa1 b=play/r/SECRETbbbb2 c=cl_out_SECRETcccc3 d=%3Fk%3DSECRETdddd4 "
                             "e=/admission/SECRETeeee5 f=token=SECRETffff6 g=player_key='SECRETgggg7' "
                             "i=%2Fsls%2Fplayer-key%2FSECRETiiii9";
    const std::string out = sls_redact_log(line);
    for (const auto &secret : secrets)
    {
        CAPTURE(secret);
        CHECK(out.find(secret) == std::string::npos);
    }
}

TEST_CASE("log redaction: a quoted stream path after key= keeps its path and loses its credential")
{
    CHECK(sls_redact_log("CSLSMapData::add ok, key='publish/live/kick', ring_size=1") ==
          "CSLSMapData::add ok, key='publish/live/kick', ring_size=1");
    CHECK(sls_redact_log("add failed for key='publish/r/relay1?k=cl_live_abc123' (cap?)") ==
          "add failed for key='publish/r/relay1?k=<redacted>' (cap?)");
}

TEST_CASE("log redaction: an unquoted key= stream path keeps its path")
{
    CHECK(sls_redact_log("CSLSMapData::get, key=publish/r/relay1?k=cl_live_abc, not found") ==
          "CSLSMapData::get, key=publish/r/relay1?k=<redacted>, not found");
    CHECK(sls_redact_log("api key=s3cr3tValue") == "api key=<redacted>");
}
