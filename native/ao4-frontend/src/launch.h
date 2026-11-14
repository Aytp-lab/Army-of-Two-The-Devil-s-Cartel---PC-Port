// Starts the game (bin\ao4_launcher.exe) for a mode with the environment and options of the
// tested starters (single player = normal player starter, split screen = S1 starter, online/LAN
// = V4 LAN starter incl. the mini Blaze server on the host). No diagnostics are enabled.
#pragma once

#include "config.h"

#include <windows.h>

#include <string>
#include <vector>

namespace ao4launch {

struct Session {
  HANDLE game = nullptr;
  HANDLE server = nullptr;  // Blaze server (online host only)
  std::wstring log;
  bool Running() const;
  void Close();  // after the game exited: stop the server, release handles
};

// Builds and starts the session. Returns false with an error text.
bool Start(const ao4cfg::Paths& paths, const ao4cfg::LauncherSettings& settings, const ao4cfg::ProfileStore& profiles,
           Session& session, std::wstring& error);

// IPv4 addresses of this PC usable for LAN/VPN co-op (no loopback / link-local).
std::vector<std::string> LocalAddresses();

// The arguments a start would use (for the help page / bug reports).
std::vector<std::wstring> BuildArguments(const ao4cfg::Paths& paths, const ao4cfg::LauncherSettings& settings,
                                         const ao4cfg::ProfileStore& profiles, const std::wstring& log);

}  // namespace ao4launch
