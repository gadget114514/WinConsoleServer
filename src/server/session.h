#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../common/frame_reader.h"
#include "../common/protocol.h"
#include "server_config.h"
#include "terminal_job.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SessionHub;

// One WCSP TCP connection.  It either attaches to a retained job, creates a
// new one, or runs a short-lived control session that only lists sessions.
class Session {
public:
    Session(SOCKET socket, const ServerConfig& config, uint32_t id,
            bool allowed, std::shared_ptr<std::atomic<int>> session_count,
            std::shared_ptr<JobRegistry> jobs,
            std::shared_ptr<std::atomic<bool>> server_stopping,
            std::shared_ptr<SessionHub> hub);
    ~Session();

    void Run();

    uint32_t Id() const { return id_; }
    int JobId() const { return job_id_.load(); }
    bool WantsSizeSync() const { return size_sync_.load(); }

    // Sends RESIZED{cols, rows, Peer} to this client when it negotiated
    // FeatureSizeSync. Safe to call from another connection's thread.
    void NotifyPeerResize(uint16_t cols, uint16_t rows);

private:
    bool ReadFrame(wcs::Frame& frame, int timeout_ms, std::string* error);
    bool SendFrame(const wcs::Frame& frame);
    bool SendError(wcs::ErrorCode code, const std::string& message);
    void EnqueueFrame(wcs::Frame frame, bool priority);
    void QueueReplay(const std::string& replay);
    void WriterLoop();
    void StartWriter();
    void JoinWriter();
    void ShutdownSocket();
    void OnJobOutput(const char* data, size_t len);
    bool HandleFrame(const wcs::Frame& frame, wcs::ExitReason& exit_reason);
    void Finish(wcs::ExitReason reason, int exit_code, bool ended);
    void RunControlSession();
    void CloseSocket();

    SOCKET socket_ = INVALID_SOCKET;
    ServerConfig config_;
    uint32_t id_ = 0;
    bool allowed_ = true;
    std::shared_ptr<std::atomic<int>> session_count_;
    std::shared_ptr<JobRegistry> jobs_;
    std::shared_ptr<TerminalJob> job_;
    std::shared_ptr<std::atomic<bool>> server_stopping_;
    std::shared_ptr<SessionHub> hub_;
    std::atomic<int> job_id_{0};
    std::atomic<bool> size_sync_{false};

    wcs::FrameReader reader_;

    int sub_id_ = 0;
    bool spawned_ = false;
    bool terminate_requested_ = false;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<wcs::Frame> queue_;
    size_t queue_bytes_ = 0;
    size_t queue_limit_ = 8u * 1024u * 1024u;
    std::vector<uint8_t> pending_live_;
    bool replay_pending_ = false;
    bool stop_after_drain_ = false;

    std::thread writer_;
    bool writer_started_ = false;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> writer_abort_{false};
};
