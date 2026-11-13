#include "server.h"
#include "session.h"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

Server::Server(ServerConfig config)
    : config_(std::move(config)),
      session_count_(std::make_shared<std::atomic<int>>(0)) {}

Server::~Server() {
    Stop();
}

bool Server::Start(std::wstring* error) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (error) *error = L"WSAStartup failed";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    const std::string port = std::to_string(config_.port);
    addrinfo* result = nullptr;
    int rc = getaddrinfo(config_.listen_addr.empty() ? nullptr : config_.listen_addr.c_str(),
                         port.c_str(), &hints, &result);
    if (rc != 0) {
        if (error) *error = L"getaddrinfo failed";
        WSACleanup();
        return false;
    }

    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

        if (bind(s, ai->ai_addr, (int)ai->ai_addrlen) == 0 &&
            listen(s, SOMAXCONN) == 0) {
            listen_socket_ = s;
            break;
        }
        closesocket(s);
    }
    freeaddrinfo(result);

    if (listen_socket_ == INVALID_SOCKET) {
        if (error) *error = L"bind/listen failed on port " +
                            std::to_wstring(config_.port);
        WSACleanup();
        return false;
    }

    running_ = true;
    return true;
}

int Server::Run() {
    if (listen_socket_ == INVALID_SOCKET) return 1;

    std::printf("WinConsoleServer listening on %s:%u\n",
                config_.listen_addr.c_str(), (unsigned)config_.port);
    std::printf("shell=%s max_sessions=%d auth=%s\n",
                config_.shell.c_str(), config_.max_sessions,
                config_.token.empty() ? "off" : "token");
    std::fflush(stdout);

    AcceptLoop();

    if (listen_socket_ != INVALID_SOCKET) {
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }
    WSACleanup();
    return 0;
}

void Server::Stop() {
    running_ = false;
    if (listen_socket_ != INVALID_SOCKET) {
        shutdown(listen_socket_, SD_BOTH);
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }
}

void Server::AcceptLoop() {
    while (running_) {
        sockaddr_in addr{};
        int addrLen = sizeof(addr);
        SOCKET client = accept(listen_socket_, (sockaddr*)&addr, &addrLen);
        if (client == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }

        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));

        const bool allowed = session_count_->load() < config_.max_sessions;
        const uint32_t id = next_id_.fetch_add(1);

        char ip[64] = {};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        std::printf("[accept] %s:%u id=%u\n", ip, (unsigned)ntohs(addr.sin_port), id);
        std::fflush(stdout);

        ServerConfig config = config_;
        std::shared_ptr<std::atomic<int>> count = session_count_;
        std::thread([client, config, id, allowed, count] {
            Session session(client, config, id, allowed, count);
            session.Run();
        }).detach();
    }
}
