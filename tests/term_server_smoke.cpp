// Functional smoke test for the WinConsoleServer WCSP terminal server.
//
// Usage:
//   term_server_smoke --server <WinConsoleServer.exe>
//
// The test starts an isolated server on a loopback port, spawns a cmd session,
// attaches a second client, detaches, reattaches, verifies retained sessions,
// and checks WCSP 1.2 FeatureSizeSync peer resize notifications.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "remote_session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_passes = 0;

void Check(bool ok, const std::string& msg) {
    if (ok) {
        ++g_passes;
        std::printf("PASS: %s\n", msg.c_str());
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    }
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), need);
    return out;
}

int FindFreePort() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        int len = sizeof(addr);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }
    closesocket(s);
    WSACleanup();
    return port;
}

struct ServerProcess {
    PROCESS_INFORMATION process{};

    bool Start(const std::wstring& exe, int port, const std::string& token) {
        std::wstring cmd = L"\"" + exe + L"\" --listen 127.0.0.1:" +
                           std::to_wstring(port) + L" --token " +
                           Utf8ToWide(token) + L" --shell cmd";
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &process)) {
            std::fprintf(stderr, "FAIL: CreateProcessW(server): %lu\n", GetLastError());
            return false;
        }
        return true;
    }

    void Stop() {
        if (process.hProcess) {
            TerminateProcess(process.hProcess, 0);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            process = {};
        }
    }

    ~ServerProcess() { Stop(); }
};

struct Capture {
    std::mutex mutex;
    std::string text;

    void Sink(const char* data, size_t len) {
        std::lock_guard<std::mutex> lock(mutex);
        text.append(data, len);
    }

    bool WaitFor(const std::string& needle, int timeoutMs) {
        const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
        while (GetTickCount() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (text.find(needle) != std::string::npos) return true;
            }
            Sleep(50);
        }
        std::lock_guard<std::mutex> lock(mutex);
        return text.find(needle) != std::string::npos;
    }
};

bool WaitForServer(const RemoteConfig& cfg, int timeoutMs) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
    while (GetTickCount() < deadline) {
        RemoteSession probe(cfg);
        TerminalStartOptions opt;
        opt.shell = L"list";
        opt.cols = 80;
        opt.rows = 25;
        std::wstring error;
        if (probe.Start(opt, &error)) {
            probe.Close();
            return true;
        }
        Sleep(100);
    }
    return false;
}

bool QuerySessions(const RemoteConfig& cfg, std::vector<wcs::SessionInfo>& sessions) {
    RemoteSession control(cfg);
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    control.onSessionList = [&](const std::vector<wcs::SessionInfo>& list) {
        std::lock_guard<std::mutex> lock(mutex);
        sessions = list;
        done = true;
        cv.notify_all();
    };

    TerminalStartOptions opt;
    opt.shell = L"list";
    opt.cols = 80;
    opt.rows = 25;
    std::wstring error;
    if (!control.Start(opt, &error)) return false;

    control.RequestSessionList();
    {
        std::unique_lock<std::mutex> lock(mutex);
        done = cv.wait_for(lock, std::chrono::seconds(3), [&] { return done; });
    }
    control.Close();
    return done;
}

bool FindSession(const std::vector<wcs::SessionInfo>& sessions, uint32_t id,
                 wcs::SessionInfo& out) {
    for (const auto& session : sessions) {
        if (session.id == id) {
            out = session;
            return true;
        }
    }
    return false;
}

bool FindRunningSession(const RemoteConfig& cfg, uint32_t id, wcs::SessionInfo& out) {
    std::vector<wcs::SessionInfo> sessions;
    if (!QuerySessions(cfg, sessions)) return false;
    if (!FindSession(sessions, id, out)) return false;
    return out.state == 0;
}

int WaitForNewestSession(const RemoteConfig& cfg, const std::string& name,
                         int timeoutMs) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
    while (GetTickCount() < deadline) {
        std::vector<wcs::SessionInfo> sessions;
        if (QuerySessions(cfg, sessions)) {
            int newest = 0;
            for (const auto& session : sessions) {
                if (session.state != 0) continue;
                if (session.name != name) continue;
                if (static_cast<int>(session.id) > newest) {
                    newest = static_cast<int>(session.id);
                }
            }
            if (newest > 0) return newest;
        }
        Sleep(100);
    }
    return 0;
}

TerminalStartOptions AttachOptions(uint32_t id) {
    TerminalStartOptions opt;
    opt.shell = L"attach:" + std::to_wstring(id);
    opt.cols = 80;
    opt.rows = 25;
    return opt;
}

struct ResizeSink {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<wcs::ResizedInfo> events;

    void Add(const wcs::ResizedInfo& info) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(info);
        cv.notify_all();
    }

    bool WaitFor(wcs::ResizeReason reason, uint16_t cols, uint16_t rows,
                 int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            for (const auto& event : events) {
                if (event.reason == reason && event.cols == cols && event.rows == rows)
                    return true;
            }
            return false;
        });
    }

    bool HasReason(wcs::ResizeReason reason) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& event : events) {
            if (event.reason == reason) return true;
        }
        return false;
    }
};

} // namespace

int main(int argc, char** argv) {
    std::wstring serverPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            serverPath = Utf8ToWide(argv[++i]);
        }
    }
    if (serverPath.empty()) {
        std::fprintf(stderr, "usage: term_server_smoke --server <WinConsoleServer.exe>\n");
        return 2;
    }

    const int port = FindFreePort();
    if (port <= 0) {
        std::fprintf(stderr, "FAIL: cannot find a free loopback port\n");
        return 2;
    }
    const std::string token = "wcsp-smoke-token";

    ServerProcess server;
    if (!server.Start(serverPath, port, token)) return 1;

    RemoteConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<uint16_t>(port);
    cfg.token = token;
    cfg.features = wcs::kDefaultFeatures;

    const bool serverReady = WaitForServer(cfg, 10000);
    Check(serverReady, "server accepts WCSP connections");
    if (!serverReady) {
        server.Stop();
        return 1;
    }

    std::wstring error;
    Capture out1;
    RemoteSession session1(cfg);
    session1.SetOutputSink([&](const char* data, size_t len) { out1.Sink(data, len); });

    TerminalStartOptions spawnOpt;
    spawnOpt.shell = L"cmd";
    spawnOpt.cols = 80;
    spawnOpt.rows = 25;
    Check(session1.Start(spawnOpt, &error), "spawned cmd session starts");

    const int sessionId = WaitForNewestSession(cfg, "wcsp-cmd", 5000);
    Check(sessionId > 0, "spawned session is listed");
    if (sessionId <= 0) {
        session1.Close();
        server.Stop();
        return 1;
    }

    Sleep(500);
    const char greeting[] = "echo hello-wcsp\r";
    session1.Write(greeting, sizeof(greeting) - 1);
    Check(out1.WaitFor("hello-wcsp", 10000), "spawned session echoes input");

    Capture out2;
    RemoteSession session2(cfg);
    session2.SetOutputSink([&](const char* data, size_t len) { out2.Sink(data, len); });
    const TerminalStartOptions attachOpt = AttachOptions(static_cast<uint32_t>(sessionId));
    Check(session2.Start(attachOpt, &error), "a second client attaches to the same job");

    const char second[] = "echo second-session\r";
    session2.Write(second, sizeof(second) - 1);
    Check(out2.WaitFor("second-session", 10000), "second client sees its own echo");
    Check(out1.WaitFor("second-session", 10000), "first client sees the second client's input");

    session2.Close();
    Sleep(300);
    const char afterSecond[] = "echo after-second\r";
    session1.Write(afterSecond, sizeof(afterSecond) - 1);
    Check(out1.WaitFor("after-second", 10000),
          "first client keeps working after the second closes");

    session1.Close();
    Sleep(700);
    wcs::SessionInfo retained;
    Check(FindRunningSession(cfg, static_cast<uint32_t>(sessionId), retained),
          "disconnecting an attach session does not kill the job");

    Capture out3;
    RemoteSession session3(cfg);
    session3.SetOutputSink([&](const char* data, size_t len) { out3.Sink(data, len); });
    Check(session3.Start(attachOpt, &error), "retained session can be reattached");
    Check(out3.WaitFor("after-second", 10000), "reattach replays prior output");
    session3.TerminateSession();
    Sleep(700);
    Check(!FindRunningSession(cfg, static_cast<uint32_t>(sessionId), retained),
          "CLOSE terminates the retained session");

    // Create another retained job for the FeatureSizeSync checks.
    Capture sizeHostOut;
    RemoteSession sizeHost(cfg);
    sizeHost.SetOutputSink(
        [&](const char* data, size_t len) { sizeHostOut.Sink(data, len); });
    Check(sizeHost.Start(spawnOpt, &error), "size-sync host session starts");
    const int sizeJobId = WaitForNewestSession(cfg, "wcsp-cmd", 5000);
    Check(sizeJobId > 0, "size-sync host session is listed");
    if (sizeJobId > 0) {
        sizeHost.Close();
        Sleep(500);

        const TerminalStartOptions sizeOpt =
            AttachOptions(static_cast<uint32_t>(sizeJobId));

        ResizeSink sinkA;
        ResizeSink sinkB;
        ResizeSink sinkLegacy;

        RemoteConfig syncCfg = cfg;
        syncCfg.features = wcs::kDefaultFeatures;

        RemoteConfig legacyCfg = cfg;
        legacyCfg.features = wcs::kDefaultFeatures & ~wcs::FeatureSizeSync;

        RemoteSession syncA(syncCfg);
        RemoteSession syncB(syncCfg);
        RemoteSession legacy(legacyCfg);
        syncA.SetResizedHandler([&](const wcs::ResizedInfo& info) { sinkA.Add(info); });
        syncB.SetResizedHandler([&](const wcs::ResizedInfo& info) { sinkB.Add(info); });
        legacy.SetResizedHandler(
            [&](const wcs::ResizedInfo& info) { sinkLegacy.Add(info); });

        Check(syncA.Start(sizeOpt, &error), "SizeSync client A attaches");
        Check(syncB.Start(sizeOpt, &error), "SizeSync client B attaches");
        Check(legacy.Start(sizeOpt, &error), "legacy client attaches without SizeSync");

        Sleep(300);
        syncB.Resize(100, 30);
        Check(sinkA.WaitFor(wcs::ResizeReason::Peer, 100, 30, 3000),
              "SizeSync peer A receives RESIZED(Peer) from B's resize");

        syncA.Resize(90, 20);
        Check(sinkB.WaitFor(wcs::ResizeReason::Peer, 90, 20, 3000),
              "SizeSync peer B receives RESIZED(Peer) from A's resize");

        Sleep(300);
        Check(!sinkLegacy.HasReason(wcs::ResizeReason::Peer),
              "a client without FeatureSizeSync receives no Peer RESIZED");

        syncA.Close();
        syncB.Close();
        legacy.Close();

        RemoteSession cleanup(cfg);
        if (cleanup.Start(sizeOpt, &error)) {
            cleanup.TerminateSession();
            Sleep(500);
        }
    }

    server.Stop();

    std::printf("\n%d passed, %d failed\n", g_passes, g_failures);
    return g_failures == 0 ? 0 : 1;
}
