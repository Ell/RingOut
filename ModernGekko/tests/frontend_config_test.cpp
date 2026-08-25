#include "frontend_config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>

int main() {
  namespace fs = std::filesystem;
  const fs::path directory =
      fs::temp_directory_path() /
      ("moderngekko-frontend-config-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  std::string error;
  const std::string controller = "SDL/0/Test Controller";
  if (!moderngekko::frontend::SaveConfig(directory, "1920x1080", false,
                                         controller, &error))
    return 1;

  const auto loaded = moderngekko::frontend::LoadConfig(directory, false);
  if (!loaded || loaded.dolphin_scale != 3 || loaded.show_fps_in_title ||
      loaded.controller != controller) {
    return 2;
  }

  moderngekko::frontend::ConfigResult netplay_config = loaded;
  netplay_config.controllers = {controller, "SDL/1/Second Controller"};
  netplay_config.controller = controller;
  netplay_config.netplay_nickname = "Kirby";
  netplay_config.netplay_address = "192.168.1.50";
  netplay_config.netplay_port = 34567;
  netplay_config.netplay_buffer = "auto";
  netplay_config.netplay_mode =
      moderngekko::frontend::NetplayMode::Rollback;
  if (!moderngekko::frontend::SaveConfig(directory, netplay_config, &error))
    return 6;
  const auto netplay_loaded =
      moderngekko::frontend::LoadConfig(directory, false);
  if (!netplay_loaded ||
      netplay_loaded.controllers != netplay_config.controllers ||
      netplay_loaded.netplay_nickname != "Kirby" ||
      netplay_loaded.netplay_address != "192.168.1.50" ||
      netplay_loaded.netplay_port != 34567 ||
      netplay_loaded.netplay_buffer != "auto" ||
      netplay_loaded.netplay_mode !=
          moderngekko::frontend::NetplayMode::Rollback) {
    return 7;
  }

  if (moderngekko::frontend::NetplayModeConfigValue(
      moderngekko::frontend::NetplayMode::Rollback) != "rollback" ||
      moderngekko::frontend::IsPlayerUsableNetplayMode(
          moderngekko::frontend::NetplayMode::Rollback, false) ||
      !moderngekko::frontend::IsPlayerUsableNetplayMode(
          moderngekko::frontend::NetplayMode::Rollback, true)) {
    return 19;
  }
  moderngekko::frontend::NetplayMode parsed_mode{};
  if (!moderngekko::frontend::ParseNetplayMode("rollback", &parsed_mode) ||
      parsed_mode != moderngekko::frontend::NetplayMode::Rollback ||
      moderngekko::frontend::ParseNetplayMode("silent-downgrade",
                                              &parsed_mode)) {
    return 20;
  }
  const auto normalized_room =
      moderngekko::frontend::NormalizeNetplayRoomCode("  A1b2C3d4\n");
  if (!normalized_room || *normalized_room != "a1b2c3d4" ||
      moderngekko::frontend::NormalizeNetplayRoomCode("a1b2c3d") ||
      moderngekko::frontend::NormalizeNetplayRoomCode("a1b2c3dz")) {
    return 21;
  }

  auto invalid_netplay = netplay_config;
  invalid_netplay.netplay_address = "not a host";
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 8;
  invalid_netplay = netplay_config;
  invalid_netplay.netplay_nickname = std::string(31, 'K');
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 9;
  if (!moderngekko::frontend::GenerateControllerConfig(
          directory, netplay_config.controllers, &error))
    return 3;
  if (moderngekko::frontend::ReadConfiguredController(directory) != controller)
    return 4;
  if (moderngekko::frontend::ReadConfiguredControllers(directory) !=
      netplay_config.controllers)
    return 10;

  std::ifstream input(directory / "Config" / "WiimoteNew.ini");
  const std::string generated{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
  if (!generated.contains("Buttons/A = `Shoulder L`\n") ||
      !generated.contains("Buttons/1 = `Button W`\n") ||
      !generated.contains("Buttons/2 = `Button S`\n") ||
      !generated.contains("Shake/X = `Trigger L`\n") ||
      !generated.contains("Extension = None\n") ||
      !generated.contains("Options/Sideways Wiimote = True\n") ||
      !generated.contains("[Wiimote2]\nDevice = SDL/1/Second Controller\n") ||
      generated.contains("Nunchuk/")) {
    return 5;
  }

  const std::string custom =
      "[Wiimote1]\nDevice = SDL/9/Custom Controller\nButtons/1 = Custom\n";
  {
    std::ofstream output(directory / "Config" / "WiimoteNew.ini",
                         std::ios::trunc);
    output << custom;
  }
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, netplay_config.controllers, &error))
    return 11;
  std::ifstream custom_input(directory / "Config" / "WiimoteNew.ini");
  const std::string preserved{std::istreambuf_iterator<char>(custom_input),
                              std::istreambuf_iterator<char>()};
  if (preserved != custom || moderngekko::frontend::ReadConfiguredController(
                                 directory) != "SDL/9/Custom Controller")
    return 12;

  // A named pad drives the GC pad profile, not just the Wiimote one. This is
  // the Steam Deck case: before it, EnsureControllerConfig wrote a keyboard
  // profile unconditionally, so a machine with a pad and no keyboard had no
  // usable input at all.
  const fs::path pad_directory = directory / "pad";
  const std::string pad = "SDL/0/Test Gamepad";
  if (!moderngekko::frontend::EnsureControllerConfig(pad_directory, pad,
                                                     &error))
    return 13;
  std::ifstream pad_input(pad_directory / "Config" / "GCPadNew.ini");
  const std::string pad_config{std::istreambuf_iterator<char>(pad_input),
                               std::istreambuf_iterator<char>()};
  if (!pad_config.contains("Device = SDL/0/Test Gamepad\n") ||
      !pad_config.contains("Buttons/A = `Button S`\n") ||
      !pad_config.contains("Main Stick/Up = `Left Y+`\n") ||
      !pad_config.contains("C-Stick/Calibration = ") ||
      pad_config.contains("XInput2")) {
    return 14;
  }

  // No pad named and (in CI) none attached: the keyboard profile is still the
  // fallback, so a desktop with no hardware keeps booting into a playable game.
  if (moderngekko::frontend::DetectSdlGamepads().empty()) {
    const fs::path keyboard_directory = directory / "keyboard";
    if (!moderngekko::frontend::EnsureControllerConfig(
            keyboard_directory, std::span<const std::string>{}, &error))
      return 15;
    std::ifstream keyboard_input(keyboard_directory / "Config" /
                                 "GCPadNew.ini");
    const std::string keyboard_config{
        std::istreambuf_iterator<char>(keyboard_input),
        std::istreambuf_iterator<char>()};
    if (!keyboard_config.contains("XInput2") ||
        keyboard_config.contains("SDL/"))
      return 16;
  } else {
    // The same call on a machine that DOES have a pad, which is the path that
    // segfaulted on the Steam Deck: nothing named, so the span was repointed at
    // a vector of detected pads that then went out of scope, and the Wiimote
    // profile written afterwards read it back. CI has no pad and can never
    // reach this, so it only ever fires on real hardware -- which is precisely
    // where the bug lived.
    const fs::path detected_directory = directory / "detected";
    if (!moderngekko::frontend::EnsureControllerConfig(
            detected_directory, std::span<const std::string>{}, &error))
      return 17;
    std::ifstream detected_input(detected_directory / "Config" /
                                 "GCPadNew.ini");
    const std::string detected_config{
        std::istreambuf_iterator<char>(detected_input),
        std::istreambuf_iterator<char>()};
    if (!detected_config.contains("SDL/") ||
        !fs::is_regular_file(detected_directory / "Config" / "WiimoteNew.ini"))
      return 18;
  }

  fs::remove_all(directory);
  return 0;
}
