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
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

constexpr wchar_t kMainClass[] = L"WinConsoleTerminalMain";

constexpr UINT WM_APP_AUTOCONNECT = WM_APP + 1;
constexpr UINT WM_APP_ERROR       = WM_APP + 2;
constexpr UINT WM_APP_EXIT        = WM_APP + 3;
constexpr UINT WM_APP_RESIZED     = WM_APP + 4;

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
}

void SaveSettings() {
    const std::wstring ini = SettingsPath();
    WritePrivateProfileStringW(L"connection", L"host", g.host.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"connection", L"port",
                               std::to_wstring(g.port).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"connection", L"shell", g.shell.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"connection", L"token", g.token.c_str(), ini.c_str());
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

void Disconnect() {
    if (g.session) {
        g.session->Close();
        g.session = nullptr;
    }
    SetStatus(L"disconnected", 0);
}

bool StartConnection(const std::wstring& host, int port,
                     const std::wstring& shell, const std::wstring& token) {
    if (host.empty()) {
        MessageBoxW(g.hwnd, L"Host is empty.", L"Connect", MB_ICONWARNING);
        return false;
    }

    Disconnect();

    RemoteConfig config;
    config.host = WideToUtf8Client(host);
    config.port = (uint16_t)port;
    config.shell = WideToUtf8Client(shell);
    config.token = WideToUtf8Client(token);

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
    opt.shell = shell;
    opt.cols = 0;
    opt.rows = 0;
    opt.codePage = CP_UTF8;

    std::wstring error;
    if (!g.view->StartSession(opt, &error)) {
        g.session = nullptr;
        SetStatus(L"connect failed", 0);
        MessageBoxW(g.hwnd, error.empty() ? L"Connection failed." : error.c_str(),
                    L"Connect", MB_ICONERROR);
        return false;
    }

    SetStatus(L"connected", 0);
    SetStatus(host + L":" + std::to_wstring(port), 1);
    SetStatus(shell, 2);
    SetStatus(std::to_wstring(g.view->Columns()) + L" x " +
              std::to_wstring(g.view->Rows()), 3);
    return true;
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
        std::wstring error = g.session ? g.session->LastError() : L"connection lost";
        Disconnect();
        SetStatus(error.empty() ? L"connection lost" : error, 0);
        return 0;
    }

    case WM_APP_EXIT: {
        const int code = (int)wp;
        if (g.session) {
            g.session->Close();
            g.session = nullptr;
        }
        SetStatus(L"process exited (code " + std::to_wstring(code) + L")", 0);
        return 0;
    }

    case WM_APP_RESIZED: {
        const int cols = LOWORD(lp);
        const int rows = HIWORD(lp);
        SetStatus(std::to_wstring(cols) + L" x " + std::to_wstring(rows), 3);
        return 0;
    }

    case WM_CLOSE:
        if (g.session) {
            const int choice = MessageBoxW(
                hwnd,
                L"Terminate the remote session?\n\n"
                L"Yes: terminate the session (WCSP CLOSE).\n"
                L"No: detach and keep it running for a later attach.\n"
                L"Cancel: keep the window open.",
                L"WinConsoleTerminal", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDCANCEL) return 0;
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
