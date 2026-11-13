#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include "server_config.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    bool Start(std::wstring* error);
    int Run();
    void Stop();

private:
    void AcceptLoop();

    ServerConfig config_;
    SOCKET listen_socket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::shared_ptr<std::atomic<int>> session_count_;
    std::atomic<uint32_t> next_id_{1};
};
