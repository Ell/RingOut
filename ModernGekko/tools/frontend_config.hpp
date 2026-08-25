#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moderngekko::frontend {
enum class NetplayMode {
  FixedDelay,
  Rollback,
};

std::string_view NetplayModeConfigValue(NetplayMode mode);
bool ParseNetplayMode(std::string_view value, NetplayMode *mode);
// The runtime supplies rollback_production_ready from its authoritative output
// and activation gate. This keeps config/UI policy testable without duplicating
// that safety decision in the frontend layer.
bool IsPlayerUsableNetplayMode(NetplayMode mode,
                               bool rollback_production_ready);

struct ResolutionOption {
  const char *text;
  int dolphin_scale;
};

struct ConfigResult {
  int dolphin_scale = 0;
  std::string resolution;
  std::string controller;
  std::vector<std::string> controllers;
  bool show_fps_in_title = true;
  std::string netplay_nickname = "Player";
  std::string netplay_address = "127.0.0.1";
  std::uint16_t netplay_port = 2626;
  std::string netplay_buffer = "auto";
  NetplayMode netplay_mode = NetplayMode::FixedDelay;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

const std::vector<ResolutionOption> &SupportedResolutions();
ConfigResult LoadConfig(const std::filesystem::path &user_directory,
                        bool create_if_missing);
bool SaveConfig(const std::filesystem::path &user_directory,
                const ConfigResult &config, std::string *error);
bool SaveConfig(const std::filesystem::path &user_directory,
                std::string_view resolution, bool show_fps_in_title,
                std::string_view controller, std::string *error);
std::string
ReadConfiguredController(const std::filesystem::path &user_directory);
std::vector<std::string>
ReadConfiguredControllers(const std::filesystem::path &user_directory);
bool ControllerConfigExists(const std::filesystem::path &user_directory);
// True when the user directory already has a GameCube pad profile.
bool GCPadConfigExists(const std::filesystem::path &user_directory);
// Write a keyboard GCPadNew.ini. This is the default for a GameCube title: the
// existing generator only ever wrote WiimoteNew.ini, a leftover from this
// tree's Wii lineage, so a fresh user directory had no GC pad at all and the
// game was unplayable without hand-writing one. key_set selects between two
// disjoint layouts so two local instances can share one keyboard.
enum class KeyboardLayout { Player1, Player2 };
bool WriteKeyboardGCPadConfig(const std::filesystem::path &user_directory,
                              KeyboardLayout layout, std::string *message);
// Write a GCPadNew.ini bound to an SDL gamepad. `device` is a fully qualified
// Dolphin device name ("SDL/0/<pad name>"); DetectSdlGamepads produces them.
bool WriteGamepadGCPadConfig(const std::filesystem::path &user_directory,
                             std::string_view device, std::string *message);
// Connected SDL gamepads, as Dolphin device names, in Dolphin's own order.
// Empty when there is no pad -- which is the signal to fall back to a keyboard
// profile. Enumerated rather than hardcoded: the Steam Deck's pad reaches us
// through Steam Input as a virtual X-Box 360 controller whose SDL name is NOT
// its evdev name, so any string written from memory is a guess.
std::vector<std::string> DetectSdlGamepads();
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::span<const std::string> controllers,
                              std::string *message);
bool GenerateControllerConfig(const std::filesystem::path &user_directory,
                              std::string_view controller,
                              std::string *message);
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::span<const std::string> controllers,
                            std::string *message);
bool EnsureControllerConfig(const std::filesystem::path &user_directory,
                            std::string_view controller, std::string *message);
} // namespace moderngekko::frontend
