#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../common/conpty_session.h"
#include "../common/frame_reader.h"
#include "../common/protocol.h"
#include "server_config.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class Session {
public:
    Session(SOCKET socket, const ServerConfig& config, uint32_t id,
            bool allowed, std::shared_ptr<std::atomic<int>> session_count);
    ~Session();

    void Run();

private:
    bool ReadFrame(wcs::Frame& frame, int timeout_ms, std::string* error);
    bool SendFrame(const wcs::Frame& frame);
    bool SendError(wcs::ErrorCode code, const std::string& message);
    void EnqueueFrame(wcs::Frame frame);
    void WriterLoop();
    void HandleFrame(const wcs::Frame& frame, bool& close_requested,
                     wcs::ExitReason& exit_reason);

    SOCKET socket_ = INVALID_SOCKET;
    ServerConfig config_;
    uint32_t id_ = 0;
    bool allowed_ = true;
    std::shared_ptr<std::atomic<int>> session_count_;

    ConPtySession pty_;
    wcs::FrameReader reader_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<wcs::Frame> queue_;
    size_t queue_bytes_ = 0;
    bool stop_after_drain_ = false;

    std::thread writer_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> writer_failed_{false};
};
