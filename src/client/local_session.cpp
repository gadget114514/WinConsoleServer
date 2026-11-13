#include "local_session.h"

LocalSession::LocalSession() = default;

LocalSession::~LocalSession() { Close(); }

void LocalSession::SetOutputSink(OutputFn fn) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink_ = std::move(fn);
}

bool LocalSession::Start(const TerminalStartOptions& opt, std::wstring* error) {
    session_.SetCodePage(opt.codePage);
    session_.SetOutputCallback([this](const char* data, size_t len) {
        OutputFn sink;
        {
            std::lock_guard<std::mutex> lock(sink_mutex_);
            sink = sink_;
        }
        if (sink) sink(data, len);
    });

    const int cols = opt.cols > 0 ? opt.cols : 80;
    const int rows = opt.rows > 0 ? opt.rows : 24;
    if (!session_.Start(opt.shell, cols, rows, opt.env)) {
        last_error_ = session_.LastError();
        if (error) *error = last_error_;
        return false;
    }
    return true;
}

void LocalSession::Write(const void* data, size_t len) {
    session_.Write(data, len);
}

void LocalSession::WriteFront(const void* data, size_t len) {
    session_.WriteFront(data, len);
}

void LocalSession::Resize(int cols, int rows) {
    session_.Resize(cols, rows);
}

bool LocalSession::IsRunning() const {
    return session_.IsRunning();
}

void LocalSession::Close() {
    session_.Close();
}

std::wstring LocalSession::LastError() const {
    return last_error_;
}
