// SRT test client for the e2e harness (tests/e2e/*.sh). Not shipped.
//
// Reader mode (default): connects --sockets SRT callers to one stream and
// records what each receives into <out>.<i>.ts. With --bind-port every caller
// is bound to the same local UDP port, so the server sees them all on one peer
// ip:port: the case where only a session id can tell them apart. With
// --bind-addr the callers send from that local address (another 127.x
// address looks like another peer to the server). With
// --wait-stdin the next caller connects only after a line arrives on stdin, so
// a harness can pair each caller with the session id its on_connect hook
// carried. One JSON object per line on stdout:
//   {"sock":0,"event":"connected","t_ms":12}
//   {"sock":0,"event":"bytes","total":65800,"t_ms":512}   (at most every 200 ms)
//   {"sock":0,"event":"closed","total":131600,"t_ms":2048}
//   {"event":"done","t_ms":10000}
//
// Flood mode (--flood N): each of --sockets threads makes N connect attempts,
// closing every connection at once, to drive refusal paths. Prints
//   {"event":"flood","attempts":N,"connected":M}
#include <srt/srt.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <syslog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct Options
{
    std::string host = "127.0.0.1";
    int port = 0;
    std::string streamid;
    std::string out = "reader";
    int sockets = 1;
    int bind_port = 0;
    std::string bind_addr;
    int duration_ms = 10000;
    int latency_ms = 200;
    bool wait_stdin = false;
    int flood = 0;
};

std::mutex g_print_mutex;
const auto g_start = std::chrono::steady_clock::now();

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
}

void emit(const std::string &json)
{
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::printf("%s\n", json.c_str());
    std::fflush(stdout);
}

bool parse(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        const char *v = nullptr;
        if (a == "--wait-stdin")
        {
            o.wait_stdin = true;
            continue;
        }
        if ((v = next()) == nullptr)
            return false;
        if (a == "--host")
            o.host = v;
        else if (a == "--port")
            o.port = std::atoi(v);
        else if (a == "--streamid")
            o.streamid = v;
        else if (a == "--out")
            o.out = v;
        else if (a == "--sockets")
            o.sockets = std::atoi(v);
        else if (a == "--bind-port")
            o.bind_port = std::atoi(v);
        else if (a == "--bind-addr")
            o.bind_addr = v;
        else if (a == "--duration-ms")
            o.duration_ms = std::atoi(v);
        else if (a == "--latency")
            o.latency_ms = std::atoi(v);
        else if (a == "--flood")
            o.flood = std::atoi(v);
        else
            return false;
    }
    return o.port > 0 && o.port < 65536 && !o.streamid.empty() && o.sockets > 0;
}

SRTSOCKET open_caller(const Options &o, std::string &error)
{
    SRTSOCKET s = srt_create_socket();
    if (s == SRT_INVALID_SOCK)
    {
        error = srt_getlasterror_str();
        return s;
    }
    int latency = o.latency_ms;
    srt_setsockflag(s, SRTO_LATENCY, &latency, sizeof(latency));
    srt_setsockflag(s, SRTO_STREAMID, o.streamid.c_str(), static_cast<int>(o.streamid.size()));
    int rcv_timeout = 200;
    srt_setsockflag(s, SRTO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    if (o.bind_port > 0 || !o.bind_addr.empty())
    {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(static_cast<uint16_t>(o.bind_port));
        inet_pton(AF_INET, o.bind_addr.empty() ? o.host.c_str() : o.bind_addr.c_str(), &local.sin_addr);
        if (srt_bind(s, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SRT_ERROR)
        {
            error = srt_getlasterror_str();
            srt_close(s);
            return SRT_INVALID_SOCK;
        }
    }
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(static_cast<uint16_t>(o.port));
    inet_pton(AF_INET, o.host.c_str(), &remote.sin_addr);
    if (srt_connect(s, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) == SRT_ERROR)
    {
        error = srt_getlasterror_str();
        srt_close(s);
        return SRT_INVALID_SOCK;
    }
    return s;
}

int run_flood(const Options &o)
{
    std::atomic<long> attempts{0};
    std::atomic<long> connected{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < o.sockets; t++)
    {
        threads.emplace_back(
            [&]
            {
                for (int i = 0; i < o.flood; i++)
                {
                    std::string error;
                    SRTSOCKET s = open_caller(o, error);
                    attempts.fetch_add(1);
                    if (s != SRT_INVALID_SOCK)
                    {
                        connected.fetch_add(1);
                        srt_close(s);
                    }
                }
            });
    }
    for (auto &t : threads)
        t.join();
    emit("{\"event\":\"flood\",\"attempts\":" + std::to_string(attempts.load()) +
         ",\"connected\":" + std::to_string(connected.load()) + "}");
    return 0;
}

void read_socket(int index, SRTSOCKET s, const Options &o, long long deadline_ms)
{
    const std::string path = o.out + "." + std::to_string(index) + ".ts";
    FILE *file = std::fopen(path.c_str(), "wb");
    char buf[1500];
    long long total = 0;
    long long last_report = 0;
    while (now_ms() < deadline_ms)
    {
        int n = srt_recvmsg(s, buf, sizeof(buf));
        if (n > 0)
        {
            if (file)
                std::fwrite(buf, 1, static_cast<size_t>(n), file);
            total += n;
            if (now_ms() - last_report >= 200)
            {
                last_report = now_ms();
                if (file)
                    std::fflush(file);
                emit("{\"sock\":" + std::to_string(index) + ",\"event\":\"bytes\",\"total\":" +
                     std::to_string(total) + ",\"t_ms\":" + std::to_string(last_report) + "}");
            }
            continue;
        }
        // A receive timeout (no media yet, e.g. while authorization is
        // pending) is not a close: keep reading while the socket is up.
        const int err = srt_getlasterror(nullptr);
        if ((err == SRT_ETIMEOUT || err == SRT_EASYNCRCV) && srt_getsockstate(s) == SRTS_CONNECTED)
            continue;
        break;
    }
    if (file)
        std::fclose(file);
    emit("{\"sock\":" + std::to_string(index) + ",\"event\":\"closed\",\"total\":" + std::to_string(total) +
         ",\"t_ms\":" + std::to_string(now_ms()) + "}");
    srt_close(s);
}

} // namespace

int main(int argc, char **argv)
{
    Options o;
    if (!parse(argc, argv, o))
    {
        std::fprintf(stderr, "usage: %s --port P --streamid SID [--host H] [--out PREFIX] [--sockets N] "
                             "[--bind-port B] [--bind-addr A] [--duration-ms D] [--latency L] [--wait-stdin] [--flood N]\n",
                     argv[0]);
        return 2;
    }
    srt_startup();
    srt_setloglevel(LOG_CRIT);
    if (o.flood > 0)
    {
        int rc = run_flood(o);
        srt_cleanup();
        return rc;
    }

    const long long deadline = now_ms() + o.duration_ms;
    std::vector<std::thread> readers;
    int rc = 0;
    for (int i = 0; i < o.sockets; i++)
    {
        if (i > 0 && o.wait_stdin)
        {
            std::string line;
            if (!std::getline(std::cin, line))
                break;
        }
        std::string error;
        SRTSOCKET s = open_caller(o, error);
        if (s == SRT_INVALID_SOCK)
        {
            emit("{\"sock\":" + std::to_string(i) + ",\"event\":\"refused\",\"t_ms\":" + std::to_string(now_ms()) +
                 "}");
            std::fprintf(stderr, "socket %d: %s\n", i, error.c_str());
            rc = 1;
            continue;
        }
        emit("{\"sock\":" + std::to_string(i) + ",\"event\":\"connected\",\"t_ms\":" + std::to_string(now_ms()) +
             "}");
        readers.emplace_back(read_socket, i, s, std::cref(o), deadline);
    }
    for (auto &t : readers)
        t.join();
    emit("{\"event\":\"done\",\"t_ms\":" + std::to_string(now_ms()) + "}");
    srt_cleanup();
    return rc;
}
