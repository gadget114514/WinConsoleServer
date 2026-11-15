#include "server_config.h"
#include "../common/version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>

std::string WideToUtf8Server(const std::wstring& text) {
    if (text.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWideServer(const std::string& text) {
    if (text.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), need);
    return out;
}

namespace {

std::wstring DefaultWorkingDirectory() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path))) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }
    wchar_t env[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return env;
    return L".";
}

std::wstring SystemPath(const wchar_t* relative) {
    wchar_t dir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring result(dir);
    result += L"\\";
    result += relative;
    return result;
}

std::wstring Quote(const std::wstring& path) {
    return L"\"" + path + L"\"";
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FindBash() {
    // Prefer Git for Windows. SearchPathW may find the WSL launcher, which is
    // not suitable for ConPTY here.
    const wchar_t* candidates[] = {
        L"C:\\Program Files\\Git\\bin\\bash.exe",
        L"C:\\Program Files\\Git\\usr\\bin\\bash.exe",
        L"C:\\Program Files (x86)\\Git\\bin\\bash.exe",
    };
    for (const wchar_t* path : candidates) {
        if (FileExists(path)) return path;
    }
    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        std::wstring path = std::wstring(localAppData) + L"\\Programs\\Git\\bin\\bash.exe";
        if (FileExists(path)) return path;
    }
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"bash.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        std::wstring result(found);
        if (result.find(L"System32") == std::wstring::npos) return result;
    }
    return {};
}

bool IsAllowed(const std::string& name, const ServerConfig& cfg) {
    for (const std::string& allowed : cfg.allowed_shells) {
        if (_stricmp(allowed.c_str(), name.c_str()) == 0) return true;
    }
    return false;
}

void PrintServerHelp() {
    std::wprintf(
        L"WinConsoleServer %s - WCSP terminal server\n"
        L"\n"
        L"Usage:\n"
        L"  WinConsoleServer.exe [options]\n"
        L"  WinConsoleServer.exe help\n"
        L"\n"
        L"Default: listen on 0.0.0.0:7000 and start cmd when a client requests it.\n"
        L"\n"
        L"Connection options:\n"
        L"  --listen host:port        Listen address (default 0.0.0.0:7000).\n"
        L"                            Use 127.0.0.1:7000 to restrict to this host.\n"
        L"  --shell cmd|powershell|bash\n"
        L"                            Default shell (default cmd).\n"
        L"  --cwd path                Working directory for spawned shells.\n"
        L"  --allow-arbitrary-shell   Allow shells outside cmd/powershell/bash.\n"
        L"  --max-sessions N          Maximum concurrent sessions (default 8).\n"
        L"  --history-bytes N         Replay buffer per job, 64 KiB..64 MiB\n"
        L"                            (default 32 MiB).\n"
        L"\n"
        L"Authentication options:\n"
        L"  --token secret            Shared WCSP token. If omitted, the token\n"
        L"                            comes from --token-file, WCS_TOKEN, or is\n"
        L"                            disabled in console mode.\n"
        L"  --token-file path         Read the token from a file.\n"
        L"\n"
        L"Logging options:\n"
        L"  --log-file path           Append server logs to a file.\n"
        L"  --verbose, -V             Enable verbose logging.\n"
        L"\n"
        L"Windows service options (administrator):\n"
        L"  --install-service         Install and start the Windows service.\n"
        L"                            If no token is supplied, one is generated at\n"
        L"                            %%ProgramData%%\\WinConsoleServer\\token.txt.\n"
        L"  --uninstall-service       Stop and remove the Windows service.\n"
        L"  --service-name name       Service name (default WinConsoleServer).\n"
        L"  --service                 Run under the Service Control Manager.\n"
        L"                            Do not use this from an interactive shell.\n"
        L"\n"
        L"Help:\n"
        L"  --help, -h, help          Show this help and exit.\n"
        L"\n"
        L"Examples:\n"
        L"  WinConsoleServer.exe --listen 127.0.0.1:7000 --shell cmd\n"
        L"  WinConsoleServer.exe --listen 0.0.0.0:7000 --shell powershell ^\n"
        L"      --token-file C:\\ProgramData\\WinConsoleServer\\token.txt\n"
        L"  WinConsoleServer.exe --install-service --listen 0.0.0.0:7000 ^\n"
        L"      --shell cmd --history-bytes 33554432\n"
        L"  WinConsoleServer.exe --uninstall-service\n"
        L"\n"
        L"Installed paths:\n"
        L"  %%ProgramFiles%%\\WinConsoleServer\\WinConsoleServer.exe\n"
        L"  %%ProgramData%%\\WinConsoleServer\\token.txt\n"
        L"  %%ProgramData%%\\WinConsoleServer\\server.log\n"
        L"  <install>\\doc\\llm-setup.md   Non-interactive setup guide for LLM agents\n"
        L"\n"
        L"Exit codes: 0 = success/help, 1 = invalid arguments or startup error.\n",
        WCS_VERSION_W);
}

} // namespace

bool ResolveShell(const std::string& requested, const ServerConfig& cfg,
                  ShellSpec* spec, std::wstring* error) {
    std::string name = requested.empty() ? cfg.shell : requested;

    if (!cfg.allow_arbitrary_shell && !IsAllowed(name, cfg)) {
        if (error) *error = L"shell not allowed: " + Utf8ToWideServer(name);
        return false;
    }

    std::wstring command;
    if (_stricmp(name.c_str(), "cmd") == 0) {
        wchar_t comspec[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"COMSPEC", comspec, MAX_PATH) > 0) {
            command = Quote(comspec);
        } else {
            command = Quote(SystemPath(L"cmd.exe"));
        }
        spec->display_name = "cmd";
    } else if (_stricmp(name.c_str(), "powershell") == 0) {
        command = Quote(SystemPath(L"WindowsPowerShell\\v1.0\\powershell.exe"));
        spec->display_name = "powershell";
    } else if (_stricmp(name.c_str(), "bash") == 0) {
        std::wstring bash = FindBash();
        if (bash.empty()) {
            if (error) *error = L"bash.exe not found";
            return false;
        }
        command = Quote(bash) + L" --login -i";
        spec->display_name = "bash";
    } else {
        if (!cfg.allow_arbitrary_shell) {
            if (error) *error = L"unknown shell: " + Utf8ToWideServer(name);
            return false;
        }
        command = Utf8ToWideServer(name);
        spec->display_name = name;
    }

    if (command.empty()) {
        if (error) *error = L"failed to resolve shell";
        return false;
    }
    spec->command_line = command;
    return true;
}

bool ParseServerArgs(int argc, wchar_t** argv, ServerConfig& cfg,
                     std::wstring* error) {
    cfg.cwd = WideToUtf8Server(DefaultWorkingDirectory());

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        auto next = [&](std::wstring& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };

        if (arg == L"--listen") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--listen requires a value"; return false; }
            size_t colon = value.rfind(L':');
            if (colon == std::wstring::npos) {
                if (error) *error = L"--listen must be host:port";
                return false;
            }
            cfg.listen_addr = WideToUtf8Server(value.substr(0, colon));
            cfg.port = (uint16_t)std::wcstoul(value.c_str() + colon + 1, nullptr, 10);
        } else if (arg == L"--shell") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--shell requires a value"; return false; }
            cfg.shell = WideToUtf8Server(value);
        } else if (arg == L"--cwd") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--cwd requires a value"; return false; }
            cfg.cwd = WideToUtf8Server(value);
        } else if (arg == L"--token") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--token requires a value"; return false; }
            cfg.token = WideToUtf8Server(value);
        } else if (arg == L"--token-file") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--token-file requires a value"; return false; }
            cfg.token_file = WideToUtf8Server(value);
        } else if (arg == L"--max-sessions") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--max-sessions requires a value"; return false; }
            cfg.max_sessions = std::max(1, _wtoi(value.c_str()));
        } else if (arg == L"--history-bytes") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--history-bytes requires a value"; return false; }
            long long bytes = std::wcstoll(value.c_str(), nullptr, 10);
            if (bytes < 64 * 1024) bytes = 64 * 1024;
            if (bytes > 64ll * 1024 * 1024) bytes = 64ll * 1024 * 1024;
            cfg.history_bytes = static_cast<size_t>(bytes);
        } else if (arg == L"--service") {
            cfg.run_as_service = true;
        } else if (arg == L"--install-service") {
            cfg.install_service = true;
        } else if (arg == L"--uninstall-service") {
            cfg.uninstall_service = true;
        } else if (arg == L"--service-name") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--service-name requires a value"; return false; }
            cfg.service_name = WideToUtf8Server(value);
        } else if (arg == L"--log-file") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--log-file requires a value"; return false; }
            cfg.log_file = WideToUtf8Server(value);
        } else if (arg == L"--allow-arbitrary-shell") {
            cfg.allow_arbitrary_shell = true;
        } else if (arg == L"--verbose" || arg == L"-V") {
            cfg.verbose = true;
        } else if (arg == L"--help" || arg == L"-h" || arg == L"help") {
            PrintServerHelp();
            return false;
        } else {
            if (error) *error = L"unknown argument: " + arg;
            return false;
        }
    }
    return true;
}

namespace {

std::wstring Win32ErrorText(DWORD code);

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

bool ReadTokenFile(const std::wstring& path, std::string& out,
                   std::wstring* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = L"cannot open token file: " + path + L" (" +
                     Win32ErrorText(GetLastError()) + L")";
        }
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        if (error) *error = L"invalid token file size: " + path;
        return false;
    }

    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = out.empty()
                        ? TRUE
                        : ReadFile(file, out.data(), (DWORD)out.size(), &read,
                                   nullptr);
    CloseHandle(file);
    if (!ok || read != out.size()) {
        if (error) *error = L"cannot read token file: " + path;
        return false;
    }
    out = TrimAscii(std::move(out));
    if (out.empty()) {
        if (error) *error = L"token file is empty: " + path;
        return false;
    }
    return true;
}

std::wstring Win32ErrorText(DWORD code) {
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

} // namespace

bool ResolveServerToken(ServerConfig& cfg, std::wstring* error) {
    if (!cfg.token.empty()) return true;

    if (!cfg.token_file.empty()) {
        return ReadTokenFile(Utf8ToWideServer(cfg.token_file), cfg.token, error);
    }

    DWORD length = GetEnvironmentVariableW(L"WCS_TOKEN", nullptr, 0);
    if (length > 0) {
        std::wstring value(length, L'\0');
        DWORD got = GetEnvironmentVariableW(L"WCS_TOKEN", value.data(), length);
        if (got > 0) {
            value.resize(got);
            cfg.token = TrimAscii(WideToUtf8Server(value));
        }
    }
    return true;
}
