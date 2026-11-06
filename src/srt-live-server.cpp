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

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/resource.h>
#include <time.h>
#include <httplib.h>
#include "spdlog/spdlog.h"

using namespace std;
using namespace httplib;

#include <nlohmann/json.hpp>
#include "SLSLog.hpp"
#include "SLSManager.hpp"
#include "SLSGenerationSet.hpp"
#include "AsyncHttpClient.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <chrono>

using json = nlohmann::json;

/*
 * Signal handlers. Only async-signal-safe operations are allowed here
 * (POSIX.1-2008 §2.4.3). spdlog and sls_remove_pid (unlink + free) are
 * not on the safe list, and a plain bool write has no atomicity guarantee
 * across the signal boundary. Store flags as volatile sig_atomic_t and
 * let the main loop observe them, log, and clean up.
 */
static volatile sig_atomic_t b_exit = 0;
static void ctrl_c_handler(int s)
{
    (void)s;
    b_exit = 1;
}

static volatile sig_atomic_t b_reload = 0;
static void reload_handler(int s)
{
    (void)s;
    b_reload = 1;
}

// Constant-time comparison for API key checks. The naive == short-circuits
// on the first differing byte and leaks the matched prefix length via timing.
// Iterate over the longer of the two strings and OR in every difference so
// the loop cost does not vary with the input on equal-length candidates.
static bool sls_ct_equal(const std::string &a, const std::string &b)
{
    size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned char diff = (unsigned char)(a.size() ^ b.size());
    for (size_t i = 0; i < n; i++)
    {
        unsigned char ca = i < a.size() ? (unsigned char)a[i] : 0;
        unsigned char cb = i < b.size() ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

Server svr;

// Every live manager generation: the current one, plus any retired by a
// SIGHUP reload that still serve the readers they had. The main thread
// publishes and retires them; HTTP handlers only take snapshots. See
// SLSGenerationSet.hpp for the lifetime rules.
static CSLSGenerationSet<CSLSManager> g_managers;

static bool sls_api_key_authorized(const sls_conf_srt_t *conf_srt, const Request &req)
{
    if (conf_srt == nullptr || conf_srt->api_keys.empty() || !req.has_header("Authorization"))
        return false;
    const std::string auth_header = req.get_header_value("Authorization");
    bool authorized = false;
    // No early exit: every configured key is compared.
    for (const auto &key : conf_srt->api_keys)
        authorized |= sls_ct_equal(key, auth_header);
    return authorized;
}

// The log pipeline's health, exported so the drop counter and the security
// event counts reach monitoring even when the log lines themselves were
// dropped.
static json sls_log_health_json()
{
    const sls_log_health_t health = sls_get_log_health();
    json events = json::object();
    events["authRefusals"] = health.security_events[static_cast<int>(SLSSecurityEvent::AUTH_REFUSAL)];
    events["admissionRefusals"] = health.security_events[static_cast<int>(SLSSecurityEvent::ADMISSION_REFUSAL)];
    events["kicks"] = health.security_events[static_cast<int>(SLSSecurityEvent::KICK)];
    events["adminDisconnects"] = health.security_events[static_cast<int>(SLSSecurityEvent::ADMIN_DISCONNECT)];

    json out = json::object();
    out["droppedMessages"] = health.dropped;
    out["securityDroppedMessages"] = health.security_dropped;
    out["suppressedRefusals"] = health.suppressed_refusals;
    out["sinkStalled"] = health.stalled;
    out["sinkStallEvents"] = health.stall_events;
    out["securityEvents"] = events;
    return out;
}

// A /disconnect-player session id: 1-20 decimal digits, non-zero, no sign,
// no whitespace, no overflow.
static bool sls_parse_session_id(const std::string &text, uint64_t &out)
{
    if (text.empty() || text.size() > 20)
        return false;
    uint64_t value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
            return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (value == 0)
        return false;
    out = value;
    return true;
}

// A /disconnect-player stream key: non-empty, bounded, no control bytes.
static bool sls_valid_stream_param(const std::string &stream)
{
    if (stream.empty() || stream.size() >= URL_MAX_LEN)
        return false;
    return std::none_of(stream.begin(), stream.end(),
                        [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}

/**
 * usage information
 */
#define BANNER_WIDTH 40
#define VERSION_STRING "v" SLS_VERSION
static void usage()
{
    spdlog::info("{:-<{}}", "", BANNER_WIDTH);
    spdlog::info("{: ^{}}", "irl-srt-server", BANNER_WIDTH);
    spdlog::info("{: ^{}}", VERSION_STRING, BANNER_WIDTH);
    spdlog::info("{: ^{}}", "Based on srt-live-server", BANNER_WIDTH);
    spdlog::info("{: ^{}}", "Modified by IRLServer (https://github.com/irlserver/irl-srt-server)", BANNER_WIDTH);
    spdlog::info("{:-<{}}", "", BANNER_WIDTH);
}

// add new parameter here
static sls_conf_cmd_t conf_cmd_opt[] = {
    SLS_SET_OPT(string, c, conf_file_name, "conf file name", 1, 1023),
    SLS_SET_OPT(string, s, c_cmd, "cmd: reload", 1, 1023),
    SLS_SET_OPT(string, l, log_level, "log level: fatal/error/warning/info/debug/trace", 1, 1023),
    //  SLS_SET_OPT(int, x, xxx,          "", 1, 100),//example
};

// The socket is bound on the main thread before this starts, so svr.stop()
// at shutdown always has a listening socket to close.
void httpWorker()
{
    svr.listen_after_bind();
}

bool file_exists(const char *path)
{
    return access(path, R_OK) == 0;
}

int main(int argc, char *argv[])
{
    struct sigaction sigIntHandler;
    struct sigaction sigHupHandler;
    struct sigaction sigPipeHandler;
    sls_opt_t sls_opt;

    // A closed stdout or log pipe must cost log lines, not the process: with
    // SIGPIPE ignored a write to it fails with EPIPE, which the log sinks
    // tolerate. Set before the logging threads start so they inherit it.
    memset(&sigPipeHandler, 0, sizeof(sigPipeHandler));
    sigPipeHandler.sa_handler = SIG_IGN;
    sigemptyset(&sigPipeHandler.sa_mask);
    sigaction(SIGPIPE, &sigPipeHandler, nullptr);

    // Load the time zone once, before any thread exists, so later
    // localtime_r calls on the worker and log threads only read it.
    tzset();
    initialize_logger();

    std::shared_ptr<CSLSManager> sls_manager;
    std::thread http_thread;
    std::shared_ptr<std::shared_future<AsyncHttpResponse>> stat_future;
    int64_t last_stat_post_time = 0;
    std::string stat_post_url;
    int stat_post_interval = 0;

    int ret = SLS_OK;
    int httpPort = 8181;
    std::string httpBindAddr = "::"; // all interfaces by default (historical behavior)
    char cors_header[URL_MAX_LEN] = "*";
    sls_conf_srt_t *conf_srt = NULL;

    usage();

    // parse cmd line
    memset(&sls_opt, 0, sizeof(sls_opt));
    if (argc > 1)
    {
        // parset argv
        int cmd_size = sizeof(conf_cmd_opt) / sizeof(sls_conf_cmd_t);
        ret = sls_parse_argv(argc, argv, &sls_opt, conf_cmd_opt, cmd_size);
        if (ret != SLS_OK)
        {
            return SLS_ERROR;
        }
    }

    // reload
    if (strcmp(sls_opt.c_cmd, "") != 0)
    {
        return sls_send_cmd(sls_opt.c_cmd);
    }

    // log level
    if (strlen(sls_opt.log_level) > 0)
    {
        sls_set_log_level(sls_opt.log_level);
    }

    // Test erro info...
    // CSLSSrt::libsrt_print_error_info();

    // ctrl + c to exit
    sigIntHandler.sa_handler = ctrl_c_handler;
    sigemptyset(&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;
    sigaction(SIGINT, &sigIntHandler, 0);

    // hup to reload
    sigHupHandler.sa_handler = reload_handler;
    sigemptyset(&sigHupHandler.sa_mask);
    sigHupHandler.sa_flags = 0;
    sigaction(SIGHUP, &sigHupHandler, 0);
    sigaction(SIGTERM, &sigIntHandler, 0);

    // init srt
    CSLSSrt::libsrt_init();

    // parse conf file
    if (strlen(sls_opt.conf_file_name) == 0)
    {
        const char *search_paths[] = {"/etc/sls/sls.conf", "/usr/local/etc/sls/sls.conf", "/usr/etc/sls/sls.conf",
                                      "./sls.conf"};

        bool found = false;
        for (auto &path : search_paths)
        {
            if (file_exists(path))
            {
                snprintf(sls_opt.conf_file_name, sizeof(sls_opt.conf_file_name), "%s", path);
                found = true;
                break;
            }
        }

        if (!found)
        {
            spdlog::critical("No configuration file found in standard paths.");
            goto EXIT_PROC;
        }
    }
    ret = sls_conf_open(sls_opt.conf_file_name);
    if (ret != SLS_OK)
    {
        spdlog::critical("Could not read configuration file, exiting.");
        goto EXIT_PROC;
    }

    // Raise the open-file-descriptor ceiling before binding listeners. A busy
    // server holds an fd per SRT socket, relay and epoll instance, so the
    // distro default soft limit can be exhausted under load. Best-effort and
    // never fatal; bounded by the hard limit when unprivileged.
    {
        sls_conf_srt_t *nofile_conf = (sls_conf_srt_t *)sls_conf_get_root_conf();
        rlim_t desired = (nofile_conf && nofile_conf->nofile_limit > 0) ? (rlim_t)nofile_conf->nofile_limit : 65536;
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        {
            spdlog::warn("getrlimit(RLIMIT_NOFILE) failed (errno={}); leaving fd limit unchanged.", errno);
        }
        else
        {
            rlim_t target = desired;
            if (rl.rlim_max != RLIM_INFINITY && target > rl.rlim_max)
                target = rl.rlim_max;
            if (rl.rlim_cur >= target)
            {
                spdlog::info("RLIMIT_NOFILE soft limit already {} (>= requested {}).", (unsigned long long)rl.rlim_cur,
                             (unsigned long long)desired);
            }
            else
            {
                rlim_t previous = rl.rlim_cur;
                rl.rlim_cur = target;
                if (setrlimit(RLIMIT_NOFILE, &rl) == 0)
                    spdlog::info("RLIMIT_NOFILE soft limit raised {} -> {} (requested {}, hard {}).",
                                 (unsigned long long)previous, (unsigned long long)target, (unsigned long long)desired,
                                 (unsigned long long)rl.rlim_max);
                else
                    spdlog::warn("Could not raise RLIMIT_NOFILE to {} (errno={}); staying at {}.",
                                 (unsigned long long)target, errno, (unsigned long long)previous);
            }
        }
    }

    sls_load_pid_filename();
    if (0 != sls_write_pid(getpid()))
    {
        spdlog::critical("Could not write PID file, exiting.");
        goto EXIT_PROC;
    }

    // sls manager
    spdlog::info("SRT Live Server is running...");

    sls_manager = std::make_shared<CSLSManager>();
    if (SLS_OK != sls_manager->start())
    {
        spdlog::critical("sls_manager->start failed, exiting.");
        goto EXIT_PROC;
    }
    g_managers.publish(sls_manager);

    conf_srt = (sls_conf_srt_t *)sls_conf_get_root_conf();

    // Drop privileges after listeners have bound (so :<1024 ports still work
    // if the admin configured one) but before we start handling traffic.
    if (SLS_OK != sls_drop_privileges(conf_srt->user, conf_srt->group))
    {
        spdlog::critical("sls_drop_privileges failed, exiting.");
        goto EXIT_PROC;
    }

    ret = strnlen(conf_srt->stat_post_url, URL_MAX_LEN);
    if (ret >= URL_MAX_LEN)
    {
        spdlog::critical("stat_post_url is too long, exiting.");
        goto EXIT_PROC;
    }
    else if (ret > 0)
    {
        stat_post_url = conf_srt->stat_post_url;
        stat_post_interval = conf_srt->stat_post_interval;
        last_stat_post_time = sls_gettime_ms();
    }

    if (strlen(conf_srt->cors_header) > 0)
    {
        strcpy(cors_header, conf_srt->cors_header);
    }

    // Lightweight liveness/readiness probe. Returns 200 OK unconditionally
    // as long as the HTTP server is responsive — does NOT iterate publishers
    // or take any data-path locks, unlike /stats. Kubernetes probes should
    // target this endpoint to avoid blocking the publisher map_data write
    // lock every probe interval, which manifests as periodic msRcvBuf
    // spikes on healthy streams.
    svr.Get("/healthz",
            [&](const Request &req, Response &res)
            {
                (void)req;
                res.status = 200;
                res.set_header("Cache-Control", "no-cache");
                res.set_content("{\"status\":\"ok\"}", "application/json");
            });

    svr.Get("/stats",
            [&](const Request &req, Response &res)
            {
                json ret;
                // A snapshot: the manager and its configuration generation stay
                // alive for this whole request, even across a reload.
                std::shared_ptr<CSLSManager> manager = g_managers.current();
                std::shared_ptr<sls_conf_base_t> conf_root = manager ? manager->conf_generation() : nullptr;
                sls_conf_srt_t *conf_srt = (sls_conf_srt_t *)conf_root.get();

                if (!manager || !conf_srt)
                { // Check config ptr too
                    ret["status"] = "error";
                    ret["message"] = "Internal server error: manager or config not available";
                    res.status = 500;
                    res.set_header("Access-Control-Allow-Origin", cors_header);
                    res.set_content(ret.dump(), "application/json");
                    return;
                }

                int clear = req.has_param("reset") ? 1 : 0;
                auto is_authorized = [&]() -> bool { return sls_api_key_authorized(conf_srt, req); };

                if (clear && !is_authorized())
                {
                    ret["status"] = "error";
                    ret["message"] = "Unauthorized: API key required or invalid for reset.";
                    res.status = 401;
                    res.set_header("Access-Control-Allow-Origin", cors_header);
                    res.set_content(ret.dump(), "application/json");
                    return;
                }

                // If publisher param exists, use old logic
                if (req.has_param("publisher"))
                {
                    ret = manager->generate_json_for_publisher(req.get_param_value("publisher"), clear);
                    if (ret["status"] == "error")
                    {
                        res.status = 404; // Not Found
                    }
                    else if (is_authorized())
                    {
                        ret["logging"] = sls_log_health_json();
                    }
                }
                else
                {
                    // Publisher param missing: List all publishers if API key is configured
                    if (is_authorized())
                    {
                        ret = manager->generate_json_for_all_publishers(clear);
                        ret["logging"] = sls_log_health_json();
                        // Status should already be 'ok' from generate_json_for_all_publishers
                        // No need to set 404 here, as we are listing all (even if empty)
                    }
                    else
                    {
                        ret["status"] = "error";
                        ret["message"] = "Unauthorized: API key required or invalid.";
                        res.status = 401; // Unauthorized
                    }
                }

                res.set_header("Access-Control-Allow-Origin", cors_header);
                res.set_content(ret.dump(), "application/json");
            });

    svr.Post("/disconnect",
             [&](const Request &req, Response &res)
             {
                 json ret;
                 // The current generation first, then any still draining after
                 // a reload: a publisher that connected before the reload is
                 // still kickable.
                 std::vector<std::shared_ptr<CSLSManager>> generations = g_managers.all();
                 std::shared_ptr<sls_conf_base_t> conf_root =
                     generations.empty() ? nullptr : generations.front()->conf_generation();
                 sls_conf_srt_t *conf_srt = (sls_conf_srt_t *)conf_root.get();

                 if (generations.empty() || !conf_srt)
                 {
                     ret["status"] = "error";
                     ret["message"] = "Internal server error: manager or config not available";
                     res.status = 500;
                     res.set_header("Access-Control-Allow-Origin", cors_header);
                     res.set_content(ret.dump(), "application/json");
                     return;
                 }

                 // Check if stream parameter is provided
                 if (!req.has_param("stream"))
                 {
                     ret["status"] = "error";
                     ret["message"] = "Missing 'stream' parameter";
                     res.status = 400; // Bad Request
                     res.set_header("Access-Control-Allow-Origin", cors_header);
                     res.set_content(ret.dump(), "application/json");
                     return;
                 }

                 // Check authorization with API key (none configured => denied)
                 bool authorized = sls_api_key_authorized(conf_srt, req);

                 if (!authorized)
                 {
                     ret["status"] = "error";
                     ret["message"] = "Unauthorized: API key required or invalid.";
                     res.status = 401; // Unauthorized
                     res.set_header("Access-Control-Allow-Origin", cors_header);
                     res.set_content(ret.dump(), "application/json");
                     return;
                 }

                 // Disconnect the stream
                 std::string stream_name = req.get_param_value("stream");
                 for (const auto &generation : generations)
                 {
                     ret = generation->disconnect_stream(stream_name);
                     if (ret["status"] == "ok")
                         break;
                 }

                 if (ret["status"] == "error")
                 {
                     res.status = 404; // Not Found
                 }

                 res.set_header("Access-Control-Allow-Origin", cors_header);
                 res.set_content(ret.dump(), "application/json");
             });

    // Kick exactly one player. The session id comes from the on_connect hook
    // (session_id) or /stats (players[].sessionId) and is unique for the life
    // of the process; stream and session must BOTH match. A separate route
    // from /disconnect, so a request that omits a parameter can never fall
    // through to a publisher kick. Responses never echo the stream key: it
    // can contain a publish credential.
    svr.Post("/disconnect-player",
             [&](const Request &req, Response &res)
             {
                 json ret;
                 res.set_header("Access-Control-Allow-Origin", cors_header);
                 auto reply = [&](int status, const char *message)
                 {
                     ret["status"] = status == 200 ? "ok" : "error";
                     ret["message"] = message;
                     res.status = status;
                     res.set_content(ret.dump(), "application/json");
                 };

                 std::vector<std::shared_ptr<CSLSManager>> generations = g_managers.all();
                 std::shared_ptr<sls_conf_base_t> conf_root =
                     generations.empty() ? nullptr : generations.front()->conf_generation();
                 const sls_conf_srt_t *conf_srt = (const sls_conf_srt_t *)conf_root.get();
                 if (generations.empty() || conf_srt == nullptr)
                     return reply(500, "Internal server error: manager or config not available");

                 if (!sls_api_key_authorized(conf_srt, req))
                     return reply(401, "Unauthorized: API key required or invalid.");

                 if (req.get_param_value_count("stream") != 1 || req.get_param_value_count("session") != 1)
                     return reply(400, "Exactly one 'stream' and one 'session' parameter are required");
                 const std::string stream_key = req.get_param_value("stream");
                 if (!sls_valid_stream_param(stream_key))
                     return reply(400, "Invalid 'stream' parameter");
                 uint64_t session_id = 0;
                 if (!sls_parse_session_id(req.get_param_value("session"), session_id))
                     return reply(400, "Invalid 'session' parameter: expected a positive decimal integer");

                 for (const auto &generation : generations)
                 {
                     json result = generation->disconnect_player(stream_key, session_id);
                     if (result["status"] == "ok")
                     {
                         ret["session"] = session_id;
                         return reply(200, "Player disconnect requested");
                     }
                 }
                 return reply(404, "Player session not found");
             });

    if (conf_srt->http_port)
    {
        httpPort = conf_srt->http_port;
    }
    if (strlen(conf_srt->http_bind_addr) > 0)
    {
        httpBindAddr = conf_srt->http_bind_addr;
    }
    if (svr.bind_to_port(httpBindAddr, httpPort))
    {
        spdlog::info("HTTP control plane listening on {}:{}", httpBindAddr, httpPort);
        http_thread = std::thread(httpWorker);
    }
    else
    {
        spdlog::error("HTTP control plane could not bind {}:{}; continuing without it.", httpBindAddr, httpPort);
    }

    while (!b_exit)
    {
        int64_t cur_tm_ms = sls_gettime_ms();
        ret = 0;
        if (sls_manager->is_single_thread())
        {
            ret = sls_manager->single_thread_handler();
        }

        // Log-sink stall detection and the periodic drop/suppression summary.
        sls_log_watchdog_tick();

        // Check if we should log summary
        if (sls_get_log_config().summary_enabled)
        {
            std::string summary_msg;
            if (sls_get_summary_logger().should_log_summary(sls_get_log_config().summary_interval_sec, summary_msg))
            {
                spdlog::info(summary_msg);
            }
        }
        if (!stat_post_url.empty() && stat_post_interval > 0)
        {
            if (stat_future && stat_future->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                auto response = stat_future->get();
                stat_future = nullptr;
                if (!response.success)
                    spdlog::warn("Stats POST failed: {}", response.error);
            }

            if (!stat_future && (cur_tm_ms - last_stat_post_time >= stat_post_interval))
            {
                std::string stats_json = sls_manager->get_stat_info();
                auto future = AsyncHttpClient::instance().post_async(stat_post_url, stats_json, "application/json", 10);
                stat_future = std::make_shared<std::shared_future<AsyncHttpResponse>>(std::move(future));
                last_stat_post_time = cur_tm_ms;
            }
        }

        msleep(10);

        /*for test reload...
        int64_t tm_cur = sls_gettime();
        int64_t d = tm_cur - tm;
        if ( d >= 10000000) {
            b_reload = !b_reload;
            tm = tm_cur;
            printf("\n\n\n\n");
        }
        //*/

        // Check reloaded managers. A drained one leaves the set first, so no
        // new HTTP snapshot can pick it up; stop() then waits for any request
        // already using it. The memory goes when the last snapshot drops its
        // reference, on whichever thread that is.
        for (const auto &manager : g_managers.retiring())
        {
            if (SLS_OK == manager->check_invalid())
            {
                spdlog::info("Checking reloaded manager, deleting manager={:p} ...", fmt::ptr(manager.get()));
                g_managers.remove(manager);
                manager->stop();
            }
        }

        if (b_reload)
        {
            // Reload
            b_reload = 0;
            spdlog::info("Reloading SRT Live Server...");
            ret = sls_manager->reload();
            if (ret != SLS_OK)
            {
                spdlog::error("Reload failed [sls_manager->reload failed]");
                continue;
            }
            g_managers.retire_current();
            sls_manager.reset();
            spdlog::info("Pushing old sls_manager to list.");

            // Do NOT free the old config tree here. The manager just retired is
            // still draining and its roles/listeners/relays still hold raw
            // sls_conf_* pointers into the current generation. The tree is
            // reference-counted and owned by each CSLSManager (see
            // CSLSManager::start / m_conf_generation): sls_conf_open() below
            // publishes a NEW generation while the old one stays alive until the
            // retiring manager is destroyed by the check_invalid() sweep above.
            // Calling sls_conf_close() here was the SIGHUP-reload use-after-free.
            ret = sls_conf_open(sls_opt.conf_file_name);
            if (ret != SLS_OK)
            {
                spdlog::critical("Reload failed (could not read config file)");
                break;
            }
            spdlog::info("Successfuly reloaded config file.");
            // Re-point the cached root conf at the new generation; the old tree
            // it referenced is now owned solely by the retiring manager.
            conf_srt = (sls_conf_srt_t *)sls_conf_get_root_conf();

            spdlog::info("Reloading PID file location (if needed)");
            if (sls_reload_pid() != SLS_OK)
            {
                spdlog::critical("Reload recreate PID file");
                break;
            }

            sls_manager = std::make_shared<CSLSManager>();
            if (SLS_OK != sls_manager->start())
            {
                spdlog::critical("Reload failed [sls_manager->start]");
                break;
            }
            g_managers.publish(sls_manager);
            if (strlen(conf_srt->stat_post_url) > 0)
            {
                stat_post_url = conf_srt->stat_post_url;
                stat_post_interval = conf_srt->stat_post_interval;
                last_stat_post_time = sls_gettime_ms();
                stat_future = nullptr;
            }

            spdlog::info("Reloaded successfully.");
        }
    }

EXIT_PROC:
    spdlog::info("Stopping SRT Live Server...");

    // Stop the control plane first and wait for its in-flight requests, so no
    // handler is still running when the managers below are stopped.
    if (http_thread.joinable())
    {
        svr.wait_until_ready();
        svr.stop();
        http_thread.join();
        spdlog::info("HTTP control plane stopped");
    }

    {
        // The current manager may not be in the set: one whose start() failed
        // was never published.
        std::vector<std::shared_ptr<CSLSManager>> generations = g_managers.clear();
        generations.erase(std::remove(generations.begin(), generations.end(), sls_manager), generations.end());
        if (sls_manager)
        {
            sls_manager->stop();
            sls_manager.reset();
            spdlog::info("Released sls_manager");
        }

        // release all reload manager
        spdlog::info("Releasing reload_manager_list, count={:d}.", generations.size());
        for (const auto &manager : generations)
        {
            manager->stop();
        }
        spdlog::info("Released reload_manager_list");
    }

    stat_future = nullptr;

    // uninit srt
    spdlog::info("Destroy SRT objects");
    CSLSSrt::libsrt_uninit();

    spdlog::info("Closing configuration file");
    sls_conf_close();

    spdlog::info("Removing PID file");
    sls_remove_pid();

    spdlog::info("Execution finished, goodbye.");

    // The log threads flush their queues when they are joined at exit. If a
    // sink is blocked (a stdout nobody reads), that join would never return;
    // exit without it, since the remaining lines cannot be written anyway.
    if (sls_log_is_stalled())
        std::_Exit(0);

    return 0;
}
