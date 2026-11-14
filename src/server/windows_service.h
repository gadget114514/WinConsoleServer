#pragma once

#include "server_config.h"

#include <string>

// Redirects stdout/stderr to `log_file` (append). The parent directory is
// created when needed. Returns false and fills `error` on failure.
bool RedirectServerOutput(const std::string& log_file, std::wstring* error);

// Runs the server under the Service Control Manager. `config.service_name`
// must match the installed service name.
int RunAsService(const ServerConfig& config);

// Installs the service with the given configuration and tries to start it.
// When neither --token nor --token-file is supplied, a random token is
// generated in %ProgramData%\WinConsoleServer\token.txt and
// `token_file_out` receives that path.
bool InstallWindowsService(const ServerConfig& config, std::wstring* error,
                           bool* started, std::wstring* token_file_out);

// Stops (best effort) and removes the service.
bool UninstallWindowsService(const std::string& service_name,
                             std::wstring* error);
