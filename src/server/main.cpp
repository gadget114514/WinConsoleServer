#include "server.h"
#include "server_config.h"

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
