#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include "terminal/include/TerminalView.h"
#include "remote_session.h"
#include "resource.h"
#include "../common/version.h"

#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <memory>
#include <mutex>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

constexpr wchar_t kMainClass[] = L"WinConsoleTerminalMain";

constexpr UINT WM_APP_AUTOCONNECT = WM_APP + 1;
constexpr UINT WM_APP_ERROR       = WM_APP + 2;
constexpr UINT WM_APP_EXIT        = WM_APP + 3;
constexpr UINT WM_APP_RESIZED     = WM_APP + 4;
constexpr UINT WM_APP_AUTO_RECONNECT = WM_APP + 5;
constexpr UINT WM_APP_AUTO_RECONNECT_FAILED = WM_APP + 6;

constexpr UINT_PTR kConnectionTimerId = 1;

enum class ConnectionState {
    Idle,
    Connecting,
    Connected,
    Reconnecting,
    Failed,
    Exited,
};

struct AppState {
    HWND hwnd = nullptr;
    HWND status = nullptr;
    HWND view_hwnd = nullptr;
    TerminalView* view = nullptr;
    RemoteSession* session = nullptr;

    std::wstring host;
    int port = 7000;
    std::wstring shell = L"cmd";
    std::wstring token;
    bool auto_connect = false;

    // Same-session reconnection (WCSP JOB_ID + attach:<id>). The server keeps
    // a detached ConPTY job alive, so a dropped connection can resume it.
    uint32_t server_job_id = 0;
    bool auto_reconnect = true;
    bool closing = false;
    bool reconnect_in_flight = false;
    int reconnect_attempts = 0;
    uint64_t next_reconnect_tick_ms = 0;
    std::wstring last_error;
    std::wstring active_target;
    ConnectionState state = ConnectionState::Idle;
};

AppState g;

std::string WideToUtf8Client(const std::wstring& text) {
    if (text.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWideClient(const std::string& text) {
    if (text.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(),
                                   nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), need);
    return out;
}

std::wstring SettingsPath() {
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        return L"WinConsoleTerminal.ini";
    }
    std::wstring path(appdata);
    path += L"\\WinConsoleTerminal.ini";
    return path;
}

void LoadSettings() {
    const std::wstring ini = SettingsPath();
    wchar_t buf[512] = {};

    GetPrivateProfileStringW(L"connection", L"host", L"127.0.0.1", buf, 512, ini.c_str());
    g.host = buf;
    if (g.host.empty()) g.host = L"127.0.0.1";
    g.port = (int)GetPrivateProfileIntW(L"connection", L"port", 7000, ini.c_str());
    GetPrivateProfileStringW(L"connection", L"shell", L"cmd", buf, 512, ini.c_str());
    g.shell = buf;
    GetPrivateProfileStringW(L"connection", L"token", L"", buf, 512, ini.c_str());
    g.token = buf;
    g.auto_reconnect =
        GetPrivateProfileIntW(L"connection", L"auto_reconnect", 1, ini.c_str()) != 0;
}

void SaveSettings() {
    const std::wstring ini = SettingsPath();
    WritePrivateProfileStringW(L"connection", L"host", g.host.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"connection", L"port",
                               std::to_wstring(g.port).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"connection", L"shell", g.shell.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"connection", L"token", g.token.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"connection", L"auto_reconnect",
                               g.auto_reconnect ? L"1" : L"0", ini.c_str());
}

void SetStatus(const std::wstring& text, int part = 0) {
    if (!g.status) return;
    SendMessageW(g.status, SB_SETTEXTW, (WPARAM)part, (LPARAM)text.c_str());
}

void Layout() {
    if (!g.hwnd || !g.view) return;

    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    int statusHeight = 0;
    if (g.status) {
        SendMessageW(g.status, WM_SIZE, 0, 0);
        RECT sr{};
        GetWindowRect(g.status, &sr);
        statusHeight = sr.bottom - sr.top;
    }
    const int viewHeight = (rc.bottom - rc.top) - statusHeight;
    if (g.view_hwnd) {
        MoveWindow(g.view_hwnd, 0, 0, rc.right - rc.left,
                   viewHeight > 0 ? viewHeight : 0, TRUE);
    }

    int parts[4] = { (rc.right - rc.left) / 3,
                     (rc.right - rc.left) * 2 / 3,
                     (rc.right - rc.left) - 180,
                     rc.right - rc.left };
    if (g.status) {
        SendMessageW(g.status, SB_SETPARTS, 4, (LPARAM)parts);
    }
}

void UpdateStatus() {
    if (!g.status) return;
    std::wstring state_text;
    switch (g.state) {
    case ConnectionState::Idle:       state_text = L"disconnected"; break;
    case ConnectionState::Connecting: state_text = L"connecting..."; break;
    case ConnectionState::Connected:  state_text = L"connected"; break;
    case ConnectionState::Reconnecting:
        state_text = L"reconnecting...";
        break;
    case ConnectionState::Failed:
        state_text = g.last_error.empty() ? L"connection lost"
                                          : L"connection lost - " + g.last_error;
        break;
    case ConnectionState::Exited:
        state_text = L"process exited";
        break;
    }
    SetStatus(state_text, 0);
    SetStatus(g.host + L":" + std::to_wstring(g.port), 1);

    std::wstring target = g.active_target.empty() ? g.shell : g.active_target;
    if (g.server_job_id > 0) {
        target += L"  #" + std::to_wstring(g.server_job_id);
    }
    SetStatus(target, 2);

    if (g.view) {
        SetStatus(std::to_wstring(g.view->Columns()) + L" x " +
                  std::to_wstring(g.view->Rows()), 3);
    }
}

void CloseCurrentSession() {
    if (g.session) {
        g.session->Close();
        g.session = nullptr;
    }
}

void CancelScheduledReconnect() {
    g.reconnect_in_flight = false;
    g.next_reconnect_tick_ms = 0;
}

uint64_t ReconnectBackoffMs(int attempts) {
    uint64_t delay = 1000;
    for (int i = 0; i < attempts && delay < 30000; ++i) {
        delay *= 2;
        if (delay > 30000) delay = 30000;
    }
    return delay;
}

void ScheduleAutoReconnect(const std::wstring& detail, int attempts) {
    g.last_error = detail;
    g.reconnect_in_flight = false;
    g.reconnect_attempts = attempts;
    if (g.auto_reconnect && g.server_job_id > 0 && !g.closing) {
        g.state = ConnectionState::Reconnecting;
        g.next_reconnect_tick_ms = GetTickCount64() + ReconnectBackoffMs(attempts);
    } else {
        g.state = ConnectionState::Failed;
        g.next_reconnect_tick_ms = 0;
    }
    UpdateStatus();
}

void Disconnect() {
    CancelScheduledReconnect();
    CloseCurrentSession();
    g.state = ConnectionState::Idle;
    UpdateStatus();
}

bool OpenTarget(const std::wstring& target, bool show_error,
                std::wstring* out_error) {
    RemoteConfig config;
    config.host = WideToUtf8Client(g.host);
    config.port = (uint16_t)g.port;
    config.shell = WideToUtf8Client(target);
    config.token = WideToUtf8Client(g.token);

    auto* session = new RemoteSession(config);
    session->onExit = [hwnd = g.hwnd](const wcs::ExitInfo& info) {
        PostMessageW(hwnd, WM_APP_EXIT, (WPARAM)info.exit_code, 0);
    };
    session->onError = [hwnd = g.hwnd](const wcs::ErrorInfo&) {
        PostMessageW(hwnd, WM_APP_ERROR, 0, 0);
    };
    session->onResized = [hwnd = g.hwnd](const wcs::ResizedInfo& info) {
        PostMessageW(hwnd, WM_APP_RESIZED,
                     MAKELONG(info.cols, info.rows), 0);
    };

    g.session = session;
    g.view->SetIo(std::unique_ptr<ITerminalIo>(session));

    TerminalStartOptions opt;
    opt.shell = target;
    opt.cols = 0;
    opt.rows = 0;
    opt.codePage = CP_UTF8;

    std::wstring error;
    if (!g.view->StartSession(opt, &error)) {
        g.session = nullptr;
        if (error.empty()) error = L"Connection failed.";
        if (out_error) *out_error = error;
        if (show_error) {
            MessageBoxW(g.hwnd, error.c_str(), L"Connect", MB_ICONERROR);
        }
        return false;
    }

    g.active_target = target;
    g.server_job_id = static_cast<uint32_t>(session->JobId());
    if (g.server_job_id == 0 && target.rfind(L"attach:", 0) == 0) {
        g.server_job_id =
            static_cast<uint32_t>(std::wcstoul(target.c_str() + 7, nullptr, 10));
    }
    g.reconnect_attempts = 0;
    g.reconnect_in_flight = false;
    g.next_reconnect_tick_ms = 0;
    g.last_error.clear();
    return true;
}

bool StartConnection(const std::wstring& host, int port,
                     const std::wstring& shell, const std::wstring& token) {
    if (host.empty()) {
        MessageBoxW(g.hwnd, L"Host is empty.", L"Connect", MB_ICONWARNING);
        return false;
    }

    Disconnect();
    g.server_job_id = 0;
    g.active_target.clear();
    g.reconnect_attempts = 0;
    g.last_error.clear();
    g.state = ConnectionState::Connecting;
    UpdateStatus();

    std::wstring error;
    if (!OpenTarget(shell, true, &error)) {
        g.state = ConnectionState::Failed;
        g.last_error = error;
        UpdateStatus();
        return false;
    }

    g.state = ConnectionState::Connected;
    UpdateStatus();
    return true;
}

bool TryReconnect(bool auto_mode) {
    if (!g.hwnd) return false;

    const uint32_t job_id = g.server_job_id;
    std::wstring fallback_target = g.active_target;
    if (fallback_target.empty() || fallback_target.rfind(L"attach:", 0) == 0) {
        fallback_target = g.shell;
    }
    std::wstring attach_target;
    if (job_id > 0) {
        attach_target = L"attach:" + std::to_wstring(job_id);
    }

    CancelScheduledReconnect();
    CloseCurrentSession();
    g.view->ResetForReconnect();
    g.state = ConnectionState::Connecting;
    UpdateStatus();

    std::wstring error;
    bool ok = false;
    if (!attach_target.empty()) {
        ok = OpenTarget(attach_target, false, &error);
    }
    if (!ok && !auto_mode && !fallback_target.empty() &&
        fallback_target != attach_target) {
        error.clear();
        ok = OpenTarget(fallback_target, false, &error);
    }

    if (!ok) {
        if (error.empty()) error = L"Reconnect failed.";
        ScheduleAutoReconnect(error, g.reconnect_attempts + 1);
        if (!auto_mode) {
            MessageBoxW(g.hwnd, error.c_str(), L"Reconnect", MB_ICONERROR);
        }
        return false;
    }

    g.state = ConnectionState::Connected;
    g.reconnect_attempts = 0;
    g.reconnect_in_flight = false;
    g.next_reconnect_tick_ms = 0;
    g.last_error.clear();
    UpdateStatus();
    return true;
}

bool ProbeServerReachable(const std::wstring& host, int port, int timeout_ms) {
    if (port <= 0) return false;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string host_utf8 = WideToUtf8Client(host.empty() ? L"127.0.0.1" : host);
    const std::string port_text = std::to_string(port);
    bool reachable = false;
    if (getaddrinfo(host_utf8.c_str(), port_text.c_str(), &hints, &result) == 0) {
        for (addrinfo* ai = result; ai && !reachable; ai = ai->ai_next) {
            SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == INVALID_SOCKET) continue;

            u_long nonblocking = 1;
            ioctlsocket(s, FIONBIO, &nonblocking);
            int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
            if (rc == 0) {
                reachable = true;
            } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(s, &writefds);
                timeval tv{};
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                if (select(0, nullptr, &writefds, nullptr, &tv) > 0) {
                    int socket_error = 0;
                    int length = sizeof(socket_error);
                    if (getsockopt(s, SOL_SOCKET, SO_ERROR,
                                   reinterpret_cast<char*>(&socket_error),
                                   &length) == 0 &&
                        socket_error == 0) {
                        reachable = true;
                    }
                }
            }
            closesocket(s);
        }
        freeaddrinfo(result);
    }
    WSACleanup();
    return reachable;
}

void StartAutoReconnectProbe(HWND hwnd, std::wstring host, int port) {
    std::thread([hwnd, host = std::move(host), port]() {
        const bool reachable = ProbeServerReachable(host, port, 800);
        PostMessageW(hwnd, reachable ? WM_APP_AUTO_RECONNECT
                                     : WM_APP_AUTO_RECONNECT_FAILED,
                     0, 0);
    }).detach();
}

void RunAutoReconnect(HWND hwnd) {
    if (g.state != ConnectionState::Reconnecting) return;
    if (g.reconnect_in_flight) return;
    if (g.server_job_id == 0) return;
    const uint64_t now = GetTickCount64();
    if (now < g.next_reconnect_tick_ms) return;
    g.reconnect_in_flight = true;
    StartAutoReconnectProbe(hwnd, g.host, g.port);
}

INT_PTR CALLBACK ConnectProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemTextW(dlg, IDC_HOST, g.host.c_str());
        SetDlgItemInt(dlg, IDC_PORT, (UINT)g.port, FALSE);
        SetDlgItemTextW(dlg, IDC_TOKEN, g.token.c_str());

        HWND combo = GetDlgItem(dlg, IDC_SHELL);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"cmd");
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"powershell");
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"bash");
        int sel = 0;
        if (_wcsicmp(g.shell.c_str(), L"powershell") == 0) sel = 1;
        else if (_wcsicmp(g.shell.c_str(), L"bash") == 0) sel = 2;
        SendMessageW(combo, CB_SETCURSEL, sel, 0);

        SetFocus(GetDlgItem(dlg, IDC_HOST));
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            wchar_t host[512] = {};
            wchar_t token[512] = {};
            GetDlgItemTextW(dlg, IDC_HOST, host, 512);
            GetDlgItemTextW(dlg, IDC_TOKEN, token, 512);
            if (host[0] == L'\0') {
                MessageBoxW(dlg, L"Host is required.", L"Connect", MB_ICONWARNING);
                return TRUE;
            }
            BOOL translated = FALSE;
            UINT port = GetDlgItemInt(dlg, IDC_PORT, &translated, FALSE);
            if (!translated || port == 0 || port > 65535) {
                MessageBoxW(dlg, L"Port must be 1-65535.", L"Connect", MB_ICONWARNING);
                return TRUE;
            }

            HWND combo = GetDlgItem(dlg, IDC_SHELL);
            int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
            const wchar_t* shell = L"cmd";
            if (sel == 1) shell = L"powershell";
            else if (sel == 2) shell = L"bash";

            g.host = host;
            g.port = (int)port;
            g.shell = shell;
            g.token = token;
            SaveSettings();
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    }
    return FALSE;
}

bool ShowConnectDialog(bool start_connection = true) {
    if (DialogBoxParamW(GetModuleHandleW(nullptr),
                        MAKEINTRESOURCEW(IDD_CONNECT), g.hwnd,
                        ConnectProc, 0) != IDOK) {
        return false;
    }
    if (start_connection) {
        StartConnection(g.host, g.port, g.shell, g.token);
    }
    return true;
}

bool QuerySessions(std::vector<wcs::SessionInfo>& sessions, std::wstring* error) {
    RemoteConfig config;
    config.host = WideToUtf8Client(g.host);
    config.port = (uint16_t)g.port;
    config.token = WideToUtf8Client(g.token);
    config.features = wcs::kDefaultFeatures;

    RemoteSession session(config);
    std::mutex mutex;
    std::condition_variable cv;
    bool received = false;
    bool failed = false;
    std::wstring failure;

    session.onSessionList = [&](const std::vector<wcs::SessionInfo>& list) {
        std::lock_guard<std::mutex> lock(mutex);
        sessions = list;
        received = true;
        cv.notify_all();
    };
    session.onError = [&](const wcs::ErrorInfo& info) {
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
        failure = Utf8ToWideClient(info.message);
        cv.notify_all();
    };

    TerminalStartOptions opt;
    opt.shell = L"list";
    opt.cols = 80;
    opt.rows = 24;
    std::wstring start_error;
    if (!session.Start(opt, &start_error)) {
        if (error) {
            *error = start_error.empty() ? L"Cannot connect to the server." : start_error;
        }
        return false;
    }

    session.RequestSessionList();
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(5),
                    [&] { return received || failed; });
    }
    session.Close();

    if (failed) {
        if (error) *error = failure.empty() ? L"Session list request failed." : failure;
        return false;
    }
    if (!received) {
        if (error) *error = L"Timed out waiting for the session list.";
        return false;
    }
    return true;
}

struct SessionDialogState {
    const std::vector<wcs::SessionInfo>* sessions = nullptr;
};

const wchar_t* SessionStateName(uint8_t state) {
    switch (state) {
    case 0: return L"running";
    case 1: return L"completed";
    case 2: return L"failed";
    case 3: return L"timed out";
    case 4: return L"idle-timed out";
    case 5: return L"killed";
    default: return L"unknown";
    }
}

INT_PTR CALLBACK SessionsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        auto* state = reinterpret_cast<SessionDialogState*>(GetWindowLongPtrW(
            dlg, GWLP_USERDATA));
        if (!state) {
            state = reinterpret_cast<SessionDialogState*>(lp);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        HWND list = GetDlgItem(dlg, IDC_SESSION_LIST);
        if (state && state->sessions && list) {
            for (const auto& s : *state->sessions) {
                std::wstring label = std::to_wstring(s.id);
                label += L"  [";
                label += SessionStateName(s.state);
                label += L"]  ";
                label += Utf8ToWideClient(s.name);
                if (!s.command.empty()) {
                    label += L"  -  ";
                    label += Utf8ToWideClient(s.command);
                }
                SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)label.c_str());
            }
            if (!state->sessions->empty()) {
                SendMessageW(list, LB_SETCURSEL, 0, 0);
            }
        }
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            HWND list = GetDlgItem(dlg, IDC_SESSION_LIST);
            int sel = list ? (int)SendMessageW(list, LB_GETCURSEL, 0, 0) : -1;
            if (sel < 0) {
                MessageBoxW(dlg, L"Select a session.", L"Attach", MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(dlg, sel + 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        default:
            break;
        }
        break;
    }
    return FALSE;
}

int RunSessionsDialog(const std::vector<wcs::SessionInfo>& sessions) {
    SessionDialogState state;
    state.sessions = &sessions;
    INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr),
                                     MAKEINTRESOURCEW(IDD_SESSIONS), g.hwnd,
                                     SessionsProc,
                                     reinterpret_cast<LPARAM>(&state));
    if (result <= 0) return -1;
    return (int)result - 1;
}

void NewSession(const wchar_t* shell) {
    StartConnection(g.host, g.port, shell, g.token);
}

void AttachSession() {
    std::vector<wcs::SessionInfo> sessions;
    std::wstring error;
    if (!QuerySessions(sessions, &error)) {
        MessageBoxW(g.hwnd, error.c_str(), L"Attach to session", MB_ICONERROR);
        return;
    }
    if (sessions.empty()) {
        MessageBoxW(g.hwnd, L"There are no retained sessions.",
                    L"Attach to session", MB_ICONINFORMATION);
        return;
    }
    int index = RunSessionsDialog(sessions);
    if (index < 0 || index >= (int)sessions.size()) return;

    const auto& selected = sessions[(size_t)index];
    std::wstring target = L"attach:" + std::to_wstring(selected.id);
    StartConnection(g.host, g.port, target, g.token);
}

// Quote an argument for CreateProcessW's command line (CommandLineToArgvW
// rules). Empty arguments are not needed here.
std::wstring QuoteCommandLineArg(const std::wstring& arg) {
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(ch);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

void AttachAllSessions() {
    std::vector<wcs::SessionInfo> sessions;
    std::wstring error;
    if (!QuerySessions(sessions, &error)) {
        MessageBoxW(g.hwnd, error.c_str(), L"Attach to all sessions", MB_ICONERROR);
        return;
    }

    std::vector<const wcs::SessionInfo*> running;
    for (const auto& session : sessions) {
        if (session.state == 0) running.push_back(&session);
    }
    if (running.empty()) {
        MessageBoxW(g.hwnd, L"There are no running retained sessions.",
                    L"Attach to all sessions", MB_ICONINFORMATION);
        return;
    }

    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        MessageBoxW(g.hwnd, L"Cannot determine the executable path.",
                    L"Attach to all sessions", MB_ICONERROR);
        return;
    }

    const std::wstring exeArg = QuoteCommandLineArg(exe);
    int launched = 0;
    int failed = 0;
    for (const auto* session : running) {
        std::wstring cmd = exeArg;
        cmd += L" --host ";
        cmd += QuoteCommandLineArg(g.host);
        cmd += L" --port ";
        cmd += std::to_wstring(g.port);
        if (!g.token.empty()) {
            cmd += L" --token ";
            cmd += QuoteCommandLineArg(g.token);
        }
        cmd += L" --shell ";
        cmd += QuoteCommandLineArg(L"attach:" + std::to_wstring(session->id));
        cmd += L" --connect";

        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe, mutableCmd.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &si, &pi)) {
            ++launched;
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            ++failed;
        }
    }

    SetStatus(L"opened " + std::to_wstring(launched) + L" session window(s)", 0);
    if (failed > 0) {
        MessageBoxW(g.hwnd,
                    (L"Failed to open " + std::to_wstring(failed) +
                     L" session window(s).").c_str(),
                    L"Attach to all sessions", MB_ICONWARNING);
    }
}

void CloseSession() {
    if (!g.session) {
        SetStatus(L"no session", 0);
        return;
    }
    g.session->TerminateSession();
    g.session = nullptr;
    g.server_job_id = 0;
    CancelScheduledReconnect();
    g.state = ConnectionState::Idle;
    UpdateStatus();
    SetStatus(L"session closed", 0);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = hwnd;

        g.view = new TerminalView();
        if (!g.view->Create(hwnd)) {
            delete g.view;
            g.view = nullptr;
            return -1;
        }
        g.view_hwnd = g.view->Hwnd();

        g.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, hwnd, (HMENU)1000,
                                   GetModuleHandleW(nullptr), nullptr);
        SetStatus(L"ready", 0);
        Layout();
        SetTimer(hwnd, kConnectionTimerId, 500, nullptr);

        PostMessageW(hwnd, WM_APP_AUTOCONNECT, 0, 0);
        return 0;
    }

    case WM_SIZE:
        Layout();
        return 0;

    case WM_DPICHANGED: {
        RECT* suggested = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SETFOCUS:
        if (g.view_hwnd) SetFocus(g.view_hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_NEW_POWERSHELL:
            NewSession(L"powershell");
            return 0;
        case IDM_NEW_CMD:
            NewSession(L"cmd");
            return 0;
        case IDM_NEW_BASH:
            NewSession(L"bash");
            return 0;
        case IDM_ATTACH:
            AttachSession();
            return 0;
        case IDM_ATTACH_ALL:
            AttachAllSessions();
            return 0;
        case IDM_CONNECT:
            ShowConnectDialog();
            return 0;
        case IDM_RECONNECT:
            TryReconnect(false);
            return 0;
        case IDM_DISCONNECT:
            Disconnect();
            SetStatus(L"detached (session kept)", 0);
            return 0;
        case IDM_CLOSE_SESSION:
            CloseSession();
            return 0;
        case IDM_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case IDM_COPY:
            if (g.view_hwnd) SendMessageW(g.view_hwnd, WM_COPY, 0, 0);
            return 0;
        case IDM_PASTE:
            if (g.view_hwnd) SendMessageW(g.view_hwnd, WM_PASTE, 0, 0);
            return 0;
        case IDM_ABOUT:
            MessageBoxW(hwnd,
                        L"WinConsoleTerminal " WCS_VERSION_W
                        L"\nNative Win32 terminal for WinConsoleServer.",
                        L"About", MB_ICONINFORMATION);
            return 0;
        default:
            break;
        }
        break;

    case WM_APP_AUTOCONNECT:
        if (g.auto_connect) {
            StartConnection(g.host, g.port, g.shell, g.token);
        } else {
            ShowConnectDialog();
        }
        return 0;

    case WM_APP_ERROR: {
        if (g.closing) return 0;
        std::wstring error = g.session ? g.session->LastError() : L"connection lost";
        if (error.empty()) error = L"connection lost";
        CloseCurrentSession();
        ScheduleAutoReconnect(error, 0);
        return 0;
    }

    case WM_APP_AUTO_RECONNECT:
        if (g.state == ConnectionState::Reconnecting) {
            TryReconnect(true);
        }
        return 0;

    case WM_APP_AUTO_RECONNECT_FAILED:
        if (g.state == ConnectionState::Reconnecting) {
            g.reconnect_in_flight = false;
            ++g.reconnect_attempts;
            g.next_reconnect_tick_ms =
                GetTickCount64() + ReconnectBackoffMs(g.reconnect_attempts);
            UpdateStatus();
        }
        return 0;

    case WM_APP_EXIT: {
        const int code = (int)wp;
        CloseCurrentSession();
        CancelScheduledReconnect();
        g.server_job_id = 0;
        g.last_error.clear();
        g.state = ConnectionState::Exited;
        UpdateStatus();
        SetStatus(L"process exited (code " + std::to_wstring(code) + L")", 0);
        return 0;
    }

    case WM_APP_RESIZED: {
        const int cols = LOWORD(lp);
        const int rows = HIWORD(lp);
        SetStatus(std::to_wstring(cols) + L" x " + std::to_wstring(rows), 3);
        return 0;
    }

    case WM_TIMER:
        if (wp == kConnectionTimerId) {
            RunAutoReconnect(hwnd);
        }
        return 0;

    case WM_CLOSE:
        g.closing = true;
        KillTimer(hwnd, kConnectionTimerId);
        if (g.session) {
            const int choice = MessageBoxW(
                hwnd,
                L"Terminate the remote session?\n\n"
                L"Yes: terminate the session (WCSP CLOSE).\n"
                L"No: detach and keep it running for a later attach.\n"
                L"Cancel: keep the window open.",
                L"WinConsoleTerminal", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDCANCEL) {
                g.closing = false;
                SetTimer(hwnd, kConnectionTimerId, 500, nullptr);
                return 0;
            }
            if (choice == IDYES) {
                g.session->TerminateSession();
            } else {
                g.session->Close();
            }
            g.session = nullptr;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kConnectionTimerId);
        if (g.view) {
            delete g.view;
            g.view = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ParseCommandLine(int& argc, wchar_t**& argv) {
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        auto next = [&](std::wstring& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (arg == L"--host") {
            std::wstring value;
            if (!next(value)) return false;
            g.host = value;
            g.auto_connect = true;
        } else if (arg == L"--port") {
            std::wstring value;
            if (!next(value)) return false;
            g.port = _wtoi(value.c_str());
            g.auto_connect = true;
        } else if (arg == L"--shell") {
            std::wstring value;
            if (!next(value)) return false;
            g.shell = value;
        } else if (arg == L"--token") {
            std::wstring value;
            if (!next(value)) return false;
            g.token = value;
        } else if (arg == L"--connect") {
            g.auto_connect = true;
        } else if (arg == L"--help" || arg == L"-h") {
            MessageBoxW(nullptr,
                        L"WinConsoleTerminal.exe [--host host] [--port port] "
                        L"[--shell cmd|powershell|bash] [--token token] [--connect]",
                        L"WinConsoleTerminal", MB_ICONINFORMATION);
            return false;
        }
    }
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    LoadSettings();

    int argc = 0;
    wchar_t** argv = nullptr;
    if (!ParseCommandLine(argc, argv)) return 1;
    if (argv) LocalFree(argv);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    if (!TerminalView::RegisterWindowClass(instance)) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kMainClass;
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(
        0, kMainClass, L"WinConsoleTerminal",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
