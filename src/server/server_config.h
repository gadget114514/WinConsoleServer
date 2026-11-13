#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ShellSpec {
    std::wstring command_line;
    std::string display_name;
};

struct ServerConfig {
    std::string listen_addr = "0.0.0.0";
    uint16_t port = 7000;
    std::string shell = "cmd";
    std::string cwd;
    std::string token;
    std::vector<std::string> allowed_shells = { "cmd", "powershell", "bash" };
    bool allow_arbitrary_shell = false;
    int max_sessions = 8;
    int handshake_timeout_ms = 5000;
    uint32_t max_frame_bytes = 1024u * 1024u;
    uint32_t output_high_water = 768u * 1024u;
    uint32_t output_low_water = 256u * 1024u;
    bool verbose = false;
};

bool ParseServerArgs(int argc, wchar_t** argv, ServerConfig& cfg, std::wstring* error);
bool ResolveShell(const std::string& requested, const ServerConfig& cfg,
                  ShellSpec* spec, std::wstring* error);
std::string WideToUtf8Server(const std::wstring& text);
std::wstring Utf8ToWideServer(const std::string& text);
