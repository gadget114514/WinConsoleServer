#include "terminal_job.h"

#include "server_config.h"

#include <algorithm>
#include <chrono>
#include <utility>

TerminalJob::TerminalJob(int id, std::string name, std::string command,
                         size_t history_bytes)
    : id_(id), name_(std::move(name)), command_(std::move(command)),
      history_bytes_(history_bytes) {}

TerminalJob::~TerminalJob() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!finalized_) {
        HANDLE process = pty_.GetProcessHandle();
        if (process) TerminateProcess(process, 1);
        pty_.Close();
        finalized_ = true;
    }
}

bool TerminalJob::Start(const std::wstring& command_line, int cols, int rows,
                        std::wstring* error) {
    pty_.SetOutputCallback([this](const char* data, size_t len) {
        OnOutput(data, len);
    });
    if (!pty_.Start(command_line, cols, rows)) {
        if (error) *error = pty_.LastError();
        return false;
    }
    running_ = true;
    return true;
}

void TerminalJob::OnOutput(const char* data, size_t len) {
    if (!data || len == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    history_.append(data, len);
    if (history_.size() > history_bytes_) {
        history_.erase(0, history_.size() - history_bytes_);
    }
    // Subscribers are invoked while `mutex_` is held so AddSubscriber's replay
    // snapshot and the live stream stay contiguous.
    for (auto& kv : subscribers_) {
        if (kv.second) kv.second(data, len);
    }
}

int TerminalJob::AddSubscriber(OutputFn sink, std::string& replay) {
    const size_t limit = history_bytes_;
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_.size() > limit) {
        replay.assign(history_, history_.size() - limit, limit);
    } else {
        replay = history_;
    }
    const int id = ++next_sub_id_;
    subscribers_[id] = std::move(sink);
    return id;
}

void TerminalJob::RemoveSubscriber(int sub_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(sub_id);
}

bool TerminalJob::Write(const void* data, size_t len, bool front) {
    if (!data || len == 0) return false;
    return front ? pty_.WriteFront(data, len) : pty_.Write(data, len);
}

bool TerminalJob::Resize(int cols, int rows) {
    if (cols < wcs::kMinColumns) cols = wcs::kMinColumns;
    if (rows < wcs::kMinRows) rows = wcs::kMinRows;
    if (cols > wcs::kMaxDimension) cols = wcs::kMaxDimension;
    if (rows > wcs::kMaxDimension) rows = wcs::kMaxDimension;
    pty_.Resize(cols, rows);
    return true;
}

void TerminalJob::Terminate() {
    terminate_requested_ = true;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!finalized_) {
            HANDLE process = pty_.GetProcessHandle();
            if (process) TerminateProcess(process, 1);
        }
    }
    // Report the stop immediately so attached clients leave without waiting
    // for the registry monitor (the job may already be unregistered).
    running_ = false;
    state_ = static_cast<uint8_t>(JobState::Killed);
}

void TerminalJob::Poll() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (finalized_) return;

    HANDLE process = pty_.GetProcessHandle();
    if (!process) return;
    if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return;

    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code)) code = 0;
    exit_code_ = static_cast<int32_t>(code);

    // The process has exited; safe to release the PTY and close its handles.
    pty_.Close();
    finalized_ = true;
    running_ = false;
    if (terminate_requested_) {
        state_ = static_cast<uint8_t>(JobState::Killed);
    } else if (code == 0) {
        state_ = static_cast<uint8_t>(JobState::Completed);
    } else {
        state_ = static_cast<uint8_t>(JobState::Failed);
    }
}

bool TerminalJob::IsRunning() const {
    return running_.load() &&
           state_.load() == static_cast<uint8_t>(JobState::Running);
}

int32_t TerminalJob::ExitCode() const { return exit_code_.load(); }

wcs::SessionInfo TerminalJob::Info() const {
    wcs::SessionInfo info;
    info.id = static_cast<uint32_t>(id_);
    info.state = state_.load();
    std::lock_guard<std::mutex> lock(mutex_);
    info.name = name_;
    info.command = command_;
    return info;
}

JobRegistry::JobRegistry(size_t history_bytes) : history_bytes_(history_bytes) {}

JobRegistry::~JobRegistry() { Shutdown(); }

void JobRegistry::Start() {
    if (running_.exchange(true)) return;
    monitor_ = std::thread(&JobRegistry::MonitorLoop, this);
}

void JobRegistry::Shutdown() {
    if (running_.exchange(false)) {
        cv_.notify_all();
        if (monitor_.joinable()) monitor_.join();
    }

    std::vector<std::shared_ptr<TerminalJob>> jobs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : jobs_) jobs.push_back(kv.second);
        jobs_.clear();
    }
    for (auto& job : jobs) {
        if (job) job->Terminate();
    }
}

std::shared_ptr<TerminalJob> JobRegistry::Create(const std::string& name,
                                                 const std::string& command_line,
                                                 int cols, int rows,
                                                 std::wstring* error) {
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_id_++;
    }

    auto job = std::make_shared<TerminalJob>(id, name, command_line,
                                             history_bytes_);
    if (!job->Start(Utf8ToWideServer(command_line), cols, rows, error)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    jobs_[id] = job;
    return job;
}

std::shared_ptr<TerminalJob> JobRegistry::Find(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second;
}

bool JobRegistry::Remove(int id) {
    std::shared_ptr<TerminalJob> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return false;
        job = it->second;
        jobs_.erase(it);
    }
    if (job) job->Terminate();
    return true;
}

std::vector<wcs::SessionInfo> JobRegistry::List() const {
    std::vector<std::shared_ptr<TerminalJob>> jobs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs.reserve(jobs_.size());
        for (auto& kv : jobs_) jobs.push_back(kv.second);
    }

    std::vector<wcs::SessionInfo> result;
    result.reserve(jobs.size());
    for (auto& job : jobs) {
        if (job) result.push_back(job->Info());
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const wcs::SessionInfo& a, const wcs::SessionInfo& b) {
                         const bool ar = a.state == 0;
                         const bool br = b.state == 0;
                         if (ar != br) return ar;
                         return a.id < b.id;
                     });
    return result;
}

void JobRegistry::MonitorLoop() {
    while (running_) {
        std::vector<std::shared_ptr<TerminalJob>> jobs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs.reserve(jobs_.size());
            for (auto& kv : jobs_) jobs.push_back(kv.second);
        }
        for (auto& job : jobs) {
            if (job) job->Poll();
        }

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(200),
                     [this] { return !running_.load(); });
    }
}
