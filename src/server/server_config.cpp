#include "server_config.h"

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
        } else if (arg == L"--max-sessions") {
            std::wstring value;
            if (!next(value)) { if (error) *error = L"--max-sessions requires a value"; return false; }
            cfg.max_sessions = std::max(1, _wtoi(value.c_str()));
        } else if (arg == L"--allow-arbitrary-shell") {
            cfg.allow_arbitrary_shell = true;
        } else if (arg == L"--verbose" || arg == L"-V") {
            cfg.verbose = true;
        } else if (arg == L"--help" || arg == L"-h") {
            wprintf(L"Usage: WinConsoleServer.exe [options]\n"
                    L"  --listen host:port        default 0.0.0.0:7000\n"
                    L"  --shell cmd|powershell|bash\n"
                    L"  --cwd path\n"
                    L"  --token secret\n"
                    L"  --max-sessions N\n"
                    L"  --allow-arbitrary-shell\n"
                    L"  --verbose\n");
            return false;
        } else {
            if (error) *error = L"unknown argument: " + arg;
            return false;
        }
    }
    return true;
}
