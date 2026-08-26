#include "frontend_config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>

#ifndef MODERNGEKKO_NO_SDL_GAMEPADS
#include <SDL3/SDL.h>
#endif

namespace fs = std::filesystem;

namespace moderngekko::frontend {
std::string_view NetplayModeConfigValue(NetplayMode mode) {
  switch (mode) {
  case NetplayMode::FixedDelay:
    return "fixed-delay";
  case NetplayMode::Rollback:
    return "rollback";
  }
  return "fixed-delay";
}

bool ParseNetplayMode(std::string_view value, NetplayMode *mode) {
  if (mode == nullptr)
    return false;
  if (value == "fixed-delay" || value == "fixeddelay") {
    *mode = NetplayMode::FixedDelay;
    return true;
  }
  if (value == "rollback") {
    *mode = NetplayMode::Rollback;
    return true;
  }
  return false;
}

bool IsPlayerUsableNetplayMode(NetplayMode mode,
                               bool rollback_production_ready) {
  return mode == NetplayMode::FixedDelay || rollback_production_ready;
}

std::optional<std::string> NormalizeNetplayRoomCode(std::string_view value) {
  const auto is_space = [](unsigned char c) { return std::isspace(c); };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  if (value.size() != 8)
    return std::nullopt;
  std::string normalized;
  normalized.reserve(value.size());
  for (const unsigned char c : value) {
    if (!std::isxdigit(c))
      return std::nullopt;
    normalized.push_back(static_cast<char>(std::tolower(c)));
  }
  return normalized;
}

namespace {
std::string Trim(std::string value) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool ValidNetplayAddress(std::string_view value) {
  if (value.empty() || value.size() > 253)
    return false;
  return std::ranges::all_of(value, [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-' || c == '_';
  });
}

} // namespace

const std::vector<ResolutionOption> &SupportedResolutions() {
  // These are the output-resolution labels used by Dolphin's integer EFB
  // scales.
  static const std::vector<ResolutionOption> resolutions = {
      {"640x528", 1},   {"1280x720", 2},  {"1920x1080", 3},  {"2560x1440", 4},
      {"3840x2160", 6}, {"5120x2880", 8}, {"7680x4320", 12},
  };
  return resolutions;
}

ConfigResult LoadConfig(const fs::path &user_directory,
                        bool create_if_missing) {
  const fs::path path = user_directory / "config.ini";
  if (!fs::exists(path) && create_if_missing) {
    std::string error;
    if (!SaveConfig(user_directory, "1920x1080", true, {}, &error))
      return {.error = std::move(error)};
  }

  std::ifstream file(path);
  if (!file)
    return {.error = "can't open " + path.string()};

  ConfigResult config;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' ||
        trimmed[0] == '[')
      continue;
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string::npos)
      return {.error = "invalid config.ini line: " + trimmed};
    const std::string key = Lower(Trim(trimmed.substr(0, separator)));
    const std::string raw_value = Trim(trimmed.substr(separator + 1));
    const std::string value = Lower(raw_value);
    if (key == "resolution")
      config.resolution = value;
    else if (key == "controller")
      config.controller = raw_value;
    else if (key.starts_with("controller") && key.size() == 11 &&
             key.back() >= '1' && key.back() <= '4') {
      const std::size_t index = static_cast<std::size_t>(key.back() - '1');
      if (config.controllers.size() <= index)
        config.controllers.resize(index + 1);
      config.controllers[index] = raw_value;
    } else if (key == "show_fps_in_title") {
      if (value == "true" || value == "1" || value == "yes" || value == "on")
        config.show_fps_in_title = true;
      else if (value == "false" || value == "0" || value == "no" ||
               value == "off")
        config.show_fps_in_title = false;
      else
        return {.error = "show_fps_in_title must be true or false"};
    } else if (key == "nickname")
      config.netplay_nickname = raw_value;
    else if (key == "address")
      config.netplay_address = raw_value;
    else if (key == "port") {
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          raw_value.data(), raw_value.data() + raw_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != raw_value.data() + raw_value.size() || port == 0 ||
          port > 65535)
        return {.error = "netplay port must be between 1 and 65535"};
      config.netplay_port = static_cast<std::uint16_t>(port);
    } else if (key == "mode") {
      if (!ParseNetplayMode(value, &config.netplay_mode))
        return {.error = "netplay mode must be fixed-delay or rollback"};
    } else if (key == "performance_overlay") {
      if (value == "true" || value == "1" || value == "yes" || value == "on")
        config.netplay_performance_overlay = true;
      else if (value == "false" || value == "0" || value == "no" ||
               value == "off")
        config.netplay_performance_overlay = false;
      else
        return {.error = "netplay performance_overlay must be true or false"};
    } else if (key == "diagnostic_logging") {
      if (value == "true" || value == "1" || value == "yes" || value == "on")
        config.netplay_diagnostic_logging = true;
      else if (value == "false" || value == "0" || value == "no" ||
               value == "off")
        config.netplay_diagnostic_logging = false;
      else
        return {.error = "netplay diagnostic_logging must be true or false"};
    } else if (key == "buffer") {
      if (value != "auto") {
        unsigned int frames = 0;
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), frames);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() || frames < 1 ||
            frames > 20)
          return {.error =
                      "netplay buffer must be auto or a value from 1 to 20"};
      }
      config.netplay_buffer = value;
    }
  }
  if (config.resolution.empty())
    return {.error = "config.ini is missing resolution=<width>x<height>"};

  std::erase(config.controllers, std::string{});
  if (config.controllers.empty() && !config.controller.empty())
    config.controllers.push_back(config.controller);
  if (config.controller.empty() && !config.controllers.empty())
    config.controller = config.controllers.front();
  if (config.netplay_nickname.empty())
    return {.error = "netplay nickname cannot be empty"};
  if (config.netplay_nickname.size() > 30)
    return {.error = "netplay nickname cannot exceed 30 characters"};
  if (!ValidNetplayAddress(config.netplay_address))
    return {.error = "netplay address must be an IPv4 address or hostname"};

  for (const ResolutionOption &option : SupportedResolutions()) {
    if (config.resolution == option.text) {
      config.dolphin_scale = option.dolphin_scale;
      return config;
    }
  }

  // Dolphin also accepts exact raw EFB multiples even when they do not have a
  // common display label.
  for (int scale = 1; scale <= 12; ++scale) {
    const std::string raw =
        std::to_string(640 * scale) + "x" + std::to_string(528 * scale);
    if (config.resolution == raw) {
      config.dolphin_scale = scale;
      return config;
    }
  }

  return {.error = "unsupported Dolphin internal resolution '" +
                   config.resolution +
                   "'; use a listed display resolution or an exact 640x528 "
                   "multiple up to 12x"};
}

bool SaveConfig(const fs::path &user_directory, const ConfigResult &config,
                std::string *error) {
  if (config.resolution.empty() || config.netplay_nickname.empty() ||
      config.netplay_nickname.size() > 30 ||
      config.netplay_nickname.find_first_of("\r\n") != std::string::npos ||
      !ValidNetplayAddress(config.netplay_address) ||
      config.netplay_address.find_first_of("\r\n") != std::string::npos ||
      config.netplay_port == 0) {
    if (error)
      *error = "invalid frontend settings";
    return false;
  }
  if (config.netplay_buffer != "auto") {
    unsigned int frames = 0;
    const auto parsed = std::from_chars(
        config.netplay_buffer.data(),
        config.netplay_buffer.data() + config.netplay_buffer.size(), frames);
    if (parsed.ec != std::errc{} ||
        parsed.ptr !=
            config.netplay_buffer.data() + config.netplay_buffer.size() ||
        frames < 1 || frames > 20) {
      if (error)
        *error = "netplay buffer must be auto or a value from 1 to 20";
      return false;
    }
  }
  std::error_code ec;
  fs::create_directories(user_directory, ec);
  if (ec) {
    if (error)
      *error = "can't create user directory: " + ec.message();
    return false;
  }
  std::ofstream file(user_directory / "config.ini", std::ios::trunc);
  if (!file) {
    if (error)
      *error = "can't write " + (user_directory / "config.ini").string();
    return false;
  }
  file << "# ModernGekko frontend settings\n"
          "# This is Dolphin's internal render target, not the window size.\n"
          "[Video]\n"
          "resolution="
       << config.resolution << '\n'
       << "show_fps_in_title=" << (config.show_fps_in_title ? "true" : "false")
       << '\n'
       << "[Input]\n";
  for (std::size_t i = 0; i < config.controllers.size() && i < 4; ++i) {
    if (config.controllers[i].find_first_of("\r\n") != std::string::npos) {
      if (error)
        *error = "controller device cannot contain a newline";
      return false;
    }
    file << "controller" << i + 1 << '=' << config.controllers[i] << '\n';
  }
  file << "[Netplay]\n"
       << "mode=" << NetplayModeConfigValue(config.netplay_mode) << '\n'
       << "nickname=" << config.netplay_nickname << '\n'
       << "address=" << config.netplay_address << '\n'
       << "port=" << config.netplay_port << '\n'
       << "buffer=" << config.netplay_buffer << '\n'
       << "performance_overlay="
       << (config.netplay_performance_overlay ? "true" : "false") << '\n'
       << "diagnostic_logging="
       << (config.netplay_diagnostic_logging ? "true" : "false") << '\n';
  return true;
}

bool SaveConfig(const fs::path &user_directory, std::string_view resolution,
                bool show_fps_in_title, std::string_view controller,
                std::string *error) {
  ConfigResult config = LoadConfig(user_directory, false);
  if (!config)
    config = {};
  config.resolution = resolution;
  config.show_fps_in_title = show_fps_in_title;
  config.controller = controller;
  config.controllers.clear();
  if (!controller.empty())
    config.controllers.emplace_back(controller);
  return SaveConfig(user_directory, config, error);
}

std::string ReadConfiguredController(const fs::path &user_directory) {
  const std::vector<std::string> controllers =
      ReadConfiguredControllers(user_directory);
  return controllers.empty() ? std::string{} : controllers.front();
}

std::vector<std::string>
ReadConfiguredControllers(const fs::path &user_directory) {
  std::ifstream input(user_directory / "Config" / "WiimoteNew.ini");
  std::vector<std::string> controllers;
  std::string line;
  std::size_t wiimote = 4;
  while (std::getline(input, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.starts_with('[') && trimmed.ends_with(']')) {
      wiimote = 4;
      if (trimmed.size() == 10 && trimmed.starts_with("[Wiimote") &&
          trimmed[8] >= '1' && trimmed[8] <= '4')
        wiimote = static_cast<std::size_t>(trimmed[8] - '1');
      continue;
    }
    if (wiimote >= 4)
      continue;
    const std::size_t separator = trimmed.find('=');
    if (separator != std::string::npos &&
        Trim(trimmed.substr(0, separator)) == "Device") {
      const std::string device = Trim(trimmed.substr(separator + 1));
      if (!device.empty()) {
        if (controllers.size() <= wiimote)
          controllers.resize(wiimote + 1);
        controllers[wiimote] = device;
      }
    }
  }
  std::erase(controllers, std::string{});
  return controllers;
}

bool ControllerConfigExists(const fs::path &user_directory) {
  std::error_code ec;
  return fs::is_regular_file(user_directory / "Config" / "WiimoteNew.ini", ec);
}

bool GCPadConfigExists(const fs::path &user_directory) {
  std::error_code ec;
  return fs::is_regular_file(user_directory / "Config" / "GCPadNew.ini", ec);
}

namespace {
using GamepadControl = moderngekko::frontend::GamepadControl;

struct GamepadControlInfo {
  const char *label;
  const char *key;
  const char *default_expression;
};

constexpr std::array<GamepadControlInfo,
                     moderngekko::frontend::kGamepadControlCount>
    kGamepadControls = {{{"A", "Buttons/A", "`Button S`"},
                         {"B", "Buttons/B", "`Button E`"},
                         {"X", "Buttons/X", "`Button W`"},
                         {"Y", "Buttons/Y", "`Button N`"},
                         {"Z", "Buttons/Z", "`Shoulder R`"},
                         {"Start", "Buttons/Start", "`Start`"},
                         {"L", "Triggers/L", "`Trigger L`"},
                         {"R", "Triggers/R", "`Trigger R`"},
                         {"D-pad Up", "D-Pad/Up", "`Pad N`"},
                         {"D-pad Down", "D-Pad/Down", "`Pad S`"},
                         {"D-pad Left", "D-Pad/Left", "`Pad W`"},
                         {"D-pad Right", "D-Pad/Right", "`Pad E`"},
                         {"Main Stick Up", "Main Stick/Up", "`Left Y+`"},
                         {"Main Stick Down", "Main Stick/Down", "`Left Y-`"},
                         {"Main Stick Left", "Main Stick/Left", "`Left X-`"},
                         {"Main Stick Right", "Main Stick/Right", "`Left X+`"},
                         {"C-Stick Up", "C-Stick/Up", "`Right Y+`"},
                         {"C-Stick Down", "C-Stick/Down", "`Right Y-`"},
                         {"C-Stick Left", "C-Stick/Left", "`Right X-`"},
                         {"C-Stick Right", "C-Stick/Right", "`Right X+`"}}};

constexpr std::array<const char *, 21> kSdlButtonNames = {
    "Button S", "Button E", "Button W", "Button N", "Back",    "Guide",
    "Start",    "Thumb L",  "Thumb R",  "Shoulder L", "Shoulder R",
    "Pad N",    "Pad S",    "Pad W",    "Pad E",      "Misc 1",
    "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4",   "Touchpad",
};

constexpr std::array<const char *, 6> kSdlAxisNames = {
    "Left X", "Left Y", "Right X", "Right Y", "Trigger L", "Trigger R"};

std::size_t ControlIndex(GamepadControl control) {
  return static_cast<std::size_t>(control);
}

bool ValidProfileValue(std::string_view value) {
  return !value.empty() &&
         value.find_first_of("\r\n") == std::string_view::npos;
}

std::string SettingKey(std::string_view line) {
  const std::string trimmed = Trim(std::string(line));
  const auto separator = trimmed.find('=');
  if (separator == std::string::npos)
    return {};
  return Trim(trimmed.substr(0, separator));
}

bool WriteProfileLines(const fs::path &destination,
                       const std::vector<std::string> &lines,
                       std::string *message) {
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (message)
      *message = "can't create controller config directory: " + ec.message();
    return false;
  }

  const fs::path temporary = destination.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      if (message)
        *message = "can't write " + temporary.string();
      return false;
    }
    for (const std::string &line : lines)
      output << line << '\n';
    output.close();
    if (!output) {
      if (message)
        *message = "can't write " + temporary.string();
      fs::remove(temporary, ec);
      return false;
    }
  }

  const fs::path backup = destination.string() + ".bak";
  if (fs::is_regular_file(destination, ec)) {
    ec.clear();
    fs::copy_file(destination, backup, fs::copy_options::overwrite_existing,
                  ec);
    if (ec) {
      if (message)
        *message = "can't back up controller profile: " + ec.message();
      fs::remove(temporary, ec);
      return false;
    }
  }

  // Windows cannot rename over an existing path. Replace it only after the
  // backup is durable, then restore that backup if the final rename fails.
  ec.clear();
  fs::remove(destination, ec);
  if (ec) {
    if (message)
      *message = "can't replace controller profile: " + ec.message();
    fs::remove(temporary, ec);
    return false;
  }
  fs::rename(temporary, destination, ec);
  if (ec) {
    std::error_code restore_error;
    if (fs::is_regular_file(backup, restore_error))
      fs::copy_file(backup, destination, fs::copy_options::overwrite_existing,
                    restore_error);
    if (message)
      *message = "can't install controller profile: " + ec.message();
    fs::remove(temporary, restore_error);
    return false;
  }
  return true;
}
} // namespace

std::string_view GamepadControlLabel(GamepadControl control) {
  const std::size_t index = ControlIndex(control);
  return index < kGamepadControls.size() ? kGamepadControls[index].label : "";
}

GamepadProfile DefaultGamepadProfile(std::string_view device) {
  GamepadProfile profile;
  profile.device = device;
  for (std::size_t i = 0; i < kGamepadControls.size(); ++i)
    profile.bindings[i] = kGamepadControls[i].default_expression;
  return profile;
}

bool LoadGamepadProfile(const fs::path &user_directory, GamepadProfile *profile,
                        std::string *message) {
  if (profile == nullptr) {
    if (message)
      *message = "missing controller profile output";
    return false;
  }
  const fs::path destination = user_directory / "Config" / "GCPadNew.ini";
  std::ifstream input(destination);
  if (!input) {
    if (message)
      *message = "can't open " + destination.string();
    return false;
  }

  *profile = DefaultGamepadProfile({});
  bool in_pad_one = false;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string trimmed = Trim(line);
    if (trimmed.starts_with('[')) {
      in_pad_one = trimmed == "[GCPad1]";
      continue;
    }
    if (!in_pad_one)
      continue;
    const auto separator = trimmed.find('=');
    if (separator == std::string::npos)
      continue;
    const std::string key = Trim(trimmed.substr(0, separator));
    const std::string value = Trim(trimmed.substr(separator + 1));
    if (key == "Device")
      profile->device = value;
    for (std::size_t i = 0; i < kGamepadControls.size(); ++i) {
      if (key == kGamepadControls[i].key)
        profile->bindings[i] = value;
    }
  }
  if (profile->device.empty()) {
    if (message)
      *message = "GCPad1 has no input device";
    return false;
  }
  return true;
}

bool SaveGamepadProfile(const fs::path &user_directory,
                        const GamepadProfile &profile, std::string *message) {
  if (!ValidProfileValue(profile.device)) {
    if (message)
      *message = "invalid gamepad device name";
    return false;
  }
  for (const std::string &binding : profile.bindings) {
    if (!ValidProfileValue(binding)) {
      if (message)
        *message = "every GameCube control must have a valid binding";
      return false;
    }
  }

  const fs::path destination = user_directory / "Config" / "GCPadNew.ini";
  std::vector<std::string> lines;
  {
    std::ifstream input(destination);
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      lines.push_back(std::move(line));
    }
  }
  if (lines.empty())
    lines.emplace_back("[GCPad1]");

  std::map<std::string, std::string> replacements;
  replacements["Device"] = profile.device;
  for (std::size_t i = 0; i < kGamepadControls.size(); ++i)
    replacements[kGamepadControls[i].key] = profile.bindings[i];
  replacements["Triggers/L-Analog"] =
      profile.bindings[ControlIndex(GamepadControl::L)];
  replacements["Triggers/R-Analog"] =
      profile.bindings[ControlIndex(GamepadControl::R)];

  bool found_pad_one = false;
  bool in_pad_one = false;
  std::map<std::string, bool> written;
  std::vector<std::string> updated;
  const auto append_missing = [&] {
    for (const auto &[key, value] : replacements) {
      if (!written[key])
        updated.push_back(key + " = " + value);
    }
    if (!written["Main Stick/Calibration"])
      updated.emplace_back("Main Stick/Calibration = 100.00 141.42 100.00 "
                           "141.42 100.00 141.42 100.00 141.42");
    if (!written["C-Stick/Calibration"])
      updated.emplace_back("C-Stick/Calibration = 100.00 141.42 100.00 "
                           "141.42 100.00 141.42 100.00 141.42");
  };

  for (const std::string &line : lines) {
    const std::string trimmed = Trim(line);
    if (trimmed.starts_with('[')) {
      if (in_pad_one)
        append_missing();
      in_pad_one = trimmed == "[GCPad1]";
      found_pad_one = found_pad_one || in_pad_one;
      updated.push_back(line);
      continue;
    }
    if (!in_pad_one) {
      updated.push_back(line);
      continue;
    }
    const std::string key = SettingKey(line);
    if (key == "Main Stick/Calibration" || key == "C-Stick/Calibration")
      written[key] = true;
    const auto replacement = replacements.find(key);
    if (replacement == replacements.end()) {
      updated.push_back(line);
      continue;
    }
    if (!written[key]) {
      updated.push_back(key + " = " + replacement->second);
      written[key] = true;
    }
  }
  if (in_pad_one)
    append_missing();
  if (!found_pad_one) {
    if (!updated.empty() && !updated.back().empty())
      updated.emplace_back();
    updated.emplace_back("[GCPad1]");
    append_missing();
  }

  if (!WriteProfileLines(destination, updated, message))
    return false;
  if (message)
    *message = "gamepad mapping saved (" + profile.device + ")";
  return true;
}

std::optional<std::string> DolphinSdlButtonExpression(int button) {
  if (button < 0 || static_cast<std::size_t>(button) >= kSdlButtonNames.size())
    return std::nullopt;
  return "`" + std::string(kSdlButtonNames[button]) + "`";
}

std::optional<std::string> DolphinSdlAxisExpression(int axis, int value) {
  if (axis < 0 || static_cast<std::size_t>(axis) >= kSdlAxisNames.size() ||
      value == 0)
    return std::nullopt;
  std::string expression = "`" + std::string(kSdlAxisNames[axis]);
  if (axis < 4) {
    bool negative = value < 0;
    if (axis % 2 == 1)
      negative = !negative;
    expression += negative ? '-' : '+';
  }
  expression += '`';
  return expression;
}

bool WriteKeyboardGCPadConfig(const fs::path &user_directory,
                              KeyboardLayout layout, std::string *message) {
  // Dolphin's Linux keyboard/mouse device is the X master pointer/keyboard
  // pair, exposed by the XInput2 backend under the pointer's name. The game
  // runs under XWayland here, so this works on a Wayland session too.
  constexpr const char *kDevice = "XInput2/0/Virtual core pointer";

  // Two disjoint layouts, so one keyboard can drive two local instances --
  // which is exactly what a two-peer netplay session on one machine needs, and
  // is unavoidable there: input has to keep working without window focus, so
  // both instances see every key.
  struct Keys {
    const char *up, *down, *left, *right;
    const char *a, *b, *x, *y, *z, *l, *r, *start;
  };
  const Keys keys = (layout == KeyboardLayout::Player1)
                        ? Keys{"Up", "Down", "Left", "Right", "Z", "X",
                               "C", "V", "F", "A", "S", "Return"}
                        : Keys{"I", "K", "J", "L", "B", "N",
                               "M", "comma", "H", "G", "T", "Y"};

  const fs::path destination = user_directory / "Config" / "GCPadNew.ini";
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (message)
      *message = "can't create controller config directory: " + ec.message();
    return false;
  }
  std::ofstream output(destination, std::ios::trunc);
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }

  // Only port 1 is mapped. Under netplay each machine supplies one pad and the
  // host's mapping decides which in-game port it drives, so a local port 2
  // would be wrong there; for single player the game only needs port 1.
  output << "[GCPad1]\n"
         << "Device = " << kDevice << '\n'
         << "Buttons/A = `" << keys.a << "`\n"
         << "Buttons/B = `" << keys.b << "`\n"
         << "Buttons/X = `" << keys.x << "`\n"
         << "Buttons/Y = `" << keys.y << "`\n"
         << "Buttons/Z = `" << keys.z << "`\n"
         << "Buttons/Start = `" << keys.start << "`\n"
         << "Triggers/L = `" << keys.l << "`\n"
         << "Triggers/R = `" << keys.r << "`\n"
         << "D-Pad/Up = `" << keys.up << "`\n"
         << "D-Pad/Down = `" << keys.down << "`\n"
         << "D-Pad/Left = `" << keys.left << "`\n"
         << "D-Pad/Right = `" << keys.right << "`\n"
         // The stick as well as the D-pad: this game reads movement from the
         // analog stick, and a D-pad-only binding leaves the character rooted.
         << "Main Stick/Up = `" << keys.up << "`\n"
         << "Main Stick/Down = `" << keys.down << "`\n"
         << "Main Stick/Left = `" << keys.left << "`\n"
         << "Main Stick/Right = `" << keys.right << "`\n";
  output.close();
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }
  if (message)
    *message = std::string("keyboard mapped (") +
               (layout == KeyboardLayout::Player1 ? "arrows + ZXCV"
                                                 : "IJKL + BNM") +
               ")";
  return true;
}

std::vector<std::string> DetectSdlGamepads() {
  std::vector<std::string> devices;
#ifndef MODERNGEKKO_NO_SDL_GAMEPADS

  // Refcounted, and this runs before the emulator brings up its own input, so
  // initialising here does not disturb Dolphin's later SDL use. Quit only what
  // we started. No video subsystem: gamepads enumerate headless, which is what
  // Game Mode and CI both need.
  const bool started = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!started)
    return devices;

  int count = 0;
  SDL_JoystickID *const ids = SDL_GetGamepads(&count);
  if (ids != nullptr) {
    for (int i = 0; i < count; ++i) {
      // Dolphin names a device "<source>/<id>/<name>", numbering ids per source
      // in the order it adds them, and its SDL backend takes the name from
      // SDL_GetGamepadName -- so enumerating in SDL's order reproduces it.
      SDL_Gamepad *const pad = SDL_OpenGamepad(ids[i]);
      if (pad == nullptr)
        continue;
      const char *const name = SDL_GetGamepadName(pad);
      if (name != nullptr && *name != '\0')
        devices.push_back("SDL/" + std::to_string(devices.size()) + "/" + name);
      SDL_CloseGamepad(pad);
    }
    SDL_free(ids);
  }

  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
#endif
  return devices;
}

bool WriteGamepadGCPadConfig(const fs::path &user_directory,
                             std::string_view device, std::string *message) {
  return SaveGamepadProfile(user_directory, DefaultGamepadProfile(device),
                            message);
}

bool GenerateControllerConfig(const fs::path &user_directory,
                              std::span<const std::string> controllers,
                              std::string *message) {
  if (controllers.empty() || controllers.size() > 4) {
    if (message)
      *message = "select between one and four connected SDL gamepads";
    return false;
  }
  for (const std::string &controller : controllers) {
    if (controller.empty() ||
        controller.find_first_of("\r\n") != std::string_view::npos) {
      if (message)
        *message = "select connected SDL gamepads";
      return false;
    }
  }

  const fs::path destination = user_directory / "Config" / "WiimoteNew.ini";
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (message)
      *message = "can't create controller config directory: " + ec.message();
    return false;
  }
  std::ofstream output(destination, std::ios::trunc);
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i) {
    output << "[Wiimote" << i + 1 << "]\n";
    if (i >= controllers.size())
      continue;
    output << "Device = " << controllers[i] << '\n'
           << "Buttons/A = `Shoulder L`\n"
              "Buttons/B = `Shoulder R`\n"
              "Buttons/1 = `Button W`\n"
              "Buttons/2 = `Button S`\n"
              "Buttons/- = Back\n"
              "Buttons/+ = Start\n"
              "Buttons/Home = Guide\n"
              "D-Pad/Up = `Pad N`\n"
              "D-Pad/Down = `Pad S`\n"
              "D-Pad/Left = `Pad W`\n"
              "D-Pad/Right = `Pad E`\n"
              "IR/Up = `Cursor Y-`\n"
              "IR/Down = `Cursor Y+`\n"
              "IR/Left = `Cursor X-`\n"
              "IR/Right = `Cursor X+`\n"
              "Shake/X = `Trigger L`\n"
              "Shake/Y = `Trigger R`\n"
              "Shake/Z = `Trigger L`\n"
              "IRPassthrough/Object 1 X = `IR Object 1 X`\n"
              "IRPassthrough/Object 1 Y = `IR Object 1 Y`\n"
              "IRPassthrough/Object 1 Size = `IR Object 1 Size`\n"
              "IRPassthrough/Object 2 X = `IR Object 2 X`\n"
              "IRPassthrough/Object 2 Y = `IR Object 2 Y`\n"
              "IRPassthrough/Object 2 Size = `IR Object 2 Size`\n"
              "IRPassthrough/Object 3 X = `IR Object 3 X`\n"
              "IRPassthrough/Object 3 Y = `IR Object 3 Y`\n"
              "IRPassthrough/Object 3 Size = `IR Object 3 Size`\n"
              "IRPassthrough/Object 4 X = `IR Object 4 X`\n"
              "IRPassthrough/Object 4 Y = `IR Object 4 Y`\n"
              "IRPassthrough/Object 4 Size = `IR Object 4 Size`\n"
              "IMUAccelerometer/Up = `Accel Up`\n"
              "IMUAccelerometer/Down = `Accel Down`\n"
              "IMUAccelerometer/Left = `Accel Left`\n"
              "IMUAccelerometer/Right = `Accel Right`\n"
              "IMUAccelerometer/Forward = `Accel Forward`\n"
              "IMUAccelerometer/Backward = `Accel Backward`\n"
              "IMUGyroscope/Pitch Up = `Gyro Pitch Up`\n"
              "IMUGyroscope/Pitch Down = `Gyro Pitch Down`\n"
              "IMUGyroscope/Roll Left = `Gyro Roll Left`\n"
              "IMUGyroscope/Roll Right = `Gyro Roll Right`\n"
              "IMUGyroscope/Yaw Left = `Gyro Yaw Left`\n"
              "IMUGyroscope/Yaw Right = `Gyro Yaw Right`\n"
              "Rumble/Motor = Motor\n"
              "Extension = None\n"
              "Options/Sideways Wiimote = True\n";
  }
  output << "[BalanceBoard]\n";
  if (!output) {
    if (message)
      *message = "can't write " + destination.string();
    return false;
  }
  if (message)
    *message = std::to_string(controllers.size()) + " sideways Wii Remote" +
               (controllers.size() == 1 ? " mapped" : "s mapped");
  return true;
}

bool GenerateControllerConfig(const fs::path &user_directory,
                              std::string_view controller,
                              std::string *message) {
  const std::string value(controller);
  return GenerateControllerConfig(
      user_directory, std::span<const std::string>(&value, 1), message);
}

bool EnsureControllerConfig(const fs::path &user_directory,
                            std::span<const std::string> controllers,
                            std::string *message) {
  // A GameCube title needs GCPadNew.ini, and nothing here ever wrote one. A
  // fresh user directory gets a pad if one is connected and a keyboard
  // otherwise; an existing profile is never touched.
  //
  // The keyboard used to be unconditional, which is why the Steam Deck detected
  // no input at all in Game Mode: it has a built-in controller and no keyboard,
  // and the CONTROLS tab that could have fixed it is behind a menu you need
  // working input to reach.
  // Owns whatever DetectSdlGamepads() finds. It has to outlive the block
  // below, because `controllers` is a span and is repointed at it there: when
  // this vector lived inside that block, the span dangled the moment the block
  // ended and the GenerateControllerConfig call at the bottom read freed
  // memory. That crashed at startup on any machine with a pad connected and a
  // user directory fresh enough to have no WiimoteNew.ini -- the Steam Deck
  // every time, a desktop never, because with no pad detected the span was
  // left pointing at the caller's own storage.
  std::vector<std::string> detected;
  if (!GCPadConfigExists(user_directory)) {
    // An explicitly selected pad wins; otherwise ask the hardware.
    if (controllers.empty()) {
      detected = DetectSdlGamepads();
      controllers = detected;
    }

    std::string pad_message;
    const bool wrote_pad =
        !controllers.empty() &&
        WriteGamepadGCPadConfig(user_directory, controllers.front(),
                                &pad_message);
    if (!wrote_pad)
      WriteKeyboardGCPadConfig(user_directory, KeyboardLayout::Player1,
                               &pad_message);
    if (message)
      *message = pad_message;
  }
  if (ControllerConfigExists(user_directory)) {
    if (message && message->empty())
      *message = "using existing controller profile";
    return true;
  }
  // The Wiimote profile is optional for a GameCube game: failing to map an SDL
  // gamepad must not stop a keyboard player from booting.
  std::string ignored;
  GenerateControllerConfig(user_directory, controllers, &ignored);
  return true;
}

bool EnsureControllerConfig(const fs::path &user_directory,
                            std::string_view controller, std::string *message) {
  const std::string value(controller);
  return EnsureControllerConfig(
      user_directory, std::span<const std::string>(&value, 1), message);
}
} // namespace moderngekko::frontend
