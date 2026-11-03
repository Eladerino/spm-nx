// Host-side loopback test of the debug server: the vendored core (../source/debug_server.cpp)
// and spm-nx's command file (../source/spm_debug_server.cpp), compiled as they ship, with the
// diag switches, the map sweep and the keep-awake switch faked below. It boots the real server
// on 127.0.0.1:6543 against a fake SD root (SPM_DEBUG_ROOT: UserData/Logs/stderr.log and
// crash.txt in it) and drives it over a socket: every command the build implements, the log
// stream (backlog first, then live lines the pump tails out of the fake log file), input
// injection state, and the stubs' "not in this build" answers.
//   c++ -std=c++20 -I../source debug_server_tests.cpp ../source/debug_server.cpp
//       ../source/spm_debug_server.cpp -o t -pthread && ./t
#include "debug_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>

// ---- the fakes (the adapter declares these; the console build links the real ones) -----------

namespace {
std::map<std::string, bool> g_switches = {{"depth_off", false}, {"perftimer", false}, {"dkcmd", false},
                                          {"gxthread", true},   {"zcull", true},     {"dkserver", true}};
int g_setCalls = 0;
int g_flushCalls = 0;
std::string g_warpMap, g_warpDoor;
bool g_warpCalled = false;
}  // namespace

namespace swdk {
bool diag_set(const char* key, bool on) {
    auto it = g_switches.find(key);
    if (it == g_switches.end()) return false;
    it->second = on;
    ++g_setCalls;
    return true;
}
bool diag_get(const char* key) {
    auto it = g_switches.find(key);
    return it != g_switches.end() && it->second;
}
void diag_enumerate(void (*visit)(const char* name, bool on, void* user), void* user) {
    for (auto& [name, on] : g_switches) visit(name.c_str(), on, user);
}
void diag_flush_all(const char*) { ++g_flushCalls; }
uint64_t diag_present_count() { return 4242; }
void log_flush() {}
}  // namespace swdk

namespace SpmMapSweep {
void RequestWarp(const char* map, const char* door, const float*) {
    g_warpCalled = true;
    g_warpMap = map ? map : "";
    g_warpDoor = door ? door : "";
}
void CurrentPlace(char* out, size_t size) { std::snprintf(out, size, "he1_01 2D"); }
int Seq() { return 2; }
}  // namespace SpmMapSweep

namespace {
bool g_awake = false;
int g_awakeCalls = 0;
}  // namespace
extern "C" void spm_switch_keep_awake(bool on) {
    g_awake = on;
    ++g_awakeCalls;
}

// the adapter's entry points (source/spm_debug_server.cpp)
extern "C" void spm_debug_server_boot(bool enabled);
extern "C" void spm_debug_server_stop(void);

// ---- a tiny framed client ---------------------------------------------------------------

namespace {
int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

struct Client {
    int fd = -1;
    std::string in;
    bool Fill(int timeoutMs) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, timeoutMs) <= 0) return false;
        char buf[16384];
        const ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r <= 0) return false;
        in.append(buf, size_t(r));
        return true;
    }
    // one framed reply: "ok <n>\n" / "err <n>\n" + n bytes
    bool Reply(bool& ok, std::string& payload, int timeoutMs = 3000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        size_t nl;
        while ((nl = in.find('\n')) == std::string::npos) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0 || !Fill(int(left))) return false;
        }
        const std::string header = in.substr(0, nl);
        in.erase(0, nl + 1);
        ok = header.rfind("ok ", 0) == 0;
        if (!ok && header.rfind("err ", 0) != 0) return false;
        const size_t n = size_t(std::strtoull(header.c_str() + (ok ? 3 : 4), nullptr, 10));
        while (in.size() < n) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0 || !Fill(int(left))) return false;
        }
        payload = in.substr(0, n);
        in.erase(0, n);
        return true;
    }
    bool Send(const std::string& line) {
        const std::string s = line + "\n";
        return send(fd, s.data(), s.size(), 0) == ssize_t(s.size());
    }
    // for the log stream: accumulate until `want` shows up or the timeout ends
    bool StreamUntil(const std::string& want, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (in.find(want) == std::string::npos) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0 || !Fill(int(left))) return in.find(want) != std::string::npos;
        }
        return true;
    }
};

bool Connect(Client& c) {
    c.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c.fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(6543);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(c.fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(c.fd);
        c.fd = -1;
        return false;
    }
    return true;
}

bool RoundTrip(Client& c, const std::string& line, bool& ok, std::string& payload) {
    return c.Send(line) && c.Reply(ok, payload);
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("spm-debug-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "UserData" / "Logs");
    {
        std::ofstream(root / "UserData" / "Logs" / "stderr.log") << "boot line one\nboot line two\n";
        std::ofstream(root / "UserData" / "Logs" / "crash.txt") << "FAKE CRASH REPORT\nsecond line\n";
    }
    setenv("SPM_DEBUG_ROOT", root.c_str(), 1);

    // the gate: boot(false) starts nothing
    spm_debug_server_boot(false);
    CHECK(!debugsrv::running());
    {
        Client c;
        CHECK(!Connect(c));
    }

    spm_debug_server_boot(true);
    CHECK(debugsrv::running());
    CHECK(g_awake);  // keep-awake was turned on with the server

    Client c;
    bool connected = false;
    for (int i = 0; i < 30 && !connected; ++i) {
        connected = Connect(c);
        if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(connected);
    if (!connected) return 1;

    bool ok;
    std::string p;
    CHECK(RoundTrip(c, "ping", ok, p) && ok && p == "pong");
    CHECK(RoundTrip(c, "help", ok, p) && ok && p.find("press") != std::string::npos &&
          p.find("warp <map>") != std::string::npos && p.find("diag") != std::string::npos);
    CHECK(RoundTrip(c, "info", ok, p) && ok && p.find("game spm-nx") != std::string::npos &&
          p.find("place he1_01 2D") != std::string::npos && p.find("present 4242") != std::string::npos &&
          p.find(root.string()) != std::string::npos);

    // diag: list, get, set through both spellings, and the guards
    CHECK(RoundTrip(c, "diag", ok, p) && ok && p.find("perftimer=0") != std::string::npos &&
          p.find("zcull=1") != std::string::npos && p.find("dkserver=1") != std::string::npos);
    CHECK(RoundTrip(c, "diag get perftimer", ok, p) && ok && p == "perftimer=0\n");
    const int setsBefore = g_setCalls;
    CHECK(RoundTrip(c, "set perftimer 1", ok, p) && ok && g_setCalls == setsBefore + 1 &&
          g_switches["perftimer"]);
    CHECK(RoundTrip(c, "diag get perftimer", ok, p) && ok && p == "perftimer=1\n");
    CHECK(RoundTrip(c, "diag set perftimer 0", ok, p) && ok && !g_switches["perftimer"]);
    CHECK(RoundTrip(c, "diag set dkserver 0", ok, p) && !ok &&
          p.find("boot-time only") != std::string::npos && g_switches["dkserver"]);
    CHECK(RoundTrip(c, "diag set bogus 1", ok, p) && !ok &&
          p.find("unknown diag switch") != std::string::npos);
    CHECK(RoundTrip(c, "set perftimer 2", ok, p) && !ok);

    // flush and mark ride diag_flush_all, like dk_cmd's do
    const int flushesBefore = g_flushCalls;
    CHECK(RoundTrip(c, "flush", ok, p) && ok && g_flushCalls == flushesBefore + 1);
    CHECK(RoundTrip(c, "mark hello world", ok, p) && ok && g_flushCalls == flushesBefore + 2);

    // warp and place ride the map sweep
    CHECK(RoundTrip(c, "warp he1_01", ok, p) && ok && g_warpCalled && g_warpMap == "he1_01" &&
          g_warpDoor.empty());
    CHECK(RoundTrip(c, "warp", ok, p) && !ok);
    CHECK(RoundTrip(c, "place", ok, p) && ok && p == "he1_01 2D\n");

    // the core's file commands, rooted at the fake SD root
    CHECK(RoundTrip(c, "ls", ok, p) && ok && p.find("UserData") != std::string::npos);
    CHECK(RoundTrip(c, "get UserData/Logs/crash.txt", ok, p) && ok &&
          p == "FAKE CRASH REPORT\nsecond line\n");
    CHECK(RoundTrip(c, "get UserData/Logs/missing.txt", ok, p) && !ok);
    {
        // put: the payload follows the command line on the same connection
        CHECK(c.Send("put newfile.bin 5"));
        CHECK(send(c.fd, "hello", 5, 0) == 5);
        CHECK(c.Reply(ok, p) && ok && p.find("crc32 ") == 0 && p.find("size 5") != std::string::npos);
        CHECK(RoundTrip(c, "get newfile.bin", ok, p) && ok && p == "hello");
        CHECK(fs::exists(root / "newfile.bin") && !fs::exists(root / "newfile.bin.part"));
    }

    // the stubs answer err and say why
    for (const char* cmd : {"shot", "peek 80000000", "poke 80000000 0", "quit", "reload"}) {
        CHECK(RoundTrip(c, cmd, ok, p) && !ok && p.find("not in this build") != std::string::npos);
    }
    CHECK(RoundTrip(c, "frobnicate", ok, p) && !ok &&
          p.find("unknown command") != std::string::npos);

    // input injection state (the core's built-ins; the console build feeds it to the pad)
    CHECK(RoundTrip(c, "hold A", ok, p) && ok);
    CHECK((debugsrv::injected().buttons & 1ull) != 0);
    CHECK(RoundTrip(c, "release", ok, p) && ok);
    CHECK(debugsrv::injected().buttons == 0);

    // the log stream: backlog from the session's start, then live lines off the file
    Client stream;
    CHECK(Connect(stream));
    CHECK(stream.Send("log all"));
    {
        bool hok;
        std::string hp;
        CHECK(stream.Reply(hok, hp) && hok);  // the "ok 0" header, then the stream itself
    }
    CHECK(stream.StreamUntil("boot line one", 3000));
    CHECK(stream.StreamUntil("boot line two", 1000));
    {
        std::ofstream log(root / "UserData" / "Logs" / "stderr.log", std::ios::app);
        log << "live line three\n";
    }
    CHECK(stream.StreamUntil("live line three", 3000));
    close(stream.fd);

    close(c.fd);
    spm_debug_server_stop();
    CHECK(!debugsrv::running());
    CHECK(!g_awake);  // and keep-awake went back off
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        Client late;
        CHECK(!Connect(late));
    }

    fs::remove_all(root);
    if (g_failures == 0) std::printf("debug_server_tests: all checks passed\n");
    return g_failures ? 1 : 0;
}
