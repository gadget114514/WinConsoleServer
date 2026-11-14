#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "remote_session.h"
#include "version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string host = "127.0.0.1";
    // Default WCSP port shared by the server, WinConsoleTerminal and wct.
    int port = 7000;
    std::string token;
    // Read the connection token from this file (e.g. the service token file).
    std::string token_file;
    // wcs-cli settings file (INI).
    std::string config_path;
};

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

std::wstring EnvPath(const wchar_t* name) {
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) return {};
    std::wstring value(length, L'\0');
    DWORD got = GetEnvironmentVariableW(name, value.data(), length);
    if (got == 0) return {};
    value.resize(got);
    return value;
}

std::wstring AppDataDirectory() {
    std::wstring value = EnvPath(L"APPDATA");
    if (!value.empty()) return value;
    value = EnvPath(L"USERPROFILE");
    if (!value.empty()) return value + L"\\AppData\\Roaming";
    return L".";
}

std::wstring ProgramDataDirectory() {
    std::wstring value = EnvPath(L"ProgramData");
    if (!value.empty()) return value;
    wchar_t windows[MAX_PATH] = {};
    UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(windows) + L"\\ProgramData";
    return L"C:\\ProgramData";
}

std::wstring DefaultConfigPath() {
    return AppDataDirectory() + L"\\WinConsoleServer\\wcs-cli.ini";
}

std::wstring DefaultServiceTokenPath() {
    return ProgramDataDirectory() + L"\\WinConsoleServer\\token.txt";
}

void EnsureParentDirectory(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring dir = path.substr(0, slash);
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != L'\\' && dir[i] != L'/') continue;
        CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
    }
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

std::string ReadTextFile(const std::wstring& path, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot open token file: " + WideToUtf8(path);
        return {};
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        error = "invalid token file size: " + WideToUtf8(path);
        return {};
    }

    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = out.empty() ? TRUE
                          : ReadFile(file, out.data(), (DWORD)out.size(),
                                     &read, nullptr);
    CloseHandle(file);
    if (!ok || read != out.size()) {
        error = "cannot read token file: " + WideToUtf8(path);
        return {};
    }
    out = TrimAscii(std::move(out));
    if (out.empty()) error = "token file is empty: " + WideToUtf8(path);
    return out;
}

bool WriteTokenFile(const std::wstring& path, const std::string& token,
                    std::string& error) {
    EnsureParentDirectory(path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot create " + WideToUtf8(path) + " (run as administrator?)";
        return false;
    }
    std::string data = token;
    data.push_back('\n');
    DWORD written = 0;
    BOOL ok = WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != data.size()) {
        error = "cannot write " + WideToUtf8(path);
        return false;
    }
    return true;
}

std::string GenerateTokenHex() {
    unsigned char buffer[16] = {};
    if (BCryptGenRandom(nullptr, buffer, (ULONG)sizeof(buffer),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        for (size_t i = 0; i < sizeof(buffer); ++i) {
            buffer[i] = (unsigned char)(rand() & 0xFF);
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

bool WriteConfigToken(const std::string& config_path, const std::string& token,
                      std::string& error) {
    const std::wstring path = Utf8ToWide(config_path);
    EnsureParentDirectory(path);
    if (!WritePrivateProfileStringW(L"connection", L"token",
                                    Utf8ToWide(token).c_str(), path.c_str())) {
        error = "cannot write " + config_path + " (run as administrator?)";
        return false;
    }
    return true;
}

void LoadConfig(const std::string& config_path, Options& options,
                bool host_set, bool port_set, bool token_set) {
    if (config_path.empty()) return;
    const std::wstring path = Utf8ToWide(config_path);
    wchar_t buffer[1024] = {};

    if (!host_set) {
        GetPrivateProfileStringW(L"connection", L"host", L"", buffer,
                                 (DWORD)(sizeof(buffer) / sizeof(buffer[0])),
                                 path.c_str());
        if (buffer[0]) options.host = WideToUtf8(buffer);
    }
    if (!port_set) {
        int port = (int)GetPrivateProfileIntW(L"connection", L"port", 0,
                                              path.c_str());
        if (port > 0 && port <= 65535) options.port = port;
    }
    if (!token_set && options.token_file.empty()) {
        GetPrivateProfileStringW(L"connection", L"token", L"", buffer,
                                 (DWORD)(sizeof(buffer) / sizeof(buffer[0])),
                                 path.c_str());
        if (buffer[0]) options.token = WideToUtf8(buffer);
    }
}

bool ResolveToken(Options& options, std::string& error) {
    if (!options.token.empty()) return true;
    if (options.token_file.empty()) return true;
    options.token = ReadTextFile(Utf8ToWide(options.token_file), error);
    return !options.token.empty();
}

void PrintUsage() {
    std::cout <<
"wcs-cli " WCS_VERSION R"( - WinConsoleServer session administration

USAGE:
  wcs-cli [options] [command]

COMMANDS:
  list                 List retained sessions (default).
  delete <id>...       Terminate and remove retained sessions by id.
  delete --all         Terminate and remove every retained session.
  set-token [token]    Write the WCSP token to the settings file. When the
                       token is omitted a new random token is generated.
                       Use --server to write the Windows service token file,
                       or --file <path> to write a specific token file.

OPTIONS:
  --host <host>        Server host (default 127.0.0.1).
  --port <port>        WCSP port (default 7000).
  --token <token>      WCSP shared token (or the WCS_TOKEN env var).
  --token-file <path>  Read the token from a file.
  --config <path>      Settings file (default %APPDATA%\WinConsoleServer\wcs-cli.ini).
  --server             set-token: write %ProgramData%\WinConsoleServer\token.txt.
  --file <path>        set-token: write to a specific file.
  --print-only         set-token: print the token without writing it.
  -h, --help           Show this help.
  -V, --version        Show the version.

EXAMPLES:
  wcs-cli list
  wcs-cli delete 12 13
  wcs-cli set-token                 # generate and save a new token
  wcs-cli set-token secret123
  wcs-cli set-token --server        # token file used by the Windows service
  wcs-cli --token-file C:\ProgramData\WinConsoleServer\token.txt list
)";
}

std::string EnvToken() {
    DWORD len = GetEnvironmentVariableA("WCS_TOKEN", nullptr, 0);
    if (len == 0) return {};
    std::string value(len, '\0');
    DWORD got = GetEnvironmentVariableA("WCS_TOKEN", value.data(), len);
    value.resize(got);
    return TrimAscii(std::move(value));
}

bool ParsePort(const std::string& text, int& out) {
    if (text.empty()) return false;
    for (unsigned char c : text) {
        if (!std::isdigit(c)) return false;
    }
    try {
        out = std::stoi(text);
    } catch (...) {
        return false;
    }
    return out > 0 && out <= 65535;
}

bool ParseId(const std::string& text, int& out) {
    if (text.empty()) return false;
    for (unsigned char c : text) {
        if (!std::isdigit(c)) return false;
    }
    try {
        long value = std::stol(text);
        if (value <= 0 || value > 0x7fffffffL) return false;
        out = static_cast<int>(value);
    } catch (...) {
        return false;
    }
    return true;
}

std::unique_ptr<RemoteSession> MakeSession(const Options& options) {
    RemoteConfig config;
    config.host = options.host;
    config.port = static_cast<uint16_t>(std::clamp(options.port, 1, 65535));
    config.token = options.token.empty() ? EnvToken() : options.token;
    // wcs-cli only uses the control/delete targets; it never draws a PTY.
    config.features = wcs::kDefaultFeatures & ~wcs::FeatureSizeSync;
    config.connect_timeout_ms = 5000;
    return std::make_unique<RemoteSession>(std::move(config));
}

const char* StateName(uint8_t state) {
    switch (state) {
    case 0: return "running";
    case 1: return "completed";
    case 2: return "failed";
    case 3: return "timed out";
    case 4: return "idle-timed out";
    case 5: return "killed";
    default: return "unknown";
    }
}

bool FetchSessions(const Options& options, std::vector<wcs::SessionInfo>& out,
                   std::string& error) {
    auto session = MakeSession(options);

    std::mutex mutex;
    std::condition_variable cv;
    bool received = false;
    bool failed = false;
    std::string failure;

    session->onSessionList = [&](const std::vector<wcs::SessionInfo>& list) {
        std::lock_guard<std::mutex> lock(mutex);
        out = list;
        received = true;
        cv.notify_all();
    };
    session->onError = [&](const wcs::ErrorInfo& info) {
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
        failure = info.message;
        cv.notify_all();
    };

    TerminalStartOptions start;
    start.shell = L"list";
    start.cols = 80;
    start.rows = 24;
    std::wstring start_error;
    if (!session->Start(start, &start_error)) {
        error = WideToUtf8(start_error);
        if (error.empty()) error = "cannot connect";
        return false;
    }

    session->RequestSessionList();
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(5),
                    [&] { return received || failed; });
    }
    session->Close();

    if (failed) {
        error = failure;
        return false;
    }
    if (!received) {
        error = "timed out waiting for SESSION_LIST";
        return false;
    }
    return true;
}

int CommandList(const Options& options) {
    std::vector<wcs::SessionInfo> sessions;
    std::string error;
    if (!FetchSessions(options, sessions, error)) {
        std::cerr << "wcs-cli: " << error << "\n";
        return 1;
    }

    if (sessions.empty()) {
        std::cout << "No sessions.\n";
        return 0;
    }

    std::cout << "ID\tSTATE\tNAME\tCOMMAND\n";
    for (const auto& s : sessions) {
        std::cout << s.id << "\t" << StateName(s.state) << "\t" << s.name;
        if (!s.command.empty()) std::cout << "\t" << s.command;
        std::cout << "\n";
    }
    return 0;
}

bool DeleteOne(const Options& options, int id, std::string& error) {
    auto session = MakeSession(options);

    TerminalStartOptions start;
    start.shell = Utf8ToWide("delete:" + std::to_string(id));
    start.cols = 80;
    start.rows = 24;
    std::wstring start_error;
    if (!session->Start(start, &start_error)) {
        error = WideToUtf8(start_error);
        if (error.empty()) error = "delete failed";
        return false;
    }
    session->Close();
    return true;
}

int CommandDelete(const Options& options, const std::vector<int>& ids,
                  bool all) {
    std::vector<int> targets = ids;
    if (all) {
        std::vector<wcs::SessionInfo> sessions;
        std::string error;
        if (!FetchSessions(options, sessions, error)) {
            std::cerr << "wcs-cli: " << error << "\n";
            return 1;
        }
        targets.clear();
        for (const auto& s : sessions) targets.push_back((int)s.id);
    }

    if (targets.empty()) {
        std::cout << "Nothing to delete.\n";
        return 0;
    }

    int failures = 0;
    for (int id : targets) {
        std::string error;
        if (DeleteOne(options, id, error)) {
            std::cout << "deleted " << id << "\n";
        } else {
            std::cerr << "wcs-cli: " << id << ": " << error << "\n";
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

int CommandSetToken(const Options& options, const std::string& token_arg,
                    bool use_server_file, const std::string& file_path,
                    bool print_only) {
    const bool generated = token_arg.empty();
    const std::string token = generated ? GenerateTokenHex() : token_arg;

    if (use_server_file && !file_path.empty()) {
        std::cerr << "wcs-cli: --server and --file are mutually exclusive\n";
        return 1;
    }

    if (!print_only) {
        std::string error;
        if (!file_path.empty()) {
            const std::wstring path = Utf8ToWide(file_path);
            if (!WriteTokenFile(path, token, error)) {
                std::cerr << "wcs-cli: " << error << "\n";
                return 1;
            }
            std::cout << "wrote token to " << WideToUtf8(path) << "\n";
        } else if (use_server_file) {
            const std::wstring path = DefaultServiceTokenPath();
            if (!WriteTokenFile(path, token, error)) {
                std::cerr << "wcs-cli: " << error << "\n";
                return 1;
            }
            std::cout << "wrote token to " << WideToUtf8(path) << "\n";
            std::cout << "restart the WinConsoleServer service to apply it\n";
        } else {
            const std::string path =
                options.config_path.empty() ? WideToUtf8(DefaultConfigPath())
                                            : options.config_path;
            if (!WriteConfigToken(path, token, error)) {
                std::cerr << "wcs-cli: " << error << "\n";
                return 1;
            }
            std::cout << "wrote token to " << path << "\n";
        }
    }

    if (generated || print_only) {
        std::cout << "token: " << token << "\n";
    }
    return 0;
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(WideToUtf8(argvW[i]));
    LocalFree(argvW);

    Options options;
    bool host_set = false;
    bool port_set = false;
    bool token_set = false;
    bool config_set = false;
    bool use_server_file = false;
    bool print_only = false;
    bool all = false;
    std::string write_file;
    std::vector<std::string> positional;

    auto requireValue = [&](size_t& i, const std::string& flag,
                            std::string& out) {
        if (i + 1 >= args.size()) {
            std::cerr << "wcs-cli: " << flag << " requires a value\n";
            return false;
        }
        out = args[++i];
        return true;
    };

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-h" || a == "--help" || a == "help") {
            PrintUsage();
            return 0;
        }
        if (a == "-V" || a == "--version" || a == "version") {
            std::cout << "wcs-cli " WCS_VERSION << "\n";
            return 0;
        }
        if (a == "--host") {
            if (!requireValue(i, a, options.host)) return 1;
            host_set = true;
        } else if (a.rfind("--host=", 0) == 0) {
            options.host = a.substr(7);
            host_set = true;
        } else if (a == "--port") {
            std::string value;
            if (!requireValue(i, a, value)) return 1;
            if (!ParsePort(value, options.port)) {
                std::cerr << "wcs-cli: invalid --port value: " << value << "\n";
                return 1;
            }
            port_set = true;
        } else if (a.rfind("--port=", 0) == 0) {
            if (!ParsePort(a.substr(7), options.port)) {
                std::cerr << "wcs-cli: invalid --port value: " << a.substr(7)
                          << "\n";
                return 1;
            }
            port_set = true;
        } else if (a == "--token") {
            if (!requireValue(i, a, options.token)) return 1;
            token_set = true;
        } else if (a.rfind("--token=", 0) == 0) {
            options.token = a.substr(8);
            token_set = true;
        } else if (a == "--token-file") {
            if (!requireValue(i, a, options.token_file)) return 1;
        } else if (a.rfind("--token-file=", 0) == 0) {
            options.token_file = a.substr(13);
        } else if (a == "--config") {
            if (!requireValue(i, a, options.config_path)) return 1;
            config_set = true;
        } else if (a.rfind("--config=", 0) == 0) {
            options.config_path = a.substr(9);
            config_set = true;
        } else if (a == "--server") {
            use_server_file = true;
        } else if (a == "--file") {
            if (!requireValue(i, a, write_file)) return 1;
        } else if (a.rfind("--file=", 0) == 0) {
            write_file = a.substr(7);
        } else if (a == "--print-only") {
            print_only = true;
        } else if (a == "--all") {
            all = true;
        } else if (a.size() > 1 && a[0] == '-') {
            std::cerr << "wcs-cli: unknown option: " << a << "\n";
            return 1;
        } else {
            positional.push_back(a);
        }
    }

    if (!config_set) options.config_path = WideToUtf8(DefaultConfigPath());

    std::string command = positional.empty() ? "list" : positional[0];

    if (command == "set-token" || command == "token" ||
        command == "write-token") {
        if (positional.size() > 2) {
            std::cerr << "wcs-cli: set-token takes at most one token\n";
            return 1;
        }
        const std::string token_arg =
            positional.size() == 2 ? positional[1] : std::string();
        return CommandSetToken(options, token_arg, use_server_file, write_file,
                               print_only);
    }

    LoadConfig(options.config_path, options, host_set, port_set, token_set);

    std::string token_error;
    if (!ResolveToken(options, token_error)) {
        std::cerr << "wcs-cli: " << token_error << "\n";
        return 1;
    }

    if (command == "list" || command == "ls" || command == "sessions") {
        return CommandList(options);
    }
    if (command == "delete" || command == "rm" || command == "remove" ||
        command == "kill") {
        std::vector<int> ids;
        for (size_t i = 1; i < positional.size(); ++i) {
            int id = 0;
            if (!ParseId(positional[i], id)) {
                std::cerr << "wcs-cli: invalid session id: " << positional[i]
                          << "\n";
                return 1;
            }
            ids.push_back(id);
        }
        return CommandDelete(options, ids, all);
    }

    std::cerr << "wcs-cli: unknown command: " << command << "\n";
    std::cerr << "Run `wcs-cli --help` for usage.\n";
    return 1;
}
