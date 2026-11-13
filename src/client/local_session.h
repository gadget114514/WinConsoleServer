#pragma once

#include "iterminal_io.h"
#include "../common/conpty_session.h"

#include <mutex>
#include <string>

// Local ConPTY backend, used for smoke testing and for Ecode-style local use.
class LocalSession : public ITerminalIo {
public:
    LocalSession();
    ~LocalSession() override;

    void SetOutputSink(OutputFn fn) override;
    bool Start(const TerminalStartOptions& opt, std::wstring* error) override;

    void Write(const void* data, size_t len) override;
    void WriteFront(const void* data, size_t len) override;
    void Resize(int cols, int rows) override;

    bool IsRunning() const override;
    void Close() override;
    std::wstring LastError() const override;

    HANDLE GetProcessHandle() const { return session_.GetProcessHandle(); }

private:
    mutable std::mutex sink_mutex_;
    ConPtySession session_;
    OutputFn sink_;
    std::wstring last_error_;
};
