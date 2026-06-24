// Unit tests for the two config-input validators that sit on the network
// boundary: sls_parse_port_list (listen-port spec parsing) and sls_is_safe_name
// (streamid component path-safety). Both take untrusted operator/peer input, so
// their rejection paths are security-relevant and pinned here. Dependency-free:
// a local CHECK macro keeps the result independent of whether assert() is
// compiled in (Release overrides CMAKE_CXX_FLAGS_RELEASE and omits -DNDEBUG).

#include <cstdio>
#include <string>
#include <vector>

#include "core/common.hpp"
#include "core/conf.hpp"

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static bool ports_eq(const std::vector<int> &got, const std::vector<int> &want)
{
    return got == want;
}

static void test_parse_port_list_single_and_range()
{
    std::vector<int> p;
    CHECK(sls_parse_port_list("4000", p) == 1 && ports_eq(p, {4000}));
    CHECK(sls_parse_port_list("5000-5005", p) == 6 &&
          ports_eq(p, {5000, 5001, 5002, 5003, 5004, 5005}));
    CHECK(sls_parse_port_list("5000-5010", p) == 11 &&
          ports_eq(p, {5000, 5001, 5002, 5003, 5004, 5005, 5006, 5007, 5008,
                       5009, 5010}));
    // A single-port range (a == b) is valid and yields exactly that one port.
    CHECK(sls_parse_port_list("5005-5005", p) == 1 && ports_eq(p, {5005}));
}

static void test_parse_port_list_reversed_range_rejected()
{
    std::vector<int> p;
    // The bite of the suite: lo > hi must be rejected, not silently iterated.
    CHECK(sls_parse_port_list("5005-5000", p) == -1);
}

static void test_parse_port_list_dedupe_preserves_first_seen_order()
{
    std::vector<int> p;
    CHECK(sls_parse_port_list("4000,4000,4000", p) == 1 && ports_eq(p, {4000}));
    CHECK(sls_parse_port_list("5000-5002,5001", p) == 3 &&
          ports_eq(p, {5000, 5001, 5002}));
    CHECK(sls_parse_port_list("4002,4000,4001", p) == 3 &&
          ports_eq(p, {4002, 4000, 4001}));
}

static void test_parse_port_list_whitespace_tolerated()
{
    std::vector<int> p;
    CHECK(sls_parse_port_list(" 4000 , 4001 ", p) == 2 &&
          ports_eq(p, {4000, 4001}));
    CHECK(sls_parse_port_list(" 5000 - 5002 ", p) == 3 &&
          ports_eq(p, {5000, 5001, 5002}));
}

static void test_parse_port_list_empty_and_malformed()
{
    std::vector<int> p;
    CHECK(sls_parse_port_list("", p) == 0 && p.empty());
    CHECK(sls_parse_port_list(nullptr, p) == 0 && p.empty());
    CHECK(sls_parse_port_list("0", p) == -1);
    CHECK(sls_parse_port_list("65536", p) == -1);
    CHECK(sls_parse_port_list("70000", p) == -1);
    CHECK(sls_parse_port_list("abc", p) == -1);
    CHECK(sls_parse_port_list("40-", p) == -1);
    CHECK(sls_parse_port_list("-40", p) == -1);
}

static void test_is_safe_name_accepts_plain_components()
{
    CHECK(sls_is_safe_name("live"));
    CHECK(sls_is_safe_name("test_stream-1"));
    CHECK(sls_is_safe_name("a.b"));
}

static void test_is_safe_name_rejects_traversal()
{
    CHECK(!sls_is_safe_name("."));
    CHECK(!sls_is_safe_name(".."));
    CHECK(!sls_is_safe_name("../etc"));
    CHECK(!sls_is_safe_name(nullptr));
    CHECK(!sls_is_safe_name(""));
}

static void test_is_safe_name_rejects_separators_and_control_bytes()
{
    // Path separators are the security-relevant metacharacters here: components
    // become filesystem path segments and map keys, so '/' and '\\' are what
    // turn a name into a traversal primitive.
    CHECK(!sls_is_safe_name("a/b"));
    CHECK(!sls_is_safe_name("a\\b"));
    // Every byte < 0x20 (and 0x7f) is rejected. 0x00 is the C-string terminator
    // so a literal NUL truncates the name upstream; the same guard that rejects
    // 0x01 here is what would reject any embedded control byte.
    const char ctl[] = {'a', 0x01, 'b', 0};
    CHECK(!sls_is_safe_name(ctl));
    const char tab[] = {'a', '\t', 'b', 0};
    CHECK(!sls_is_safe_name(tab));
    const char del[] = {'a', 0x7f, 'b', 0};
    CHECK(!sls_is_safe_name(del));
}

static void test_set_upstreams_rejects_empty_list()
{
    // A relay `upstreams` value that tokenises to zero hosts must be rejected at
    // parse time: an empty m_upstreams later modulo-by-zeros in get_hash_url.
    char buf[1024];
    sls_conf_cmd_t cmd{};
    cmd.name = "upstreams";
    cmd.mark = "upstreams";
    cmd.offset = 0;
    cmd.set = sls_conf_set_upstreams;
    cmd.min = 1;
    cmd.max = 1023;

    CHECK(sls_conf_set_upstreams("127.0.0.1:9000", &cmd, buf) == SLS_CONF_OK);
    CHECK(std::string(buf) == "127.0.0.1:9000");
    CHECK(sls_conf_set_upstreams("a:1 b:2 c:3", &cmd, buf) == SLS_CONF_OK);

    // Empty or whitespace-only values tokenise to zero upstreams -> rejected.
    CHECK(sls_conf_set_upstreams("", &cmd, buf) != SLS_CONF_OK);
    CHECK(sls_conf_set_upstreams("\" \"", &cmd, buf) != SLS_CONF_OK);
    CHECK(sls_conf_set_upstreams("\"   \"", &cmd, buf) != SLS_CONF_OK);
}

int main()
{
    test_parse_port_list_single_and_range();
    test_parse_port_list_reversed_range_rejected();
    test_parse_port_list_dedupe_preserves_first_seen_order();
    test_parse_port_list_whitespace_tolerated();
    test_parse_port_list_empty_and_malformed();
    test_is_safe_name_accepts_plain_components();
    test_is_safe_name_rejects_traversal();
    test_is_safe_name_rejects_separators_and_control_bytes();
    test_set_upstreams_rejects_empty_list();

    if (g_failures == 0)
    {
        std::printf("OK: all config-validation tests passed\n");
        return 0;
    }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
