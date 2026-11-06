#include "doctest.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "conf.hpp"
#include "SLSListener.hpp"
#include "SLSManager.hpp"
#include "SLSPublisher.hpp"

// Before the fix sls_conf_release deleted every node through sls_conf_base_t *.
// ASan reports that as new-delete-type-mismatch, and the std::vector members of
// the srt and app blocks were never destroyed, so the sanitizer legs aborted the
// first time a server shut down.
TEST_CASE("conf: every block type registers a typed destroy function")
{
    CHECK(sls_conf_srt_t::runtime_conf.destroy_fn != nullptr);
    CHECK(sls_conf_server_t::runtime_conf.destroy_fn != nullptr);
    CHECK(sls_conf_app_t::runtime_conf.destroy_fn != nullptr);
}

TEST_CASE("conf: a parsed tree is released through each node's own type")
{
    char path[] = "/tmp/sls_conf_release_XXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    const std::string conf = "srt {\n"
                             "    worker_threads 1;\n"
                             "    api_keys first-key,second-key;\n"
                             "    server {\n"
                             "        listen_player 5990;\n"
                             "        listen_publisher 5991;\n"
                             "        domain_player play;\n"
                             "        domain_publisher publish;\n"
                             "        app {\n"
                             "            app_player live;\n"
                             "            app_publisher live;\n"
                             "            allow publish all;\n"
                             "            allow play all;\n"
                             "        }\n"
                             "    }\n"
                             "}\n";
    REQUIRE(write(fd, conf.data(), conf.size()) == static_cast<ssize_t>(conf.size()));
    close(fd);

    REQUIRE(sls_conf_open(path) == SLS_OK);
    auto *root = reinterpret_cast<sls_conf_srt_t *>(sls_conf_get_root_conf());
    REQUIRE(root != nullptr);
    CHECK(root->api_keys.size() == 2);
    auto *server = reinterpret_cast<sls_conf_server_t *>(root->child);
    REQUIRE(server != nullptr);
    auto *app = reinterpret_cast<sls_conf_app_t *>(server->child);
    REQUIRE(app != nullptr);
    CHECK(app->ip_actions.publish.size() == 1);

    // The last reference frees the tree. Under ASan a base-typed delete here
    // aborts the test binary.
    sls_conf_close();
    CHECK(sls_conf_get_root_conf() == nullptr);
    unlink(path);
}
