#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wct_client.h"

#include "remote_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

// Ctrl+] once detaches: the socket closes without CLOSE, so the remote ConPTY
// job stays alive and can be reattached with `wct <id>`. Ctrl+\ or Ctrl+] twice
// in quick succession ends the session: wct sends WCSP CLOSE, which terminates
// the remote job.
constexpr uint8_t kDetachKey = 0x1D; // Ctrl+]
constexpr uint8_t kCloseKey = 0x1C;  // Ctrl+backslash
constexpr uint64_t kDoublePressMs = 600;

enum class ExitRequest {
    None = 0,
    Close,   // Ctrl+\ or Ctrl+] twice: terminate the remote session
    Detach,  // Ctrl+]: keep the remote session
};

std::mutex g_write_mutex;
HANDLE g_in = INVALID_HANDLE_VALUE;
HANDLE g_out = INVALID_HANDLE_VALUE;
DWORD g_orig_in_mode = 0;
DWORD g_orig_out_mode = 0;
std::atomic<bool> g_stop{false};
std::atomic<ExitRequest> g_exit_request{ExitRequest::None};
// Set when another client resized the shared PTY; wct reasserts its console
// size on the next input (last-active-client-wins).
std::atomic<bool> g_peer_resized{false};
// Non-zero while the first Ctrl+] is waiting for a possible second press.
std::atomic<uint64_t> g_detach_deadline{0};

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(),
                                   nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), need);
    return out;
}

uint64_t NowTickMs() {
    return (uint64_t)GetTickCount64();
}

bool IsValidHandle(HANDLE h) {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

void WriteBytes(const void* data, size_t len) {
    if (!data || len == 0 || !IsValidHandle(g_out)) return;
    const char* p = static_cast<const char*>(data);
    std::lock_guard<std::mutex> lock(g_write_mutex);
    while (len > 0) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len, 64 * 1024));
        DWORD written = 0;
        if (!WriteFile(g_out, p, chunk, &written, nullptr) || written == 0) return;
        p += written;
        len -= written;
    }
}

void WriteText(const std::string& text) {
    WriteBytes(text.data(), text.size());
}

std::string EnvToken() {
    DWORD len = GetEnvironmentVariableA("WCS_TOKEN", nullptr, 0);
    if (len == 0) return {};
    std::string value(len, '\0');
    DWORD got = GetEnvironmentVariableA("WCS_TOKEN", value.data(), len);
    value.resize(got);
    return value;
}

// Puts the console into raw VT input/output mode and restores it on exit.
struct ConsoleGuard {
    bool active = false;
    HANDLE opened_in = INVALID_HANDLE_VALUE;
    HANDLE opened_out = INVALID_HANDLE_VALUE;

    bool Setup(std::string& error) {
        g_in = GetStdHandle(STD_INPUT_HANDLE);
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);
        // A process started inside a ConPTY may inherit pipe handles from its
        // parent instead of the console handles. Open CONIN$/CONOUT$ in that
        // case so wct still talks to its attached console.
        if (!IsValidHandle(g_in) || !GetConsoleMode(g_in, &g_orig_in_mode)) {
            opened_in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
            if (IsValidHandle(opened_in)) g_in = opened_in;
        }
        if (!IsValidHandle(g_out) || !GetConsoleMode(g_out, &g_orig_out_mode)) {
            opened_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
            if (IsValidHandle(opened_out)) g_out = opened_out;
        }
        if (!IsValidHandle(g_in) || !IsValidHandle(g_out)) {
            CloseOpenedHandles();
            error = "wct must run inside a console (stdin/stdout are not available)";
            return false;
        }
        // GetConsoleMode is the reliable console check: ConPTY handles may not
        // report FILE_TYPE_CHAR even though they are real console handles.
        if (!GetConsoleMode(g_in, &g_orig_in_mode) ||
            !GetConsoleMode(g_out, &g_orig_out_mode)) {
            CloseOpenedHandles();
            error = "wct must run inside a console (stdin/stdout are redirected)";
            return false;
        }

        DWORD outMode = g_orig_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (!SetConsoleMode(g_out, outMode)) {
            CloseOpenedHandles();
            error = "this console does not support VT output";
            return false;
        }

        DWORD inMode = g_orig_in_mode;
        inMode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT |
                    ENABLE_MOUSE_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_INSERT_MODE);
        inMode |= ENABLE_EXTENDED_FLAGS | ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(g_in, inMode)) {
            SetConsoleMode(g_out, g_orig_out_mode);
            CloseOpenedHandles();
            error = "this console does not support VT input "
                    "(Windows 10 1809 or later is required)";
            return false;
        }

        active = true;
        return true;
    }

    void CloseOpenedHandles() {
        if (opened_in != INVALID_HANDLE_VALUE) {
            HANDLE h = opened_in;
            opened_in = INVALID_HANDLE_VALUE;
            if (g_in == h) g_in = INVALID_HANDLE_VALUE;
            CloseHandle(h);
        }
        if (opened_out != INVALID_HANDLE_VALUE) {
            HANDLE h = opened_out;
            opened_out = INVALID_HANDLE_VALUE;
            if (g_out == h) g_out = INVALID_HANDLE_VALUE;
            CloseHandle(h);
        }
    }

    void Restore() {
        if (!active) return;
        WriteText("\x1b[0m\x1b[?25h\x1b[?1049l");
        SetConsoleMode(g_in, g_orig_in_mode);
        SetConsoleMode(g_out, g_orig_out_mode);
        active = false;
        CloseOpenedHandles();
    }
};

BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
        // Raw VT input forwards Ctrl+C to the remote shell; do not terminate.
        return TRUE;
    case CTRL_BREAK_EVENT:
        g_stop = true;
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop = true;
        WriteText("\x1b[0m\x1b[?25h\x1b[?1049l");
        if (g_in != INVALID_HANDLE_VALUE) SetConsoleMode(g_in, g_orig_in_mode);
        if (g_out != INVALID_HANDLE_VALUE) SetConsoleMode(g_out, g_orig_out_mode);
        return TRUE;
    default:
        return FALSE;
    }
}

bool GetConsoleSize(int& cols, int& rows) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!IsValidHandle(g_out) || !GetConsoleScreenBufferInfo(g_out, &info)) return false;
    cols = info.srWindow.Right - info.srWindow.Left + 1;
    rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    return true;
}

// Wakes a ReadFile() blocked in the input thread so it can observe g_stop.
void WakeInputThread() {
    if (!IsValidHandle(g_in)) return;
    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wRepeatCount = 1;
    record.Event.KeyEvent.wVirtualKeyCode = VK_SPACE;
    record.Event.KeyEvent.uChar.UnicodeChar = L' ';
    DWORD written = 0;
    WriteConsoleInputW(g_in, &record, 1, &written);
}

std::unique_ptr<RemoteSession> MakeSession(const WctOptions& options) {
    RemoteConfig config;
    config.host = options.host;
    config.port = static_cast<uint16_t>(std::clamp(options.port, 1, 65535));
    config.token = options.token.empty() ? EnvToken() : options.token;
    config.features = wcs::kDefaultFeatures;
    config.connect_timeout_ms = 5000;
    return std::make_unique<RemoteSession>(std::move(config));
}

} // namespace

int RunWct(const WctOptions& options) {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    ConsoleGuard console;
    std::string error;
    if (!console.Setup(error)) {
        std::cerr << "wct: " << error << std::endl;
        return 1;
    }
    struct RestoreOnExit {
        ConsoleGuard& guard;
        ~RestoreOnExit() { guard.Restore(); }
    } restore_on_exit{console};

    int cols = 80;
    int rows = 24;
    GetConsoleSize(cols, rows);

    g_stop = false;
    g_exit_request = ExitRequest::None;
    g_detach_deadline = 0;
    g_peer_resized = false;
    std::mutex message_mutex;
    std::string connection_error;
    bool exited = false;
    int exit_code = 0;

    std::unique_ptr<RemoteSession> session = MakeSession(options);
    RemoteSession* remote = session.get();

    remote->SetOutputSink([](const char* data, size_t len) {
        WriteBytes(data, len);
    });
    remote->onExit = [&](const wcs::ExitInfo& info) {
        std::lock_guard<std::mutex> lock(message_mutex);
        exited = true;
        exit_code = static_cast<int>(info.exit_code);
        g_stop = true;
    };
    remote->onError = [&](const wcs::ErrorInfo& info) {
        std::lock_guard<std::mutex> lock(message_mutex);
        connection_error = info.message;
        g_stop = true;
    };
    remote->onResized = [](const wcs::ResizedInfo& info) {
        if (info.reason == wcs::ResizeReason::Peer) g_peer_resized = true;
    };

    TerminalStartOptions start;
    start.shell = Utf8ToWide(options.target);
    start.cols = cols;
    start.rows = rows;
    start.codePage = CP_UTF8;

    std::wstring start_error;
    if (!remote->Start(start, &start_error)) {
        console.Restore();
        WriteText("wct: cannot connect to " + options.host + ":" +
                  std::to_string(options.port) + ": " +
                  WideToUtf8(start_error) + "\r\n");
        return 1;
    }

    WriteText("\x1b[0m[wct] " + options.host + ":" + std::to_string(options.port) +
              " (" + options.target + ") - Ctrl+] detaches, Ctrl+\\ or Ctrl+]x2 closes\r\n");

    std::thread input([&] {
        std::vector<char> buffer(4096);
        while (!g_stop) {
            DWORD read = 0;
            if (!ReadFile(g_in, buffer.data(), static_cast<DWORD>(buffer.size()),
                          &read, nullptr)) {
                break;
            }
            if (read == 0) break;
            if (g_stop) break;

            if (g_peer_resized.exchange(false)) {
                int cols = 0;
                int rows = 0;
                if (GetConsoleSize(cols, rows)) remote->Resize(cols, rows);
            }

            const char* begin = buffer.data();
            size_t send_start = 0;
            bool close_requested = false;
            for (size_t i = 0; i < read; ++i) {
                const uint8_t byte = static_cast<uint8_t>(begin[i]);
                if (byte == kCloseKey) {
                    if (i > send_start) remote->Write(begin + send_start, i - send_start);
                    send_start = i + 1;
                    g_exit_request = ExitRequest::Close;
                    close_requested = true;
                    break;
                }
                if (byte != kDetachKey) continue;

                if (i > send_start) remote->Write(begin + send_start, i - send_start);
                send_start = i + 1;

                const uint64_t now = NowTickMs();
                const uint64_t deadline = g_detach_deadline.load();
                if (deadline != 0 && now <= deadline) {
                    g_exit_request = ExitRequest::Close;
                    close_requested = true;
                    break;
                }
                // First Ctrl+]: wait briefly for a second press before detaching.
                g_detach_deadline = now + kDoublePressMs;
            }
            if (!close_requested && read > send_start) {
                remote->Write(begin + send_start, read - send_start);
            }
            if (close_requested) {
                g_stop = true;
                break;
            }
        }
    });

    int last_cols = cols;
    int last_rows = rows;
    uint64_t last_size_check = NowTickMs();

    while (!g_stop) {
        const uint64_t now = NowTickMs();
        if (now - last_size_check >= 100) {
            last_size_check = now;
            int new_cols = 0;
            int new_rows = 0;
            if (GetConsoleSize(new_cols, new_rows) &&
                (new_cols != last_cols || new_rows != last_rows)) {
                last_cols = new_cols;
                last_rows = new_rows;
                remote->Resize(new_cols, new_rows);
            }
        }
        const uint64_t detach_deadline = g_detach_deadline.load();
        if (detach_deadline != 0 && NowTickMs() >= detach_deadline) {
            g_exit_request = ExitRequest::Detach;
            g_stop = true;
        }
        if (!remote->IsRunning()) g_stop = true;
        Sleep(20);
    }

    WakeInputThread();
    if (input.joinable()) input.join();
    if (IsValidHandle(g_in)) FlushConsoleInputBuffer(g_in);

    const ExitRequest request = g_exit_request.load();
    if (request == ExitRequest::Detach) {
        // Plain disconnect: the server keeps the ConPTY job for a later attach.
        remote->Close();
    } else {
        // Ctrl+] twice (or an abrupt wct exit) ends the session.
        // TerminateSession closes directly when the socket is already gone.
        remote->TerminateSession();
    }
    session.reset();

    std::string report;
    {
        std::lock_guard<std::mutex> lock(message_mutex);
        if (!connection_error.empty()) {
            report = "connection error: " + connection_error;
        } else if (exited) {
            report = "session exited (code " + std::to_string(exit_code) + ")";
        } else if (request == ExitRequest::Detach) {
            report = "detached (session kept running; reattach with wct <id>)";
        } else {
            report = "session closed";
        }
    }

    console.Restore();
    WriteText("\r\n[wct] " + report + "\r\n");
    return 0;
}

int ListWctSessions(const WctOptions& options) {
    auto session = MakeSession(options);

    std::mutex mutex;
    std::vector<wcs::SessionInfo> sessions;
    bool have_list = false;
    bool failed = false;
    std::string failure;

    session->onSessionList = [&](const std::vector<wcs::SessionInfo>& list) {
        std::lock_guard<std::mutex> lock(mutex);
        sessions = list;
        have_list = true;
    };
    session->onError = [&](const wcs::ErrorInfo& info) {
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
        failure = info.message;
    };

    TerminalStartOptions start;
    start.shell = L"list";
    start.cols = 80;
    start.rows = 24;

    std::wstring start_error;
    if (!session->Start(start, &start_error)) {
        std::cerr << "wct: cannot connect to " << options.host << ":"
                  << options.port << ": " << WideToUtf8(start_error) << "\n";
        return 1;
    }

    session->RequestSessionList();

    const auto deadline = NowTickMs() + 5000;
    while (NowTickMs() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (have_list || failed) break;
        }
        Sleep(20);
    }

    session->Close();

    std::lock_guard<std::mutex> lock(mutex);
    if (failed) {
        std::cerr << "wct: " << failure << "\n";
        return 1;
    }
    if (!have_list) {
        std::cerr << "wct: no SESSION_LIST received\n";
        return 1;
    }

    static const char* kStateNames[] = {
        "running", "completed", "failed", "timed out", "idle-timed out", "killed"};
    if (sessions.empty()) {
        std::cout << "No sessions.\n";
        return 0;
    }
    for (const auto& s : sessions) {
        const char* state = s.state < 6 ? kStateNames[s.state] : "unknown";
        std::cout << s.id << "\t" << state << "\t" << s.name;
        if (!s.command.empty()) std::cout << "\t" << s.command;
        std::cout << "\n";
    }
    return 0;
}
