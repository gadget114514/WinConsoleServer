#include "windows_service.h"

#include "server.h"
#include "server_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <io.h>
#include <string>

namespace {

ServerConfig g_service_config;
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
Server* g_service_server = nullptr;
DWORD g_checkpoint = 0;
std::wstring g_service_name;

std::wstring QuoteArg(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

void SetError(std::wstring* error, const wchar_t* context, DWORD code);

void EnsureParentDirectory(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring dir = path.substr(0, slash);
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != L'\\' && dir[i] != L'/') continue;
        CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
    }
}

std::wstring ProgramDataDirectory() {
    DWORD length = GetEnvironmentVariableW(L"ProgramData", nullptr, 0);
    if (length > 0) {
        std::wstring value(length, L'\0');
        DWORD got = GetEnvironmentVariableW(L"ProgramData", value.data(), length);
        if (got > 0) {
            value.resize(got);
            if (!value.empty()) return value;
        }
    }
    wchar_t windows[MAX_PATH] = {};
    UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::wstring(windows) + L"\\ProgramData";
    }
    return L"C:\\ProgramData";
}

std::string TrimAscii(std::string text) {
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t begin = 0;
    while (begin < text.size() && is_space((unsigned char)text[begin])) ++begin;
    size_t end = text.size();
    while (end > begin && is_space((unsigned char)text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::string ReadTextFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = out.empty() ? TRUE
                          : ReadFile(file, out.data(), (DWORD)out.size(),
                                     &read, nullptr);
    CloseHandle(file);
    if (!ok || read != out.size()) return {};
    return TrimAscii(std::move(out));
}

bool WriteTextFile(const std::wstring& path, const std::string& data,
                   std::wstring* error) {
    EnsureParentDirectory(path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, L"cannot create token file", GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = data.empty() ? TRUE
                           : WriteFile(file, data.data(), (DWORD)data.size(),
                                       &written, nullptr);
    CloseHandle(file);
    if (!ok || written != data.size()) {
        SetError(error, L"cannot write token file", GetLastError());
        return false;
    }
    return true;
}

std::string GenerateTokenHex() {
    unsigned char buffer[16] = {};
    if (BCryptGenRandom(nullptr, buffer, (ULONG)sizeof(buffer),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        for (size_t i = 0; i < sizeof(buffer); ++i) {
            buffer[i] = (unsigned char)(std::rand() & 0xFF);
        }
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(buffer) * 2);
    for (unsigned char b : buffer) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

std::wstring ExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(),
                                    static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring BuildServiceArguments(const ServerConfig& cfg) {
    std::wstring args;
    auto add = [&](const std::wstring& value) {
        if (!args.empty()) args += L' ';
        args += value;
    };
    auto addValue = [&](const wchar_t* flag, const std::wstring& value) {
        add(flag);
        add(QuoteArg(value));
    };

    add(L"--service");
    if (cfg.service_name != "WinConsoleServer") {
        addValue(L"--service-name", Utf8ToWideServer(cfg.service_name));
    }
    addValue(L"--listen",
             Utf8ToWideServer(cfg.listen_addr) + L":" +
                 std::to_wstring(cfg.port));
    addValue(L"--shell", Utf8ToWideServer(cfg.shell));
    if (!cfg.cwd.empty()) addValue(L"--cwd", Utf8ToWideServer(cfg.cwd));
    if (!cfg.token.empty()) addValue(L"--token", Utf8ToWideServer(cfg.token));
    if (!cfg.token_file.empty()) {
        addValue(L"--token-file", Utf8ToWideServer(cfg.token_file));
    }
    addValue(L"--max-sessions", std::to_wstring(cfg.max_sessions));
    addValue(L"--history-bytes", std::to_wstring(cfg.history_bytes));
    if (cfg.allow_arbitrary_shell) add(L"--allow-arbitrary-shell");
    if (!cfg.log_file.empty()) {
        addValue(L"--log-file", Utf8ToWideServer(cfg.log_file));
    }
    if (cfg.verbose) add(L"--verbose");
    return args;
}

std::wstring Win32ErrorMessage(DWORD code) {
    LPWSTR buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0,
                   nullptr);
    std::wstring message = buffer ? buffer : L"unknown error";
    if (buffer) LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

void SetError(std::wstring* error, const wchar_t* context, DWORD code) {
    if (!error) return;
    *error = std::wstring(context) + L": " + Win32ErrorMessage(code);
}

void ReportServiceStatus(DWORD state, DWORD exit_code = NO_ERROR,
                         DWORD wait_hint_ms = 0) {
    if (!g_status_handle) return;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwWaitHint = wait_hint_ms;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN)
            : 0;
    g_status.dwCheckPoint =
        wait_hint_ms != 0 ? ++g_checkpoint : 0;
    SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI ServiceControlHandler(DWORD control, DWORD, void*, void*) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (g_service_server) g_service_server->Stop();
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(
        g_service_name.c_str(), ServiceControlHandler, nullptr);
    if (!g_status_handle) return;

    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 5000);

    if (!g_service_config.log_file.empty()) {
        std::wstring log_error;
        RedirectServerOutput(g_service_config.log_file, &log_error);
    }

    Server server(g_service_config);
    g_service_server = &server;

    std::wstring error;
    if (!server.Start(&error)) {
        g_service_server = nullptr;
        ReportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        return;
    }

    ReportServiceStatus(SERVICE_RUNNING);
    server.Run();
    g_service_server = nullptr;
    ReportServiceStatus(SERVICE_STOPPED);
}

} // namespace

bool RedirectServerOutput(const std::string& log_file, std::wstring* error) {
    if (log_file.empty()) return true;
    const std::wstring path = Utf8ToWideServer(log_file);
    EnsureParentDirectory(path);

    FILE* stream = nullptr;
    if (_wfreopen_s(&stream, path.c_str(), L"a", stdout) != 0 || !stream) {
        SetError(error, L"cannot open log file for stdout", GetLastError());
        return false;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Send stderr to the same handle so both streams append without opening
    // the file twice (which can fail with a sharing violation).
    const int out_fd = _fileno(stdout);
    if (out_fd >= 0) {
        _dup2(out_fd, _fileno(stderr));
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    return true;
}

int RunAsService(const ServerConfig& config) {
    g_service_config = config;
    g_service_name = Utf8ToWideServer(config.service_name);

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(g_service_name.c_str()), ServiceMain },
        { nullptr, nullptr },
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        const DWORD code = GetLastError();
        if (code == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::fprintf(stderr,
                         "This executable must be started by the Service "
                         "Control Manager.\n"
                         "Install it with --install-service or run it without "
                         "--service.\n");
        } else {
            std::fwprintf(stderr, L"StartServiceCtrlDispatcher failed: %s\n",
                          Win32ErrorMessage(code).c_str());
        }
        return 1;
    }
    return 0;
}

bool InstallWindowsService(const ServerConfig& config, std::wstring* error,
                           bool* started, std::wstring* token_file_out) {
    if (started) *started = false;
    if (token_file_out) token_file_out->clear();

    ServerConfig effective = config;
    std::wstring token_file;

    if (effective.token.empty()) {
        if (!effective.token_file.empty()) {
            token_file = Utf8ToWideServer(effective.token_file);
        } else {
            token_file = ProgramDataDirectory() +
                         L"\\WinConsoleServer\\token.txt";
            std::string token = ReadTextFile(token_file);
            if (token.empty()) {
                token = GenerateTokenHex();
                if (!WriteTextFile(token_file, token + "\n", error)) {
                    return false;
                }
            }
            effective.token_file = WideToUtf8Server(token_file);
        }
    }

    if (effective.log_file.empty()) {
        effective.log_file = WideToUtf8Server(
            ProgramDataDirectory() + L"\\WinConsoleServer\\server.log");
    }

    const std::wstring service_name = Utf8ToWideServer(effective.service_name);
    const std::wstring exe = ExecutablePath();
    if (exe.empty()) {
        SetError(error, L"cannot determine the executable path", GetLastError());
        return false;
    }

    const std::wstring command =
        QuoteArg(exe) + L" " + BuildServiceArguments(effective);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        SetError(error, L"OpenSCManager (run as administrator?)",
                 GetLastError());
        return false;
    }

    SC_HANDLE service = CreateServiceW(
        scm, service_name.c_str(), L"WinConsoleServer",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr);
    DWORD create_error = 0;
    if (!service) {
        create_error = GetLastError();
        if (create_error == ERROR_SERVICE_EXISTS) {
            service = OpenServiceW(scm, service_name.c_str(), SERVICE_ALL_ACCESS);
            if (service) {
                if (!ChangeServiceConfigW(
                        service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                        SERVICE_NO_CHANGE, command.c_str(), nullptr, nullptr,
                        nullptr, nullptr, nullptr, L"WinConsoleServer")) {
                    create_error = GetLastError();
                } else {
                    create_error = 0;
                }
            }
        }
    }

    if (!service) {
        SetError(error, L"cannot create or update the service", create_error);
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_DESCRIPTIONW description{};
    description.lpDescription =
        const_cast<LPWSTR>(
            L"WinConsoleServer: ConPTY terminal sessions over WCSP (TCP 7000).");
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    delayed.fDelayedAutostart = TRUE;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                          &delayed);

    if (StartServiceW(service, 0, nullptr)) {
        if (started) *started = true;
    } else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
        if (started) *started = true;
    }

    if (token_file_out) *token_file_out = token_file;

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}

bool UninstallWindowsService(const std::string& service_name_utf8,
                             std::wstring* error) {
    const std::wstring service_name = Utf8ToWideServer(service_name_utf8);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        SetError(error, L"OpenSCManager (run as administrator?)",
                 GetLastError());
        return false;
    }

    SC_HANDLE service = OpenServiceW(
        scm, service_name.c_str(),
        SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD code = GetLastError();
        if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
            CloseServiceHandle(scm);
            return true;
        }
        SetError(error, L"OpenService", code);
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status{};
    if (QueryServiceStatus(service, &status) &&
        status.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS stop_status{};
        ControlService(service, SERVICE_CONTROL_STOP, &stop_status);
        for (int i = 0; i < 50; ++i) {
            Sleep(100);
            if (!QueryServiceStatus(service, &status) ||
                status.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
        }
    }

    if (!DeleteService(service)) {
        SetError(error, L"DeleteService", GetLastError());
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return true;
}
