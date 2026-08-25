#pragma once

#include "frontend_config.hpp"
#include "moderngekko/runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace moderngekko::frontend {
enum class NetplayRole {
  Host,
  Join,
};

enum class NetplayExitCode {
  Failed = 1,
  InvalidConfiguration = 2,
  HostUnavailable = 10,
  VersionMismatch = 11,
  CompatibilityMismatch = 12,
  RoomFull = 13,
  GameRunning = 14,
  ServerFull = 15,
  NicknameRejected = 16,
  RollbackUnavailable = 20,
  RollbackStartRejected = 21,
  SessionDesynced = 22,
  SessionRuntimeFailed = 23,
  SessionConnectionLost = 24,
};

struct NetplayOptions {
  NetplayRole role = NetplayRole::Join;
  NetplayMode mode = NetplayMode::FixedDelay;
  std::string address = "127.0.0.1";
  std::uint16_t port = 2626;
  std::string nickname = "Player";
  std::string buffer = "auto";
  // Resolved before the lobby opens. For player rollback, `buffer=auto` maps
  // to two SI samples; a manual buffer value becomes the rollback base delay.
  unsigned rollback_base_delay_samples = 2;
  unsigned rollback_horizon_frames = 8;
  std::vector<std::string> controllers;
  // Host only: exact room size. The host still requires every mapped player to
  // become Ready before starting; headless peers do so automatically.
  unsigned players = 2;
  // Seconds to wait for peers and for the start signal. A headless lobby must
  // not block forever; a hung run looks exactly like a slow one to a script.
  unsigned lobby_timeout = 120;
};

int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options);
} // namespace moderngekko::frontend
