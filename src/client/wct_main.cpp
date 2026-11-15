#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wct_client.h"
#include "version.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

namespace {

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

void PrintUsage() {
    std::cout <<
"wct " WCS_VERSION R"( - WinConsoleServer WCSP terminal (CUI)

USAGE:
  wct [options] [job-id]

  Connects to a WinConsoleServer over the WCSP protocol and relays the remote
  ConPTY to this console. Keyboard input goes to the remote shell unchanged.

  Leaving wct with Ctrl+] detaches: the remote ConPTY job keeps running and
  can be reattached with `wct <job-id>`. Pressing Ctrl+] twice in quick
  succession or Ctrl+\ ends the session (WCSP CLOSE), which terminates the
  remote job. The session also ends when the remote shell exits.

OPTIONS:
  --host <host>       Server host (default 127.0.0.1).
  --port <port>       WCSP port (default 7000).
  --token <token>     WCSP shared token (or the WCS_TOKEN env var).
  --shell <name>      cmd (default), powershell, or bash.
  --attach <id>       Attach to a retained job instead of starting a new shell.
  --list              Print the retained sessions and exit.
  -h, --help          Show this help.
  -V, --version       Show the version.

A bare numeric argument is short for --attach <id>.

KEYS:
  Ctrl+]              Detach and exit wct; the remote job keeps running.
  Ctrl+] twice        Close the session (terminate the remote job) and exit
                      (the second press must follow within 600 ms).
  Ctrl+\              Close the session (terminate the remote job) and exit.

EXAMPLES:
  wct                          # new cmd session on 127.0.0.1:7000
  wct --list                   # list retained sessions
  wct 12                       # attach to job #12
  wct --shell powershell
  wct --host 192.168.1.10 --token <token> --attach 12
)";
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

bool IsDigits(const std::string& text) {
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

bool IsKnownShell(const std::string& text) {
    return text == "powershell" || text == "cmd" || text == "bash" ||
           text == "pwsh" || text == "powershell.exe" || text == "cmd.exe" ||
           text == "bash.exe";
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        args.push_back(WideToUtf8(argvW[i]));
    }
    LocalFree(argvW);

    WctOptions options;
    bool want_list = false;

    auto requireValue = [&](size_t& i, const std::string& flag, std::string& out) {
        if (i + 1 >= args.size()) {
            std::cerr << "wct: " << flag << " requires a value\n";
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
            std::cout << "wct " WCS_VERSION << "\n";
            return 0;
        }
        if (a == "--list" || a == "list") {
            want_list = true;
        } else if (a == "--host") {
            if (!requireValue(i, a, options.host)) return 1;
        } else if (a.rfind("--host=", 0) == 0) {
            options.host = a.substr(7);
        } else if (a == "--port") {
            std::string value;
            if (!requireValue(i, a, value)) return 1;
            if (!ParsePort(value, options.port)) {
                std::cerr << "wct: invalid --port value: " << value << "\n";
                return 1;
            }
        } else if (a.rfind("--port=", 0) == 0) {
            if (!ParsePort(a.substr(7), options.port)) {
                std::cerr << "wct: invalid --port value: " << a.substr(7) << "\n";
                return 1;
            }
        } else if (a == "--token") {
            if (!requireValue(i, a, options.token)) return 1;
        } else if (a.rfind("--token=", 0) == 0) {
            options.token = a.substr(8);
        } else if (a == "--shell") {
            if (!requireValue(i, a, options.target)) return 1;
        } else if (a.rfind("--shell=", 0) == 0) {
            options.target = a.substr(8);
        } else if (a == "--attach") {
            std::string id;
            if (!requireValue(i, a, id)) return 1;
            options.target = "attach:" + id;
        } else if (a.rfind("--attach=", 0) == 0) {
            options.target = "attach:" + a.substr(9);
        } else if (a.size() > 1 && a[0] == '-') {
            std::cerr << "wct: unknown option: " << a << "\n";
            std::cerr << "Run `wct --help` for usage.\n";
            return 1;
        } else if (a.rfind("attach:", 0) == 0) {
            options.target = a;
        } else if (IsDigits(a)) {
            options.target = "attach:" + a;
        } else if (IsKnownShell(a)) {
            options.target = a;
        } else {
            std::cerr << "wct: unknown argument: " << a << "\n";
            std::cerr << "Run `wct --help` for usage.\n";
            return 1;
        }
    }

    if (want_list) return ListWctSessions(options);
    return RunWct(options);
}
