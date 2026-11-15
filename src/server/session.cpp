#include "session.h"

#include "server_config.h"
#include "session_hub.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t kMaxQueueBytes = 8 * 1024 * 1024;
constexpr size_t kChunkBytes = 256 * 1024;
constexpr int kPollTimeoutMs = 200;

std::mutex g_log_mutex;

void LogLine(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

bool SendAll(SOCKET socket, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(socket, (const char*)data + sent, (int)(len - sent), 0);
        if (n == SOCKET_ERROR || n == 0) return false;
        sent += (size_t)n;
    }
    return true;
}

std::string ToLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool ParsePrefixedId(const std::string& shell, const char* prefix, int& id) {
    const std::string head(prefix);
    if (shell.compare(0, head.size(), head) != 0) return false;
    std::string num = shell.substr(head.size());
    if (num.empty()) return false;
    char* end = nullptr;
    long value = std::strtol(num.c_str(), &end, 10);
    if (!end || *end != '\0' || value <= 0) return false;
    id = static_cast<int>(value);
    return true;
}

bool ParseAttachId(const std::string& shell, int& id) {
    return ParsePrefixedId(shell, "attach:", id);
}

} // namespace

Session::Session(SOCKET socket, const ServerConfig& config, uint32_t id,
                 bool allowed, std::shared_ptr<std::atomic<int>> session_count,
                 std::shared_ptr<JobRegistry> jobs,
                 std::shared_ptr<std::atomic<bool>> server_stopping,
                 std::shared_ptr<SessionHub> hub)
    : socket_(socket), config_(config), id_(id), allowed_(allowed),
      session_count_(std::move(session_count)), jobs_(std::move(jobs)),
      server_stopping_(std::move(server_stopping)), hub_(std::move(hub)) {
    if (session_count_) session_count_->fetch_add(1);
}

Session::~Session() {
    JoinWriter();
    CloseSocket();
    if (session_count_) session_count_->fetch_sub(1);
}

void Session::CloseSocket() {
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

void Session::ShutdownSocket() {
    if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH);
}

bool Session::ReadFrame(wcs::Frame& frame, int timeout_ms, std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    std::vector<uint8_t> buf(8192);

    for (;;) {
        std::string parseError;
        wcs::ReadResult rr = reader_.Next(frame, &parseError);
        if (rr == wcs::ReadResult::Frame) return true;
        if (rr == wcs::ReadResult::Error) {
            if (error) *error = parseError;
            return false;
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            if (error) *error = "timeout";
            return false;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket_, &readSet);
        timeval tv{};
        tv.tv_sec = (long)(remaining / 1000);
        tv.tv_usec = (long)((remaining % 1000) * 1000);
        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel == 0) {
            if (error) *error = "timeout";
            return false;
        }
        if (sel == SOCKET_ERROR) {
            if (error) *error = "select failed";
            return false;
        }

        int n = recv(socket_, (char*)buf.data(), (int)buf.size(), 0);
        if (n <= 0) {
            if (error) *error = "connection closed";
            return false;
        }
        reader_.Append(buf.data(), (size_t)n);
    }
}

bool Session::SendFrame(const wcs::Frame& frame) {
    std::vector<uint8_t> bytes = wcs::EncodeFrame(frame);
    return SendAll(socket_, bytes.data(), bytes.size());
}

bool Session::SendError(wcs::ErrorCode code, const std::string& message) {
    wcs::ErrorInfo info;
    info.code = code;
    info.message = message;
    wcs::Frame frame;
    frame.type = wcs::FrameType::Error;
    frame.payload = wcs::EncodeError(info);
    return SendFrame(frame);
}

void Session::OnJobOutput(const char* data, size_t len) {
    if (!data || len == 0 || stopping_) return;

    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (replay_pending_) {
            pending_live_.insert(pending_live_.end(),
                                 reinterpret_cast<const uint8_t*>(data),
                                 reinterpret_cast<const uint8_t*>(data) + len);
        } else {
            for (size_t offset = 0; offset < len; offset += kChunkBytes) {
                const size_t chunk = (std::min)(kChunkBytes, len - offset);
                wcs::Frame frame;
                frame.type = wcs::FrameType::Output;
                frame.payload.assign(
                    reinterpret_cast<const uint8_t*>(data) + offset,
                    reinterpret_cast<const uint8_t*>(data) + offset + chunk);
                queue_bytes_ += chunk;
                queue_.push_back(std::move(frame));
            }
        }
        if (queue_bytes_ + pending_live_.size() > queue_limit_) overflow = true;
    }
    queue_cv_.notify_one();
    if (overflow) {
        // A client that cannot keep up is detached rather than blocking the
        // ConPTY reader thread (which holds the job output mutex).
        stopping_ = true;
        writer_abort_ = true;
        ShutdownSocket();
        queue_cv_.notify_all();
    }
}

void Session::QueueReplay(const std::string& replay) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    auto pushOutput = [this](const uint8_t* data, size_t len) {
        for (size_t offset = 0; offset < len; offset += kChunkBytes) {
            const size_t chunk = (std::min)(kChunkBytes, len - offset);
            wcs::Frame frame;
            frame.type = wcs::FrameType::Output;
            frame.payload.assign(data + offset, data + offset + chunk);
            queue_bytes_ += chunk;
            queue_.push_back(std::move(frame));
        }
    };
    if (!replay.empty()) {
        pushOutput(reinterpret_cast<const uint8_t*>(replay.data()), replay.size());
    }
    if (!pending_live_.empty()) {
        pushOutput(pending_live_.data(), pending_live_.size());
        pending_live_.clear();
    }
    if (queue_bytes_ + kMaxQueueBytes > queue_limit_) {
        queue_limit_ = queue_bytes_ + kMaxQueueBytes;
    }
    replay_pending_ = false;
}

void Session::NotifyPeerResize(uint16_t cols, uint16_t rows) {
    if (!size_sync_.load()) return;

    wcs::ResizedInfo info;
    info.cols = cols;
    info.rows = rows;
    info.reason = wcs::ResizeReason::Peer;
    wcs::Frame out;
    out.type = wcs::FrameType::Resized;
    out.payload = wcs::EncodeResized(info);
    EnqueueFrame(std::move(out), true);
}

void Session::EnqueueFrame(wcs::Frame frame, bool priority) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (frame.type == wcs::FrameType::Output) {
            queue_bytes_ += frame.payload.size();
        }
        if (priority) {
            queue_.push_front(std::move(frame));
        } else {
            queue_.push_back(std::move(frame));
        }
    }
    queue_cv_.notify_one();
}

void Session::StartWriter() {
    writer_started_ = true;
    writer_ = std::thread(&Session::WriterLoop, this);
}

void Session::JoinWriter() {
    if (writer_started_ && writer_.joinable()) {
        writer_.join();
        writer_started_ = false;
    }
}

void Session::WriterLoop() {
    for (;;) {
        wcs::Frame frame;
        bool haveFrame = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return writer_abort_ || stop_after_drain_ || !queue_.empty();
            });
            if (!queue_.empty()) {
                frame = std::move(queue_.front());
                queue_.pop_front();
                haveFrame = true;
            } else if (stop_after_drain_ || writer_abort_) {
                break;
            }
        }

        if (!haveFrame) continue;

        if (!SendFrame(frame)) {
            stopping_ = true;
            writer_abort_ = true;
            ShutdownSocket();
            break;
        }

        if (frame.type == wcs::FrameType::Output) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_bytes_ >= frame.payload.size()) {
                queue_bytes_ -= frame.payload.size();
            } else {
                queue_bytes_ = 0;
            }
            if (queue_bytes_ < kMaxQueueBytes) queue_limit_ = kMaxQueueBytes;
        }
    }
}

bool Session::HandleFrame(const wcs::Frame& frame, wcs::ExitReason& exit_reason) {
    switch (frame.type) {
    case wcs::FrameType::Input:
        if (job_ && !frame.payload.empty()) {
            job_->Write(frame.payload.data(), frame.payload.size(),
                        (frame.flags & wcs::FlagPriority) != 0);
        }
        return false;

    case wcs::FrameType::Resize: {
        if (!job_) return false;
        wcs::ResizeInfo info;
        if (!wcs::DecodeResize(frame.payload.data(), frame.payload.size(), info)) {
            return false;
        }
        int cols = std::clamp<int>(info.cols, wcs::kMinColumns, wcs::kMaxDimension);
        int rows = std::clamp<int>(info.rows, wcs::kMinRows, wcs::kMaxDimension);
        const bool resized_ok = job_->Resize(cols, rows);

        wcs::ResizedInfo resized;
        resized.cols = static_cast<uint16_t>(cols);
        resized.rows = static_cast<uint16_t>(rows);
        resized.reason = (cols != (int)info.cols || rows != (int)info.rows)
                             ? wcs::ResizeReason::Clamped
                             : wcs::ResizeReason::Applied;
        wcs::Frame out;
        out.type = wcs::FrameType::Resized;
        out.payload = wcs::EncodeResized(resized);
        EnqueueFrame(std::move(out), true);

        // Tell every other SizeSync client attached to the same job that the
        // shared PTY size changed.
        if (resized_ok && hub_) {
            hub_->NotifyPeers(job_id_.load(), id_,
                              static_cast<uint16_t>(cols),
                              static_cast<uint16_t>(rows));
        }
        return false;
    }

    case wcs::FrameType::Ping: {
        wcs::Frame pong;
        pong.type = wcs::FrameType::Pong;
        pong.payload = frame.payload;
        EnqueueFrame(std::move(pong), true);
        return false;
    }

    case wcs::FrameType::Close:
        terminate_requested_ = true;
        exit_reason = wcs::ExitReason::ClientClose;
        return true;

    default:
        return false;
    }
}

void Session::Finish(wcs::ExitReason reason, int exit_code, bool ended) {
    // A dropped connection is a detach: the ConPTY stays alive so a later
    // client can list it and reconnect. An explicit CLOSE frame terminates it
    // and removes it from the retained-session list.
    if (job_ && terminate_requested_) {
        if (!ended) job_->Terminate();
        jobs_->Remove(job_->id());
    }
    if (job_ && sub_id_ > 0) {
        job_->RemoveSubscriber(sub_id_);
        sub_id_ = 0;
    }

    if (writer_started_) {
        wcs::ExitInfo info;
        info.reason = reason;
        info.exit_code = static_cast<int32_t>(exit_code);
        wcs::Frame frame;
        frame.type = wcs::FrameType::Exit;
        frame.payload = wcs::EncodeExit(info);
        EnqueueFrame(std::move(frame), false);
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_after_drain_ = true;
        }
        queue_cv_.notify_all();
        JoinWriter();
    }
    ShutdownSocket();
}

void Session::Run() {
    if (!allowed_) {
        SendError(wcs::ErrorCode::SessionLimit, "too many sessions");
        CloseSocket();
        return;
    }

    wcs::Frame helloFrame;
    std::string readError;
    if (!ReadFrame(helloFrame, config_.handshake_timeout_ms, &readError)) {
        LogLine("[session %u] handshake read failed: %s", id_, readError.c_str());
        CloseSocket();
        return;
    }
    if (helloFrame.type != wcs::FrameType::Hello) {
        SendError(wcs::ErrorCode::InvalidHello, "expected HELLO");
        CloseSocket();
        return;
    }

    wcs::Hello hello;
    std::string decodeError;
    if (!wcs::DecodeHello(helloFrame.payload.data(), helloFrame.payload.size(),
                          hello, &decodeError)) {
        SendError(wcs::ErrorCode::InvalidHello, "bad HELLO: " + decodeError);
        CloseSocket();
        return;
    }
    if (hello.major != wcs::kProtocolMajor) {
        SendError(wcs::ErrorCode::VersionMismatch, "protocol version mismatch");
        CloseSocket();
        return;
    }

    if (!config_.token.empty()) {
        if (hello.token.empty()) {
            SendError(wcs::ErrorCode::AuthRequired, "token required");
            CloseSocket();
            return;
        }
        if (hello.token != config_.token) {
            SendError(wcs::ErrorCode::AuthFailed, "invalid token");
            CloseSocket();
            return;
        }
    }

    const uint16_t cols = (uint16_t)std::clamp<int>(hello.cols,
        wcs::kMinColumns, wcs::kMaxDimension);
    const uint16_t rows = (uint16_t)std::clamp<int>(hello.rows,
        wcs::kMinRows, wcs::kMaxDimension);

    // FeatureSizeSync is active only when both sides advertise it: HELLO_ACK
    // returns hello.features & kDefaultFeatures.
    size_sync_ = (hello.features & wcs::kDefaultFeatures &
                  wcs::FeatureSizeSync) != 0;

    const std::string requested = hello.shell.empty() ? config_.shell : hello.shell;
    const std::string lower = ToLower(requested);

    if (lower == "list" || lower == "sessions") {
        wcs::HelloAck ack;
        ack.major = wcs::kProtocolMajor;
        ack.minor = wcs::kProtocolMinor;
        ack.features = hello.features & wcs::kDefaultFeatures;
        ack.cols = cols;
        ack.rows = rows;
        ack.session_id = id_;
        ack.shell = "sessions";
        wcs::Frame ackFrame;
        ackFrame.type = wcs::FrameType::HelloAck;
        ackFrame.payload = wcs::EncodeHelloAck(ack);
        if (SendFrame(ackFrame)) RunControlSession();
        CloseSocket();
        return;
    }

    int deleteId = 0;
    if (lower.rfind("delete:", 0) == 0) {
        if (!ParsePrefixedId(requested, "delete:", deleteId) ||
            !jobs_->Remove(deleteId)) {
            SendError(wcs::ErrorCode::ShellNotAllowed,
                      "session not found: " + std::to_string(deleteId));
            CloseSocket();
            return;
        }

        wcs::HelloAck ack;
        ack.major = wcs::kProtocolMajor;
        ack.minor = wcs::kProtocolMinor;
        ack.features = hello.features & wcs::kDefaultFeatures;
        ack.cols = cols;
        ack.rows = rows;
        ack.session_id = id_;
        ack.shell = "deleted";
        wcs::Frame ackFrame;
        ackFrame.type = wcs::FrameType::HelloAck;
        ackFrame.payload = wcs::EncodeHelloAck(ack);
        if (!SendFrame(ackFrame)) {
            CloseSocket();
            return;
        }

        wcs::ExitInfo info;
        info.reason = wcs::ExitReason::Killed;
        info.exit_code = 0;
        wcs::Frame exitFrame;
        exitFrame.type = wcs::FrameType::Exit;
        exitFrame.payload = wcs::EncodeExit(info);
        SendFrame(exitFrame);
        CloseSocket();
        return;
    }

    std::string displayName;
    int attachId = 0;
    const bool attach = ParseAttachId(requested, attachId);
    if (attach) {
        job_ = jobs_->Find(attachId);
        if (!job_) {
            SendError(wcs::ErrorCode::ShellNotAllowed,
                      "session not found: " + std::to_string(attachId));
            CloseSocket();
            return;
        }
        displayName = job_->Info().name;
        job_id_ = job_->id();
    } else {
        ShellSpec shell;
        std::wstring shellError;
        if (!ResolveShell(requested, config_, &shell, &shellError)) {
            SendError(wcs::ErrorCode::ShellNotAllowed,
                      WideToUtf8Server(shellError));
            CloseSocket();
            return;
        }

        std::wstring startError;
        job_ = jobs_->Create("wcsp-" + shell.display_name,
                             WideToUtf8Server(shell.command_line),
                             cols, rows, &startError);
        if (!job_) {
            SendError(wcs::ErrorCode::ShellStartFailed,
                      "failed to start shell: " + WideToUtf8Server(startError));
            CloseSocket();
            return;
        }
        spawned_ = true;
        displayName = shell.display_name;
        job_id_ = job_->id();
    }

    // Resize the target job to the client's size before taking the replay.
    // Other clients already attached to the same job learn about it when they
    // negotiated FeatureSizeSync.
    if (job_->Resize(cols, rows) && hub_) {
        hub_->NotifyPeers(job_id_.load(), id_, cols, rows);
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        replay_pending_ = true;
    }
    std::string replay;
    sub_id_ = job_->AddSubscriber(
        [this](const char* data, size_t len) { OnJobOutput(data, len); }, replay);
    if (sub_id_ <= 0) {
        if (spawned_) job_->Terminate();
        SendError(wcs::ErrorCode::ShellNotAllowed, "could not subscribe to session");
        CloseSocket();
        return;
    }

    const bool ended = !job_->IsRunning();

    wcs::HelloAck ack;
    ack.major = wcs::kProtocolMajor;
    ack.minor = wcs::kProtocolMinor;
    ack.features = hello.features & wcs::kDefaultFeatures;
    ack.cols = cols;
    ack.rows = rows;
    ack.session_id = id_;
    ack.job_id = static_cast<uint32_t>(job_id_.load());
    ack.shell = displayName;
    wcs::Frame ackFrame;
    ackFrame.type = wcs::FrameType::HelloAck;
    ackFrame.payload = wcs::EncodeHelloAck(ack);
    if (!SendFrame(ackFrame)) {
        job_->RemoveSubscriber(sub_id_);
        sub_id_ = 0;
        if (spawned_) job_->Terminate();
        CloseSocket();
        return;
    }

    QueueReplay(replay);
    StartWriter();

    if (ended) {
        Finish(wcs::ExitReason::ChildExited, job_->ExitCode(), true);
        CloseSocket();
        return;
    }

    DWORD timeout = kPollTimeoutMs;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    DWORD sendTimeout = 5000;
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeout, sizeof(sendTimeout));

    wcs::ExitReason exitReason = wcs::ExitReason::ChildExited;
    bool clientClosed = false;
    int exitCode = 0;
    bool jobEnded = false;

    while (!stopping_) {
        if (server_stopping_ && server_stopping_->load()) {
            exitReason = wcs::ExitReason::ServerShutdown;
            clientClosed = true;
            break;
        }

        char buf[16384];
        int n = recv(socket_, buf, (int)sizeof(buf), 0);
        if (n > 0) {
            reader_.Append((const uint8_t*)buf, (size_t)n);
            for (;;) {
                wcs::Frame frame;
                std::string parseError;
                wcs::ReadResult rr = reader_.Next(frame, &parseError);
                if (rr == wcs::ReadResult::NeedMore) break;
                if (rr == wcs::ReadResult::Error) {
                    LogLine("[session %u] protocol error: %s", id_, parseError.c_str());
                    clientClosed = true;
                    exitReason = wcs::ExitReason::Killed;
                    stopping_ = true;
                    break;
                }
                if (HandleFrame(frame, exitReason)) {
                    clientClosed = true;
                    stopping_ = true;
                    break;
                }
            }
        } else if (n == 0) {
            clientClosed = true;
            exitReason = wcs::ExitReason::ClientClose;
            break;
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT) {
                clientClosed = true;
                exitReason = wcs::ExitReason::ClientClose;
                break;
            }
        }

        if (job_ && !job_->IsRunning()) {
            jobEnded = true;
            exitCode = job_->ExitCode();
            break;
        }
    }

    if (!jobEnded && job_ && !job_->IsRunning()) {
        jobEnded = true;
        exitCode = job_->ExitCode();
    }

    if (jobEnded) {
        exitReason = wcs::ExitReason::ChildExited;
    } else if (clientClosed && job_ && terminate_requested_) {
        exitReason = wcs::ExitReason::ClientClose;
    } else if (clientClosed && server_stopping_ && server_stopping_->load()) {
        exitReason = wcs::ExitReason::ServerShutdown;
    } else if (clientClosed) {
        exitReason = wcs::ExitReason::ClientClose;
    }

    stopping_ = true;
    Finish(exitReason, exitCode, jobEnded);
    CloseSocket();
}

void Session::RunControlSession() {
    DWORD timeout = kPollTimeoutMs;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    DWORD sendTimeout = 5000;
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeout, sizeof(sendTimeout));

    while (!stopping_) {
        if (server_stopping_ && server_stopping_->load()) return;

        char buf[16384];
        int n = recv(socket_, buf, (int)sizeof(buf), 0);
        if (n > 0) {
            reader_.Append((const uint8_t*)buf, (size_t)n);
            for (;;) {
                wcs::Frame frame;
                std::string parseError;
                wcs::ReadResult rr = reader_.Next(frame, &parseError);
                if (rr == wcs::ReadResult::NeedMore) break;
                if (rr == wcs::ReadResult::Error) return;

                if (frame.type == wcs::FrameType::ListSessions) {
                    wcs::Frame out;
                    out.type = wcs::FrameType::SessionList;
                    out.payload = wcs::EncodeSessionList(jobs_->List());
                    if (!SendFrame(out)) return;
                } else if (frame.type == wcs::FrameType::Ping) {
                    wcs::Frame pong;
                    pong.type = wcs::FrameType::Pong;
                    pong.payload = frame.payload;
                    if (!SendFrame(pong)) return;
                } else if (frame.type == wcs::FrameType::Close) {
                    return;
                }
            }
        } else if (n == 0) {
            return;
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT) return;
        }
    }
}
