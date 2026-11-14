#include "server.h"
#include "server_config.h"
#include "windows_service.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace {
Server* g_server = nullptr;

std::wstring AbsolutePath(const std::wstring& path) {
    if (path.empty()) return path;
    wchar_t buffer[MAX_PATH * 4] = {};
    DWORD n = GetFullPathNameW(path.c_str(), (DWORD)(sizeof(buffer) / sizeof(buffer[0])),
                               buffer, nullptr);
    if (n == 0 || n >= sizeof(buffer) / sizeof(buffer[0])) return path;
    return std::wstring(buffer, n);
}

BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        if (g_server) g_server->Stop();
        return TRUE;
    }
    return FALSE;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    ServerConfig config;
    std::wstring error;
    if (!ParseServerArgs(argc, argv, config, &error)) {
        if (!error.empty()) {
            std::fwprintf(stderr, L"error: %s\n", error.c_str());
            return 1;
        }
        return 0;
    }

    if (!config.token_file.empty()) {
        config.token_file = WideToUtf8Server(
            AbsolutePath(Utf8ToWideServer(config.token_file)));
    }
    if (!config.log_file.empty()) {
        config.log_file = WideToUtf8Server(
            AbsolutePath(Utf8ToWideServer(config.log_file)));
    }

    if (config.install_service) {
        bool started = false;
        std::wstring token_file;
        if (!InstallWindowsService(config, &error, &started, &token_file)) {
            std::fwprintf(stderr, L"install failed: %s\n", error.c_str());
            return 1;
        }
        std::wprintf(L"Installed service \"%s\"%s.\n",
                     Utf8ToWideServer(config.service_name).c_str(),
                     started ? L" and started it" : L"");
        if (!token_file.empty()) {
            std::wprintf(L"Token file: %s\n", token_file.c_str());
        }
        return 0;
    }

    if (config.uninstall_service) {
        if (!UninstallWindowsService(config.service_name, &error)) {
            std::fwprintf(stderr, L"uninstall failed: %s\n", error.c_str());
            return 1;
        }
        std::wprintf(L"Removed service \"%s\".\n",
                     Utf8ToWideServer(config.service_name).c_str());
        return 0;
    }

    if (config.run_as_service) {
        if (!ResolveServerToken(config, &error)) {
            std::fwprintf(stderr, L"token error: %s\n", error.c_str());
            return 1;
        }
        return RunAsService(config);
    }

    if (!ResolveServerToken(config, &error)) {
        std::fwprintf(stderr, L"token error: %s\n", error.c_str());
        return 1;
    }

    if (!config.log_file.empty()) {
        std::wstring log_error;
        if (!RedirectServerOutput(config.log_file, &log_error)) {
            std::fwprintf(stderr, L"log file error: %s\n", log_error.c_str());
        }
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    Server server(config);
    g_server = &server;

    if (!server.Start(&error)) {
        std::fwprintf(stderr, L"start failed: %s\n", error.c_str());
        return 1;
    }

    int rc = server.Run();
    g_server = nullptr;
    return rc;
}
