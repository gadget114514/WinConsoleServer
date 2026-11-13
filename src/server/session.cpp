#include "session.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

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

} // namespace

Session::Session(SOCKET socket, const ServerConfig& config, uint32_t id,
                 bool allowed, std::shared_ptr<std::atomic<int>> session_count)
    : socket_(socket), config_(config), id_(id), allowed_(allowed),
      session_count_(std::move(session_count)) {
    if (session_count_) session_count_->fetch_add(1);
}

Session::~Session() {
    if (session_count_) session_count_->fetch_sub(1);
}

void Session::Run() {
    if (!allowed_) {
        SendError(wcs::ErrorCode::SessionLimit, "too many sessions");
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }

    wcs::Frame helloFrame;
    std::string readError;
    if (!ReadFrame(helloFrame, config_.handshake_timeout_ms, &readError)) {
        LogLine("[session %u] handshake read failed: %s", id_, readError.c_str());
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }
    if (helloFrame.type != wcs::FrameType::Hello) {
        SendError(wcs::ErrorCode::InvalidHello, "expected HELLO");
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }

    wcs::Hello hello;
    std::string decodeError;
    if (!wcs::DecodeHello(helloFrame.payload.data(), helloFrame.payload.size(),
                          hello, &decodeError)) {
        SendError(wcs::ErrorCode::InvalidHello, "bad HELLO: " + decodeError);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }

    if (hello.major != wcs::kProtocolMajor) {
        SendError(wcs::ErrorCode::VersionMismatch, "protocol version mismatch");
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }

    if (!config_.token.empty()) {
        if (hello.token.empty()) {
            SendError(wcs::ErrorCode::AuthRequired, "token required");
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return;
        }
        if (hello.token != config_.token) {
            SendError(wcs::ErrorCode::AuthFailed, "invalid token");
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return;
        }
    }

    ShellSpec shell;
    std::wstring shellError;
    if (!ResolveShell(hello.shell, config_, &shell, &shellError)) {
        SendError(wcs::ErrorCode::ShellNotAllowed,
                  "shell not allowed: " + WideToUtf8Server(shellError));
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }

    const uint16_t cols = (uint16_t)std::clamp<int>(hello.cols,
        wcs::kMinColumns, wcs::kMaxDimension);
    const uint16_t rows = (uint16_t)std::clamp<int>(hello.rows,
        wcs::kMinRows, wcs::kMaxDimension);

    wcs::HelloAck ack;
    ack.major = wcs::kProtocolMajor;
    ack.minor = wcs::kProtocolMinor;
    ack.features = hello.features & wcs::kDefaultFeatures;
    ack.cols = cols;
    ack.rows = rows;
    ack.session_id = id_;
    ack.shell = shell.display_name;

    wcs::Frame ackFrame;
    ackFrame.type = wcs::FrameType::HelloAck;
    ackFrame.payload = wcs::EncodeHelloAck(ack);
    if (!SendFrame(ackFrame)) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }

    pty_.SetOutputCallback([this](const char* data, size_t len) {
        wcs::Frame frame;
        frame.type = wcs::FrameType::Output;
        frame.payload.assign((const uint8_t*)data, (const uint8_t*)data + len);
        EnqueueFrame(std::move(frame));
    });

    LogLine("[session %u] starting %s %dx%d", id_, shell.display_name.c_str(), cols, rows);
    if (!pty_.Start(shell.command_line, cols, rows)) {
        SendError(wcs::ErrorCode::ShellStartFailed,
                  WideToUtf8Server(pty_.LastError()));
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }

    writer_ = std::thread(&Session::WriterLoop, this);

    DWORD timeout = 200;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    bool closeRequested = false;
    wcs::ExitReason exitReason = wcs::ExitReason::ChildExited;

    while (!stopping_) {
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
                    exitReason = wcs::ExitReason::Killed;
                    stopping_ = true;
                    break;
                }
                HandleFrame(frame, closeRequested, exitReason);
                if (closeRequested) {
                    stopping_ = true;
                    break;
                }
            }
        } else if (n == 0) {
            exitReason = wcs::ExitReason::ClientClose;
            break;
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT) {
                exitReason = wcs::ExitReason::ClientClose;
                break;
            }
        }

        HANDLE process = pty_.GetProcessHandle();
        if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            exitReason = wcs::ExitReason::ChildExited;
            break;
        }
        if (!pty_.IsRunning()) {
            exitReason = wcs::ExitReason::ChildExited;
            break;
        }
    }

    stopping_ = true;
    pty_.SetOutputPaused(false);

    DWORD exitCode = 0;
    HANDLE process = pty_.GetProcessHandle();
    if (process) {
        DWORD code = 0;
        if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE) {
            exitCode = code;
        }
    }

    // Give the writer a short chance to flush what the child produced.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    pty_.Close();

    wcs::ExitInfo exitInfo;
    exitInfo.reason = exitReason;
    exitInfo.exit_code = (int32_t)exitCode;
    wcs::Frame exitFrame;
    exitFrame.type = wcs::FrameType::Exit;
    exitFrame.payload = wcs::EncodeExit(exitInfo);
    EnqueueFrame(std::move(exitFrame));

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_after_drain_ = true;
    }
    queue_cv_.notify_all();

    if (writer_.joinable()) writer_.join();

    shutdown(socket_, SD_BOTH);
    closesocket(socket_);
    socket_ = INVALID_SOCKET;

    LogLine("[session %u] closed (reason=%u code=%ld)", id_,
            (unsigned)exitInfo.reason, (long)exitInfo.exit_code);
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

void Session::EnqueueFrame(wcs::Frame frame) {
    bool pause = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (frame.type == wcs::FrameType::Output) {
            queue_bytes_ += frame.payload.size();
            if (queue_bytes_ >= config_.output_high_water) pause = true;
        }
        queue_.push_back(std::move(frame));
    }
    if (pause) pty_.SetOutputPaused(true);
    queue_cv_.notify_one();
}

void Session::WriterLoop() {
    for (;;) {
        wcs::Frame frame;
        bool haveFrame = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stopping_ || !queue_.empty() || stop_after_drain_;
            });
            if (!queue_.empty()) {
                frame = std::move(queue_.front());
                queue_.pop_front();
                haveFrame = true;
            } else if (stop_after_drain_) {
                break;
            }
        }

        if (!haveFrame) continue;

        if (!SendFrame(frame)) {
            writer_failed_ = true;
            stopping_ = true;
            shutdown(socket_, SD_BOTH);
            break;
        }

        if (frame.type == wcs::FrameType::Output) {
            bool resume = false;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (queue_bytes_ >= frame.payload.size()) {
                    queue_bytes_ -= frame.payload.size();
                } else {
                    queue_bytes_ = 0;
                }
                if (queue_bytes_ <= config_.output_low_water) resume = true;
            }
            if (resume) pty_.SetOutputPaused(false);
        }
    }
}

void Session::HandleFrame(const wcs::Frame& frame, bool& close_requested,
                          wcs::ExitReason& exit_reason) {
    switch (frame.type) {
    case wcs::FrameType::Input:
        if (!frame.payload.empty()) {
            if (frame.flags & wcs::FlagPriority) {
                pty_.WriteFront(frame.payload.data(), frame.payload.size());
            } else {
                pty_.Write(frame.payload.data(), frame.payload.size());
            }
        }
        break;

    case wcs::FrameType::Resize: {
        wcs::ResizeInfo info;
        if (!wcs::DecodeResize(frame.payload.data(), frame.payload.size(), info)) break;
        const uint16_t cols = (uint16_t)std::clamp<int>(info.cols,
            wcs::kMinColumns, wcs::kMaxDimension);
        const uint16_t rows = (uint16_t)std::clamp<int>(info.rows,
            wcs::kMinRows, wcs::kMaxDimension);
        pty_.Resize(cols, rows);

        wcs::ResizedInfo resized;
        resized.cols = cols;
        resized.rows = rows;
        resized.reason = (cols != info.cols || rows != info.rows)
                             ? wcs::ResizeReason::Clamped
                             : wcs::ResizeReason::Applied;
        wcs::Frame out;
        out.type = wcs::FrameType::Resized;
        out.payload = wcs::EncodeResized(resized);
        EnqueueFrame(std::move(out));
        break;
    }

    case wcs::FrameType::Ping: {
        wcs::Frame pong;
        pong.type = wcs::FrameType::Pong;
        pong.payload = frame.payload;
        EnqueueFrame(std::move(pong));
        break;
    }

    case wcs::FrameType::Close:
        close_requested = true;
        exit_reason = wcs::ExitReason::ClientClose;
        break;

    default:
        break;
    }
}
