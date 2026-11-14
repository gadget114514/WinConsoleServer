#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ShellSpec {
    std::wstring command_line;
    std::string display_name;
};

struct ServerConfig {
    std::string listen_addr = "0.0.0.0";
    // Default WCSP port shared by the server, WinConsoleTerminal and wct.
    uint16_t port = 7000;
    std::string shell = "cmd";
    std::string cwd;
    std::string token;
    // Reads the token from this file when `token` is empty.
    std::string token_file;
    std::vector<std::string> allowed_shells = { "cmd", "powershell", "bash" };
    bool allow_arbitrary_shell = false;
    int max_sessions = 8;
    int handshake_timeout_ms = 5000;
    uint32_t max_frame_bytes = 1024u * 1024u;
    uint32_t output_high_water = 768u * 1024u;
    uint32_t output_low_water = 256u * 1024u;
    // Raw ConPTY output retained per job and replayed to a new attachment.
    size_t history_bytes = 32ull * 1024 * 1024;
    bool verbose = false;

    // Windows service support.
    bool run_as_service = false;
    bool install_service = false;
    bool uninstall_service = false;
    std::string service_name = "WinConsoleServer";
    // Optional log file for console and service modes (UTF-8 path).
    std::string log_file;
};

bool ParseServerArgs(int argc, wchar_t** argv, ServerConfig& cfg, std::wstring* error);

// Fills cfg.token from --token, --token-file, or the WCS_TOKEN environment
// variable (in that order) when it is still empty.
bool ResolveServerToken(ServerConfig& cfg, std::wstring* error);
bool ResolveShell(const std::string& requested, const ServerConfig& cfg,
                  ShellSpec* spec, std::wstring* error);
std::string WideToUtf8Server(const std::wstring& text);
std::wstring Utf8ToWideServer(const std::string& text);
