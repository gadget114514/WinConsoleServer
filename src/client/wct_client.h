#pragma once

#include <string>

// Options for the wct console client.  It connects to a WCSP endpoint and
// relays the remote ConPTY bytes to the current console unchanged.
struct WctOptions {
    std::string host = "127.0.0.1";
    // Default WCSP port shared by the server, WinConsoleTerminal and wct.
    int port = 7000;
    std::string token;
    // WCSP shell field: "cmd", "powershell", "bash" or "attach:<id>".
    std::string target = "cmd";
};

// Runs wct in the current console and returns a process exit code.
// Ctrl+] detaches (the ConPTY job keeps running for a later `wct <id>`);
// Ctrl+] twice quickly ends the session (WCSP CLOSE terminates the remote job).
int RunWct(const WctOptions& options);

// Prints the retained-session list and returns a process exit code.
int ListWctSessions(const WctOptions& options);
