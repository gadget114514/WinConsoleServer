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

#include <cwchar>
#include <memory>
#include <string>

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

    GetPrivateProfileStringW(L"connection", L"host", L"", buf, 512, ini.c_str());
    g.host = buf;
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

void ShowConnectDialog() {
    if (DialogBoxParamW(GetModuleHandleW(nullptr),
                        MAKEINTRESOURCEW(IDD_CONNECT), g.hwnd,
                        ConnectProc, 0) == IDOK) {
        StartConnection(g.host, g.port, g.shell, g.token);
    }
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
        case IDM_CONNECT:
            ShowConnectDialog();
            return 0;
        case IDM_DISCONNECT:
            Disconnect();
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
                        L"WinConsoleTerminal 0.1\nNative Win32 terminal for WinConsoleServer.",
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
        Disconnect();
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
