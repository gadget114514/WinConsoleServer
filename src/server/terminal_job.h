#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../common/conpty_session.h"
#include "../common/protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Job state numbers match the WCSP SESSION_LIST encoding.
enum class JobState : uint8_t {
    Running = 0,
    Completed = 1,
    Failed = 2,
    TimedOut = 3,
    IdleTimedOut = 4,
    Killed = 5,
};

// A retained ConPTY job.  WCSP connections subscribe to it; the job keeps
// running when a client disconnects (a detach).  Only an explicit CLOSE
// terminates it.
class TerminalJob {
public:
    using OutputFn = std::function<void(const char*, size_t)>;

    TerminalJob(int id, std::string name, std::string command,
                size_t history_bytes);
    ~TerminalJob();

    TerminalJob(const TerminalJob&) = delete;
    TerminalJob& operator=(const TerminalJob&) = delete;

    bool Start(const std::wstring& command_line, int cols, int rows,
               std::wstring* error);

    // Returns a subscription id (>0) or 0 on failure. `replay` receives the
    // retained raw output tail.
    int AddSubscriber(OutputFn sink, std::string& replay);
    void RemoveSubscriber(int sub_id);

    bool Write(const void* data, size_t len, bool front);
    bool Resize(int cols, int rows);

    // Explicit termination (WCSP CLOSE).  The monitor thread finalizes the
    // state; this only asks the process to exit.
    void Terminate();

    // Called by the registry monitor thread.
    void Poll();

    bool IsRunning() const;
    int32_t ExitCode() const;
    int id() const { return id_; }

    wcs::SessionInfo Info() const;

private:
    void OnOutput(const char* data, size_t len);
    void ClosePtyLocked();

    const int id_;
    const std::string name_;
    const std::string command_;
    const size_t history_bytes_;

    ConPtySession pty_;

    mutable std::mutex mutex_;
    std::string history_;
    std::map<int, OutputFn> subscribers_;
    int next_sub_id_ = 0;

    std::mutex lifecycle_mutex_;
    bool finalized_ = false;
    std::atomic<bool> terminate_requested_{false};

    std::atomic<bool> running_{false};
    std::atomic<int32_t> exit_code_{0};
    std::atomic<uint8_t> state_{static_cast<uint8_t>(JobState::Running)};
};

// Owns every retained job for the lifetime of the server and polls them for
// process exit.
class JobRegistry {
public:
    explicit JobRegistry(size_t history_bytes);
    ~JobRegistry();

    JobRegistry(const JobRegistry&) = delete;
    JobRegistry& operator=(const JobRegistry&) = delete;

    std::shared_ptr<TerminalJob> Create(const std::string& name,
                                        const std::string& command_line,
                                        int cols, int rows,
                                        std::wstring* error);
    std::shared_ptr<TerminalJob> Find(int id) const;
    // Terminates the job (if still running) and removes it from the retained
    // session list. Returns false when the id is unknown.
    bool Remove(int id);
    std::vector<wcs::SessionInfo> List() const;

    void Start();
    void Shutdown();

private:
    void MonitorLoop();

    const size_t history_bytes_;
    mutable std::mutex mutex_;
    std::map<int, std::shared_ptr<TerminalJob>> jobs_;
    int next_id_ = 1;

    std::thread monitor_;
    std::atomic<bool> running_{false};
    std::condition_variable cv_;
};
