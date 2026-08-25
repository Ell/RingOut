#include "frontend_config.hpp"
#include "moderngekko/game.hpp"
#include "netplay_session.hpp"

#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "Core/NetPlay/LiveRollbackOutputGate.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
#ifndef MODERNGEKKO_FRONTEND_NAME
#define MODERNGEKKO_FRONTEND_NAME "ModernGekko"
#endif

#ifndef MODERNGEKKO_RUNNER_FILENAME
#define MODERNGEKKO_RUNNER_FILENAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

struct ExtractionState {
  std::atomic<bool> running{false};
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> total{1};
  std::mutex mutex;
  std::string status;
  std::string error;
  std::optional<fs::path> finished_game;
};

struct ModuleBuildState {
  std::atomic<bool> running{false};
  std::mutex mutex;
  int phase = 0;
  std::string status;
  std::string error;
  std::string log;
  std::string phase_scan;
  std::optional<fs::path> finished_module;
};

enum class LauncherPage {
  Play,
  Setup,
  Netplay,
  Mods,
  Settings,
};

struct DialogState {
  std::mutex mutex;
  std::optional<fs::path> selected;
  std::string error;
};

struct LauncherFonts {
  ImFont *body = nullptr;
  ImFont *heading = nullptr;
  ImFont *brand = nullptr;
};

struct ControllerOption {
  std::string label;
  std::string device;
};

std::vector<ControllerOption> EnumerateControllers() {
  std::vector<ControllerOption> result;
  std::unordered_map<std::string, int> device_ids;
  int count = 0;
  SDL_JoystickID *joystick_ids = SDL_GetJoysticks(&count);
  for (int i = 0; i < count; ++i) {
    const SDL_JoystickID joystick_id = joystick_ids[i];
    const bool gamepad = SDL_IsGamepad(joystick_id);
    const char *name_value = gamepad ? SDL_GetGamepadNameForID(joystick_id)
                                     : SDL_GetJoystickNameForID(joystick_id);
    const std::string name =
        name_value && *name_value ? name_value : "Unknown Controller";
    const int device_id = device_ids[name]++;
    if (!gamepad)
      continue;
    ControllerOption option;
    option.label = device_id == 0
                       ? name
                       : name + " (" + std::to_string(device_id + 1) + ")";
    option.device = "SDL/" + std::to_string(device_id) + "/" + name;
    result.emplace_back(std::move(option));
  }
  SDL_free(joystick_ids);
  return result;
}

int FindController(const std::vector<ControllerOption> &controllers,
                   std::string_view device) {
  const auto found =
      std::ranges::find(controllers, device, &ControllerOption::device);
  return found == controllers.end()
             ? -1
             : static_cast<int>(found - controllers.begin());
}

fs::path DefaultUserDirectory() {
#if defined(_WIN32)
  if (const char *local_app_data = std::getenv("LOCALAPPDATA"))
    return fs::path(local_app_data) / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char *xdg = std::getenv("XDG_DATA_HOME"))
    return fs::path(xdg) / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const char *home = std::getenv("HOME"))
    return fs::path(home) / ".local/share" / MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
}

fs::path DocumentsDirectory() {
#if defined(_WIN32)
  if (const char *user_profile = std::getenv("USERPROFILE"))
    return fs::path(user_profile) / "Documents";
#endif
  if (const char *home = std::getenv("HOME"))
    return fs::path(home) / "Documents";
  return fs::current_path();
}

fs::path ReadDefaultGame(const fs::path &user_directory) {
  std::ifstream file(user_directory / "default-game.txt");
  std::string value;
  std::getline(file, value);
  if (!value.empty() && value.back() == '\r')
    value.pop_back();
  return value;
}

bool WriteDefaultGame(const fs::path &user_directory, const fs::path &game,
                      std::string *error) {
  std::error_code ec;
  fs::create_directories(user_directory, ec);
  std::ofstream file(user_directory / "default-game.txt", std::ios::trunc);
  if (!file) {
    if (error)
      *error = "can't save default-game.txt";
    return false;
  }
  file << game.string() << '\n';
  return true;
}

std::vector<fs::path> FindDiscImages() {
  std::vector<fs::path> images;
  std::error_code ec;
  const fs::path documents = DocumentsDirectory();
  if (!fs::is_directory(documents, ec))
    return images;
  fs::recursive_directory_iterator iterator(
      documents, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (iterator != end) {
    if (iterator.depth() > 4)
      iterator.disable_recursion_pending();
    if (iterator->is_regular_file(ec)) {
      std::string extension = iterator->path().extension().string();
      std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (extension == ".wbfs" || extension == ".iso" || extension == ".rvz")
        images.push_back(iterator->path());
    }
    iterator.increment(ec);
    if (ec)
      ec.clear();
  }
  std::ranges::sort(images);
  return images;
}

bool ExtractDisc(const fs::path &image, const fs::path &user_directory,
                 ExtractionState *state, bool replace_existing = false) {
  auto fail = [&](std::string message) {
    std::lock_guard lock(state->mutex);
    state->error = std::move(message);
    state->running = false;
    return false;
  };

  {
    std::lock_guard lock(state->mutex);
    state->status = "Opening " + image.filename().string();
  }
  std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(image.string());
  if (!volume)
    return fail("RingOut could not read this file as a supported GameCube "
                "disc image.");

  const DiscIO::Partition partition = volume->GetGamePartition();
  const DiscIO::FileSystem *filesystem = volume->GetFileSystem(partition);
  if (!filesystem || !filesystem->IsValid())
    return fail("The disc image is unreadable or incomplete.");

  std::string disc_id = volume->GetGameID(partition);
  if (disc_id.size() != 6)
    return fail("The selected file does not contain a valid GameCube disc ID.");
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
    return fail("This is not the supported USA release for RingOut. "
                "Expected " MODERNGEKKO_REQUIRED_DISC_ID "; found " +
                disc_id + ".");
#endif
  const fs::path games_directory = user_directory / "games";
  const fs::path output = games_directory / disc_id;
  if (!replace_existing && moderngekko::InspectGame(output)) {
    std::string error;
    if (!WriteDefaultGame(user_directory, output, &error))
      return fail(error);
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Using existing extraction";
    state->running = false;
    return true;
  }

  const fs::path staging = games_directory / (disc_id + ".extracting");
  std::error_code ec;
  fs::remove_all(staging, ec);
  fs::create_directories(staging / "files", ec);
  if (ec)
    return fail("can't create extraction directory: " + ec.message());

  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting system data";
  }
  if (!DiscIO::ExportSystemData(*volume, partition, staging.string())) {
    fs::remove_all(staging, ec);
    return fail("Dolphin failed while extracting the disc system data");
  }

  state->total = std::max(1u, filesystem->GetRoot().GetTotalChildren());
  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting game files";
  }
  DiscIO::ExportDirectory(*volume, partition, filesystem->GetRoot(), true, "",
                          (staging / "files").string(),
                          [state](const std::string &path) {
                            ++state->completed;
                            std::lock_guard lock(state->mutex);
                            state->status = "Extracting " + path;
                            return false;
                          });

  const auto inspected = moderngekko::InspectGame(staging);
  if (!inspected) {
    fs::remove_all(staging, ec);
    return fail("extracted game validation failed: " + inspected.error);
  }

  fs::remove_all(output, ec);
  ec.clear();
  fs::rename(staging, output, ec);
  if (ec)
    return fail("can't publish extracted game: " + ec.message());
  std::string error;
  if (!WriteDefaultGame(user_directory, output, &error))
    return fail(error);

  {
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Extraction complete";
  }
  state->running = false;
  return true;
}

void SDLCALL FileDialogCallback(void *userdata, const char *const *filelist,
                                int) {
  auto *state = static_cast<DialogState *>(userdata);
  std::lock_guard lock(state->mutex);
  if (!filelist)
    state->error = SDL_GetError();
  else if (filelist[0])
    state->selected = filelist[0];
}

fs::path SiblingRunner(const char *argv0) {
  std::error_code ec;
  const fs::path self = fs::weakly_canonical(argv0, ec);
  fs::path runner = MODERNGEKKO_RUNNER_FILENAME;
#if defined(_WIN32)
  runner += ".exe";
#endif
  const fs::path sibling = self.parent_path() / runner;
  if (fs::is_regular_file(sibling))
    return sibling;
  const fs::path packaged = self.parent_path() / "bin" / runner;
  return fs::is_regular_file(packaged) ? packaged : runner;
}

fs::path SiblingPort(const char *argv0) {
  std::error_code ec;
  const fs::path self = fs::weakly_canonical(argv0, ec);
  fs::path port = "moderngekko-port";
#if defined(_WIN32)
  port += ".exe";
#endif
  const fs::path sibling = self.parent_path() / port;
  if (fs::is_regular_file(sibling))
    return sibling;
  const fs::path packaged = self.parent_path() / "tools" / port;
  return fs::is_regular_file(packaged) ? packaged : port;
}

fs::path SiblingAsset(const char *argv0, const fs::path &asset) {
  std::error_code ec;
  const fs::path self = fs::weakly_canonical(argv0, ec);
  return self.parent_path() / asset;
}

std::optional<fs::path> ReadActiveModule(const fs::path &user_directory,
                                         std::string_view disc_id,
                                         std::string_view dol_sha256) {
  std::ifstream input(user_directory / "Builds" / disc_id /
                      "active-module.txt");
  std::string value;
  std::getline(input, value);
  if (!value.empty() && value.back() == '\r')
    value.pop_back();
  if (value.empty() || !fs::is_regular_file(value))
    return std::nullopt;

  std::ifstream manifest(fs::path(value).parent_path() / "manifest.txt");
  bool matching_disc = false;
  bool matching_dol = false;
  std::string line;
  while (std::getline(manifest, line)) {
    matching_disc |= line == "disc_id=" + std::string(disc_id);
    matching_dol |= line == "dol_sha256=" + std::string(dol_sha256);
  }
  if (matching_disc && matching_dol)
    return fs::path(value);
  return std::nullopt;
}

std::optional<fs::path> FindInstalledModule(const char *argv0,
                                            const fs::path &user_directory,
                                            std::string_view disc_id,
                                            std::string_view dol_sha256) {
  (void)argv0;
  return ReadActiveModule(user_directory, disc_id, dol_sha256);
}

void UpdateBuildStatus(ModuleBuildState *state, std::string_view chunk) {
  state->phase_scan.append(chunk);
  if (state->phase_scan.size() > 1024)
    state->phase_scan.erase(0, state->phase_scan.size() - 1024);
  if (state->phase_scan.find("[ringout-setup] phase=publish") !=
      std::string::npos) {
    state->phase = std::max(state->phase, 5);
    state->status = "Finishing setup";
  } else if (state->phase_scan.find("[ringout-setup] phase=compile") !=
             std::string::npos) {
    state->phase = std::max(state->phase, 4);
    state->status = "Building game files";
  } else if (state->phase_scan.find("[ringout-setup] phase=configure") !=
             std::string::npos) {
    state->phase = std::max(state->phase, 3);
    state->status = "Preparing the build";
  } else if (state->phase_scan.find("[ringout-setup] phase=translate") !=
             std::string::npos) {
    state->phase = std::max(state->phase, 2);
    state->status = "Translating PowerPC game code";
  } else if (state->phase_scan.find("[ringout-setup] phase=inspect") !=
             std::string::npos) {
    state->phase = std::max(state->phase, 1);
    state->status = "Verifying the extracted game";
  }
}

void BuildModule(const char *argv0, const fs::path &game,
                 const fs::path &user_directory, std::string disc_id,
                 std::string dol_sha256, bool force_rebuild,
                 ModuleBuildState *state) {
  const fs::path port = SiblingPort(argv0);
  const fs::path output = user_directory / "Builds";
  std::vector<std::string> storage = {
      port.string(), "build",         game.string(),
      "--output",    output.string(), "--setup-progress"};
  if (force_rebuild)
    storage.emplace_back("--force-rebuild");
  std::vector<const char *> arguments;
  arguments.reserve(storage.size() + 1);
  for (const std::string &argument : storage)
    arguments.push_back(argument.c_str());
  arguments.push_back(nullptr);

  const SDL_PropertiesID properties = SDL_CreateProperties();
  SDL_Process *process = nullptr;
  if (properties) {
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                           arguments.data());
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                          SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(
        properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
  }
  if (!process) {
    std::lock_guard lock(state->mutex);
    state->error = "Could not start the setup helper " + port.string() + ": " +
                   SDL_GetError();
    state->running = false;
    return;
  }

  std::error_code log_error;
  fs::create_directories(user_directory / "Logs", log_error);
  std::ofstream log_file(user_directory / "Logs" / "setup.log",
                         std::ios::trunc);

  SDL_IOStream *output_stream = SDL_GetProcessOutput(process);
  std::array<char, 2048> buffer{};
  while (output_stream) {
    const std::size_t count =
        SDL_ReadIO(output_stream, buffer.data(), buffer.size());
    if (count > 0) {
      if (log_file) {
        log_file.write(buffer.data(), static_cast<std::streamsize>(count));
        log_file.flush();
      }
      std::lock_guard lock(state->mutex);
      state->log.append(buffer.data(), count);
      if (state->log.size() > 256 * 1024)
        state->log.erase(0, state->log.size() - 256 * 1024);
      UpdateBuildStatus(state, std::string_view(buffer.data(), count));
      continue;
    }
    if (SDL_GetIOStatus(output_stream) != SDL_IO_STATUS_NOT_READY)
      break;
    SDL_Delay(16);
  }

  int exit_code = 1;
  const bool waited = SDL_WaitProcess(process, true, &exit_code);
  const std::string wait_error = waited ? std::string{} : SDL_GetError();
  SDL_DestroyProcess(process);
  {
    std::lock_guard lock(state->mutex);
    if (!waited)
      state->error = "The setup helper could not be monitored: " + wait_error;
    else if (exit_code != 0)
      state->error = log_file ? "Game setup failed. Open the setup log for "
                                "details."
                              : "Game setup failed. Expand the technical log "
                                "for details.";
    else if (auto module =
                 ReadActiveModule(user_directory, disc_id, dol_sha256)) {
      state->finished_module = std::move(module);
      state->status = "Game ready";
    } else
      state->error = "Setup finished without creating the required game files.";
  }
  state->running = false;
}

void ConfigureLauncherStyle(float scale) {
  ImGuiStyle &style = ImGui::GetStyle();
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 0.0f;
  style.ChildRounding = 6.0f * scale;
  style.FrameRounding = 4.0f * scale;
  style.PopupRounding = 6.0f * scale;
  style.GrabRounding = 3.0f * scale;
  style.WindowPadding = ImVec2(0.0f, 0.0f);
  style.FramePadding = ImVec2(12.0f * scale, 7.0f * scale);
  style.ItemSpacing = ImVec2(10.0f * scale, 9.0f * scale);
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.067f, 0.075f, 0.094f, 1.0f);
  style.Colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.122f, 0.145f, 1.0f);
  style.Colors[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.122f, 0.145f, 1.0f);
  style.Colors[ImGuiCol_Border] = ImVec4(0.22f, 0.24f, 0.28f, 0.9f);
  style.Colors[ImGuiCol_Text] = ImVec4(0.94f, 0.95f, 0.97f, 1.0f);
  style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.64f, 0.67f, 0.72f, 1.0f);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.145f, 0.17f, 1.0f);
  style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
  style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.27f, 1.0f);
  style.Colors[ImGuiCol_Button] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.21f, 0.23f, 0.28f, 1.0f);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.43f, 0.76f, 1.0f);
  style.Colors[ImGuiCol_Header] = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.21f, 0.24f, 0.29f, 1.0f);
  style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.43f, 0.76f, 1.0f);
  style.Colors[ImGuiCol_CheckMark] = ImVec4(0.36f, 0.58f, 0.93f, 1.0f);
  style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.36f, 0.58f, 0.93f, 1.0f);
  style.Colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.24f, 0.28f, 0.8f);
}

bool NavigationButton(const char *label, LauncherPage target,
                      LauncherPage current, float width, float scale) {
  const bool selected = current == target;
  const ImVec2 position = ImGui::GetCursorScreenPos();
  if (selected) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.20f, 0.22f, 0.27f, 1.0f));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(0.067f, 0.075f, 0.094f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.14f, 0.16f, 0.19f, 1.0f));
  }
  ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.08f, 0.5f));
  const bool pressed = ImGui::Button(label, ImVec2(width, 40.0f * scale));
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  if (selected) {
    ImGui::GetWindowDrawList()->AddRectFilled(
        position, ImVec2(position.x + 3.0f * scale, position.y + 40.0f * scale),
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.36f, 0.58f, 0.93f, 1.0f)),
        2.0f * scale);
  }
  return pressed;
}

bool PrimaryButton(const char *label, const ImVec2 &size) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.43f, 0.76f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.31f, 0.50f, 0.86f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                        ImVec4(0.20f, 0.36f, 0.66f, 1.0f));
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  return pressed;
}

void SectionHeading(ImFont *heading_font, const char *title,
                    const char *description, float width, float scale) {
  ImGui::PushFont(heading_font);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  ImGui::TextDisabled("%s", description);
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0.0f, 6.0f * scale));
  ImGui::Separator();
  ImGui::Dummy(ImVec2(0.0f, 10.0f * scale));
}

void StatusLine(const char *label, const ImVec4 &color, float scale,
                bool muted = false) {
  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  const float line_height = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddCircleFilled(
      ImVec2(cursor.x + 5.0f * scale, cursor.y + line_height * 0.5f),
      4.0f * scale, ImGui::ColorConvertFloat4ToU32(color));
  ImGui::Dummy(ImVec2(12.0f * scale, line_height));
  ImGui::SameLine(0.0f, 8.0f * scale);
  if (muted)
    ImGui::TextDisabled("%s", label);
  else
    ImGui::TextUnformatted(label);
}
} // namespace

int main(int argc, char **argv) {
  bool use_wayland = false;
  bool self_test = false;
  std::optional<fs::path> user_directory_override;
  std::optional<fs::path> extract_only;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "-X11" || arg == "--x11")
      use_wayland = false;
    else if (arg == "--wayland")
      use_wayland = true;
    else if (arg == "--extract") {
      if (i + 1 >= argc) {
        std::cerr << "--extract requires a value\n";
        return 2;
      }
      extract_only = argv[++i];
    } else if (arg == "--ringout-self-test")
      self_test = true;
    else if (arg == "--user-dir") {
      if (i + 1 >= argc) {
        std::cerr << "--user-dir requires a value\n";
        return 2;
      }
      user_directory_override = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: RingOut [--x11|--wayland] [--user-dir DIR] "
                   "[--extract DISC] [--ringout-self-test]\n";
      return 0;
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      return 2;
    }
  }

  if (self_test) {
    const fs::path runner = SiblingRunner(argv[0]);
    const fs::path port = SiblingPort(argv[0]);
    const fs::path fonts = SiblingAsset(argv[0], "fonts");
    const bool valid =
        fs::is_regular_file(runner) && fs::is_regular_file(port) &&
        fs::is_regular_file(fonts / "DroidSans.ttf") &&
        fs::is_regular_file(fonts / "Roboto-Medium.ttf");
    std::cout << "RingOut C++ launcher self-test"
              << " runner=" << runner.string() << " port=" << port.string()
              << " rollback_ready="
              << (NetPlay::IsLiveRollbackProductionReady() ? "yes" : "no")
              << '\n';
    return valid ? 0 : 1;
  }

  const fs::path user_directory =
      user_directory_override.value_or(DefaultUserDirectory());
  if (extract_only) {
    ExtractionState extraction;
    extraction.running = true;
    const bool success =
        ExtractDisc(*extract_only, user_directory, &extraction);
    std::lock_guard lock(extraction.mutex);
    if (!success)
      std::cerr << "extraction failed: " << extraction.error << '\n';
    else
      std::cout << extraction.status << ": " << *extraction.finished_game
                << '\n';
    return success ? 0 : 1;
  }

  auto config = moderngekko::frontend::LoadConfig(user_directory, true);
  if (!config) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "Invalid " MODERNGEKKO_FRONTEND_NAME " config.ini",
                             config.error.c_str(), nullptr);
    return 2;
  }

#if defined(__linux__)
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, use_wayland ? "wayland" : "x11");
#endif
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    return 1;

  const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  SDL_Window *window = SDL_CreateWindow(
      MODERNGEKKO_FRONTEND_NAME, static_cast<int>(1080 * scale),
      static_cast<int>(720 * scale),
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_SetWindowMinimumSize(window, static_cast<int>(900 * scale),
                           static_cast<int>(600 * scale));
  SDL_Renderer *renderer = SDL_CreateRenderer(window, "vulkan");
  if (!renderer)
    renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.IniFilename = nullptr;
  LauncherFonts fonts;
  const fs::path font_directory = SiblingAsset(argv[0], "fonts");
  fonts.body = io.Fonts->AddFontFromFileTTF(
      (font_directory / "DroidSans.ttf").string().c_str(), 17.0f);
  if (!fonts.body)
    fonts.body = io.Fonts->AddFontDefault();
  fonts.heading = io.Fonts->AddFontFromFileTTF(
      (font_directory / "Roboto-Medium.ttf").string().c_str(), 24.0f);
  fonts.brand = io.Fonts->AddFontFromFileTTF(
      (font_directory / "Roboto-Medium.ttf").string().c_str(), 20.0f);
  if (!fonts.heading)
    fonts.heading = fonts.body;
  if (!fonts.brand)
    fonts.brand = fonts.body;
  io.FontDefault = fonts.body;
  ConfigureLauncherStyle(scale);
  ImGui::GetStyle().FontScaleDpi = scale;
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  std::vector<fs::path> images = FindDiscImages();
  std::optional<fs::path> selected_image =
      images.empty() ? std::nullopt : std::optional(images[0]);
  fs::path current_game = ReadDefaultGame(user_directory);
  auto current_metadata = moderngekko::InspectGame(current_game);
  std::string startup_game_error;
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (current_metadata &&
      current_metadata.metadata->disc_id != MODERNGEKKO_REQUIRED_DISC_ID) {
    startup_game_error =
        "The saved game is not the supported USA release. Choose a Soulcalibur "
        "II (USA) disc image to continue.";
    current_game.clear();
    current_metadata = moderngekko::InspectGame(current_game);
  }
#endif
  std::optional<fs::path> active_module;
  if (current_metadata)
    active_module = FindInstalledModule(argv[0], user_directory,
                                        current_metadata.metadata->disc_id,
                                        current_metadata.metadata->dol_sha256);
  const auto &resolutions = moderngekko::frontend::SupportedResolutions();
  bool show_fps_in_title = config.show_fps_in_title;
  std::array<char, 31> netplay_nickname{};
  std::array<char, 256> netplay_address{};
  std::array<char, 16> netplay_room_code{};
  std::snprintf(netplay_nickname.data(), netplay_nickname.size(), "%s",
                config.netplay_nickname.c_str());
  std::snprintf(netplay_address.data(), netplay_address.size(), "%s",
                config.netplay_address.c_str());
  int netplay_port = config.netplay_port;
  bool automatic_buffer = config.netplay_buffer == "auto";
  int manual_buffer = automatic_buffer ? 5 : std::stoi(config.netplay_buffer);
  const bool rollback_production_ready =
      NetPlay::IsLiveRollbackProductionReady();
  int netplay_mode =
      config.netplay_mode == moderngekko::frontend::NetplayMode::Rollback ? 1
                                                                          : 0;
  int netplay_direct_mode = netplay_mode;
  bool netplay_direct_advanced = false;
  int resolution_index = 0;
  for (std::size_t i = 0; i < resolutions.size(); ++i) {
    if (config.resolution == resolutions[i].text)
      resolution_index = static_cast<int>(i);
  }

  DialogState dialog;
  dialog.error = std::move(startup_game_error);
  std::vector<ControllerOption> controllers = EnumerateControllers();
  bool controller_profile_exists =
      moderngekko::frontend::GCPadConfigExists(user_directory);
  std::vector<std::string> configured_controllers = config.controllers;
  std::string selected_controller = configured_controllers.empty()
                                        ? config.controller
                                        : configured_controllers.front();
  int controller_index = FindController(controllers, selected_controller);
  std::string controller_status;
  const auto select_controller = [&](int index) {
    std::string message;
    if (!moderngekko::frontend::WriteGamepadGCPadConfig(
            user_directory, controllers[index].device, &message)) {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(message);
      return false;
    }
    std::string error;
    if (!moderngekko::frontend::SaveConfig(
            user_directory, resolutions[resolution_index].text,
            show_fps_in_title, controllers[index].device, &error)) {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(error);
      return false;
    }
    selected_controller = controllers[index].device;
    configured_controllers = {selected_controller};
    controller_profile_exists = true;
    controller_status = std::move(message);
    return true;
  };
  const auto ensure_controller = [&] {
    if (moderngekko::frontend::GCPadConfigExists(user_directory)) {
      controller_profile_exists = true;
      controller_status = selected_controller.empty()
                              ? "Keyboard profile ready"
                              : "GameCube controller profile ready";
      return true;
    }
    std::string message;
    const std::vector<std::string> devices =
        controller_index >= 0
            ? std::vector<std::string>{controllers[controller_index].device}
            : std::vector<std::string>{};
    if (!moderngekko::frontend::EnsureControllerConfig(user_directory, devices,
                                                       &message)) {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(message);
      return false;
    }
    controller_profile_exists = true;
    controller_status = std::move(message);
    return true;
  };
  const auto refresh_controllers = [&] {
    controllers = EnumerateControllers();
    controller_index = FindController(controllers, selected_controller);
    if (!controller_profile_exists) {
      if (controller_index < 0 && !controllers.empty()) {
        controller_index = 0;
        selected_controller = controllers.front().device;
      }
      if (controller_index >= 0)
        select_controller(controller_index);
      else
        controller_status = "No SDL gamepad detected";
    } else {
      controller_status = selected_controller.empty()
                              ? "Keyboard profile ready"
                              : "GameCube controller profile ready";
    }
  };
  refresh_controllers();
  ExtractionState extraction;
  std::jthread extraction_thread;
  bool rebuild_after_extraction = false;
  ModuleBuildState module_build;
  std::jthread module_thread;
  LauncherPage page = current_metadata && active_module ? LauncherPage::Play
                                                        : LauncherPage::Setup;
  enum class LaunchMode {
    None,
    Solo,
    Host,
    Join,
  };
  const auto start_module_build = [&](bool force_rebuild = false) {
    if (!current_metadata || module_build.running)
      return;
    if (module_thread.joinable())
      module_thread.join();
    {
      std::lock_guard lock(module_build.mutex);
      module_build.status = "Starting the setup helper";
      module_build.error.clear();
      module_build.log.clear();
      module_build.phase_scan.clear();
      module_build.phase = 0;
      module_build.finished_module.reset();
    }
    module_build.running = true;
    const fs::path game = current_game;
    const std::string disc_id = current_metadata.metadata->disc_id;
    const std::string dol_sha256 = current_metadata.metadata->dol_sha256;
    module_thread =
        std::jthread([argv0 = std::string(argv[0]), game, user_directory,
                      disc_id, dol_sha256, force_rebuild, &module_build] {
          BuildModule(argv0.c_str(), game, user_directory, disc_id, dol_sha256,
                      force_rebuild, &module_build);
        });
  };
  const auto save_netplay = [&] {
    config.netplay_nickname = netplay_nickname.data();
    config.netplay_address = netplay_address.data();
    config.netplay_port = static_cast<std::uint16_t>(netplay_port);
    config.netplay_buffer =
        automatic_buffer ? "auto" : std::to_string(manual_buffer);
    config.netplay_mode =
        netplay_mode == 1 ? moderngekko::frontend::NetplayMode::Rollback
                          : moderngekko::frontend::NetplayMode::FixedDelay;
    config.controllers = configured_controllers;
    if (config.controllers.empty() && !selected_controller.empty())
      config.controllers.push_back(selected_controller);
    config.controller =
        config.controllers.empty() ? std::string{} : config.controllers.front();
    config.resolution = resolutions[resolution_index].text;
    config.show_fps_in_title = show_fps_in_title;
    std::string error;
    if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
      return true;
    std::lock_guard lock(dialog.mutex);
    dialog.error = std::move(error);
    return false;
  };
  bool done = false;
  LaunchMode launch_mode = LaunchMode::None;
  while (!done) {
    bool controllers_changed = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT ||
          event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if (extraction.running || module_build.running) {
          std::lock_guard lock(dialog.mutex);
          dialog.error = "Game preparation is still running. Keep RingOut open "
                         "until it finishes.";
        } else
          done = true;
      }
      if (event.type == SDL_EVENT_JOYSTICK_ADDED ||
          event.type == SDL_EVENT_JOYSTICK_REMOVED ||
          event.type == SDL_EVENT_GAMEPAD_REMAPPED) {
        controllers_changed = true;
      }
      if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
          (event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ||
           event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
        constexpr int page_count = 5;
        const int direction =
            event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ? -1 : 1;
        page = static_cast<LauncherPage>(
            (static_cast<int>(page) + direction + page_count) % page_count);
      }
    }
    if (controllers_changed)
      refresh_controllers();

    {
      std::lock_guard lock(dialog.mutex);
      if (dialog.selected) {
        selected_image = std::move(dialog.selected);
        dialog.selected.reset();
        dialog.error.clear();
        page = LauncherPage::Setup;
      }
    }
    std::optional<fs::path> extracted_game;
    {
      std::lock_guard lock(extraction.mutex);
      if (extraction.finished_game) {
        extracted_game = *extraction.finished_game;
        extraction.finished_game.reset();
      }
    }
    if (extracted_game) {
      const bool force_rebuild = rebuild_after_extraction;
      rebuild_after_extraction = false;
      current_game = *extracted_game;
      current_metadata = moderngekko::InspectGame(current_game);
      active_module.reset();
      if (current_metadata && !force_rebuild)
        active_module = FindInstalledModule(
            argv[0], user_directory, current_metadata.metadata->disc_id,
            current_metadata.metadata->dol_sha256);
      if (!active_module)
        start_module_build(force_rebuild);
    }
    {
      std::lock_guard lock(module_build.mutex);
      if (module_build.finished_module) {
        active_module = std::move(module_build.finished_module);
        module_build.finished_module.reset();
        page = LauncherPage::Play;
      }
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(MODERNGEKKO_FRONTEND_NAME " Launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    const float navigation_width = 196.0f * scale;
    ImGui::BeginChild("##navigation", ImVec2(navigation_width, 0.0f), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(22.0f * scale, 28.0f * scale));
    ImGui::PushFont(fonts.brand);
    ImGui::TextUnformatted("RingOut");
    ImGui::PopFont();
    ImGui::SetCursorPosY(86.0f * scale);
    const float nav_button_width = navigation_width - 32.0f * scale;
    ImGui::SetCursorPosX(16.0f * scale);
    if (NavigationButton("Play", LauncherPage::Play, page, nav_button_width,
                         scale))
      page = LauncherPage::Play;
    ImGui::SetCursorPosX(16.0f * scale);
    if (NavigationButton("Game files", LauncherPage::Setup, page,
                         nav_button_width, scale))
      page = LauncherPage::Setup;
    ImGui::SetCursorPosX(16.0f * scale);
    if (NavigationButton("Netplay", LauncherPage::Netplay, page,
                         nav_button_width, scale))
      page = LauncherPage::Netplay;
    ImGui::SetCursorPosX(16.0f * scale);
    if (NavigationButton("Mods", LauncherPage::Mods, page, nav_button_width,
                         scale))
      page = LauncherPage::Mods;
    ImGui::SetCursorPosX(16.0f * scale);
    if (NavigationButton("Settings", LauncherPage::Settings, page,
                         nav_button_width, scale))
      page = LauncherPage::Settings;
    ImGui::SetCursorPos(
        ImVec2(22.0f * scale, ImGui::GetWindowHeight() - 46.0f * scale));
    StatusLine(active_module ? "Ready" : "Setup required",
               active_module ? ImVec4(0.32f, 0.65f, 0.45f, 1.0f)
                             : ImVec4(0.85f, 0.64f, 0.27f, 1.0f),
               scale, !active_module);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(0.075f, 0.083f, 0.102f, 1.0f));
    ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f), false);
    ImGui::SetCursorPos(ImVec2(32.0f * scale, 30.0f * scale));
    ImGui::BeginGroup();
    const float content_width =
        ImGui::GetContentRegionAvail().x - 32.0f * scale;

    if (page == LauncherPage::Play) {
      SectionHeading(fonts.heading, "Play",
                     "Launch the game or start a netplay session.",
                     content_width, scale);
      ImGui::PushFont(fonts.brand);
      ImGui::TextUnformatted("Soulcalibur II");
      ImGui::PopFont();
      if (active_module) {
        StatusLine("Ready", ImVec4(0.32f, 0.65f, 0.45f, 1.0f), scale);
      } else if (current_metadata) {
        StatusLine("Setup incomplete", ImVec4(0.85f, 0.64f, 0.27f, 1.0f),
                   scale, true);
        ImGui::TextDisabled("Finish setup before playing.");
      } else {
        StatusLine("Game files needed", ImVec4(0.85f, 0.64f, 0.27f, 1.0f),
                   scale, true);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width -
                               16.0f * scale);
        ImGui::TextDisabled(
            "Choose your Soulcalibur II (USA) disc image once to create the "
            "local game files RingOut needs.");
        ImGui::PopTextWrapPos();
      }
      ImGui::Dummy(ImVec2(0.0f, 14.0f * scale));
      if (active_module) {
        if (PrimaryButton("Play", ImVec2(160.0f * scale, 44.0f * scale)) &&
            ensure_controller()) {
          launch_mode = LaunchMode::Solo;
          done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Netplay", ImVec2(140.0f * scale, 44.0f * scale)))
          page = LauncherPage::Netplay;
      } else if (PrimaryButton("Set up game",
                               ImVec2(160.0f * scale, 44.0f * scale)))
        page = LauncherPage::Setup;
    } else if (page == LauncherPage::Setup) {
      SectionHeading(fonts.heading, "Game files",
                     "Choose a Soulcalibur II (USA) ISO, RVZ, or WBFS file. "
                     "RingOut keeps one local set of game files.",
                     content_width, scale);
      const bool preparing = extraction.running || module_build.running;
      ImGui::TextUnformatted("Disc image");
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
      if (selected_image) {
        ImGui::TextDisabled("%s", selected_image->string().c_str());
      } else if (active_module) {
        ImGui::TextDisabled(
            "Game files are configured. Choose another disc image only to "
            "replace them.");
      } else {
        ImGui::TextDisabled("No disc image selected");
      }
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));
      ImGui::BeginDisabled(preparing);
      if (ImGui::Button("Choose disc image",
                        ImVec2(180.0f * scale, 40.0f * scale))) {
        static constexpr SDL_DialogFileFilter filters[] = {
            {"GameCube disc images", "iso;rvz;wbfs"},
            {"Plain ISO", "iso"},
            {"Dolphin RVZ", "rvz"},
            {"WBFS", "wbfs"}};
        const std::string documents = DocumentsDirectory().string();
        SDL_ShowOpenFileDialog(FileDialogCallback, &dialog, window, filters,
                               static_cast<int>(std::size(filters)),
                               documents.c_str(), false);
      }
      if (selected_image) {
        ImGui::SameLine();
        const char *setup_action = active_module      ? "Replace game files"
                                   : current_metadata ? "Restart setup"
                                                      : "Set up game";
        if (PrimaryButton(setup_action,
                          ImVec2(170.0f * scale, 40.0f * scale))) {
          if (extraction_thread.joinable())
            extraction_thread.join();
          extraction.completed = 0;
          extraction.total = 1;
          extraction.running = true;
          {
            std::lock_guard lock(extraction.mutex);
            extraction.error.clear();
            extraction.status = "Opening the disc image";
            extraction.finished_game.reset();
          }
          const fs::path image = *selected_image;
          const bool replace_existing = static_cast<bool>(current_metadata);
          rebuild_after_extraction = replace_existing;
          extraction_thread =
              std::jthread([image, user_directory, replace_existing,
                            &extraction] {
                ExtractDisc(image, user_directory, &extraction,
                            replace_existing);
              });
        }
      }
      if (current_metadata && !active_module) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));
        if (PrimaryButton("Continue setup",
                          ImVec2(150.0f * scale, 40.0f * scale)))
          start_module_build();
      }
      ImGui::EndDisabled();

      ImGui::Dummy(ImVec2(0.0f, 10.0f * scale));
      ImGui::Separator();
      ImGui::Dummy(ImVec2(0.0f, 10.0f * scale));
      ImGui::TextUnformatted("Setup status");
      ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
      std::string build_log;
      std::string build_status;
      std::string build_error;
      int build_phase = 0;
      {
        std::lock_guard lock(module_build.mutex);
        build_log = module_build.log;
        build_status = module_build.status;
        build_error = module_build.error;
        build_phase = module_build.phase;
      }
      const std::array<const char *, 5> phases = {
          "Verify disc", "Extract files", "Translate game code", "Build game",
          "Finish setup"};
      int active_phase = extraction.running ? 0
                         : module_build.running
                             ? std::clamp(build_phase - 1, 1, 4)
                         : active_module    ? 5
                         : current_metadata ? 1
                                            : 0;
      for (std::size_t i = 0; i < phases.size(); ++i) {
        const bool complete = active_phase > static_cast<int>(i);
        const bool current = active_phase == static_cast<int>(i) && preparing;
        StatusLine(phases[i],
                   complete  ? ImVec4(0.32f, 0.65f, 0.45f, 1.0f)
                   : current ? ImVec4(0.36f, 0.58f, 0.93f, 1.0f)
                             : ImVec4(0.35f, 0.38f, 0.43f, 1.0f),
                   scale, !complete && !current);
      }
      if (extraction.running) {
        const float progress =
            std::min(1.0f, static_cast<float>(extraction.completed.load()) /
                               extraction.total.load());
        ImGui::ProgressBar(progress, ImVec2(content_width, 0.0f));
      } else if (module_build.running)
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()),
                           ImVec2(content_width, 0.0f), build_status.c_str());
      {
        std::lock_guard lock(extraction.mutex);
        if (!extraction.status.empty())
          ImGui::TextDisabled("%s", extraction.status.c_str());
        if (!extraction.error.empty())
          ImGui::TextColored(ImVec4(0.94f, 0.416f, 0.373f, 1.0f), "%s",
                             extraction.error.c_str());
      }
      if (!build_error.empty())
        ImGui::TextColored(ImVec4(0.94f, 0.416f, 0.373f, 1.0f), "%s",
                           build_error.c_str());
      if (!build_log.empty() && ImGui::CollapsingHeader("Technical log")) {
        ImGui::BeginChild("##setup-log", ImVec2(content_width, 145.0f * scale),
                          true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(build_log.c_str());
        if (module_build.running)
          ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::TextDisabled(
            "Log: %s",
            (user_directory / "Logs" / "setup.log").string().c_str());
      }
    } else if (page == LauncherPage::Netplay) {
      SectionHeading(fonts.heading, "Netplay",
                     "Create or join an Online Room with a short code. Both "
                     "players need the same RingOut build and game files.",
                     content_width, scale);
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
      ImGui::TextDisabled(
          "Beta rooms use Dolphin's hosted rendezvous service, then connect "
          "players directly. L1 and R1 switch pages with a controller.");
      ImGui::PopTextWrapPos();
      if (!active_module) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));
        StatusLine("Complete game setup before using netplay",
                   ImVec4(0.85f, 0.64f, 0.27f, 1.0f), scale, true);
      }
      ImGui::Dummy(ImVec2(0.0f, 6.0f * scale));
      ImGui::Checkbox("Advanced: use Direct IP", &netplay_direct_advanced);
      if (netplay_direct_advanced) {
        ImGui::TextUnformatted("Network mode");
        ImGui::RadioButton("Fixed delay", &netplay_direct_mode, 0);
        if (!rollback_production_ready)
          ImGui::BeginDisabled();
        ImGui::RadioButton("Rollback", &netplay_direct_mode, 1);
        if (!rollback_production_ready)
          ImGui::EndDisabled();
        netplay_mode = netplay_direct_mode;
      } else {
        netplay_mode = 1;
        ImGui::TextUnformatted("Mode: Rollback");
      }
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
      if (rollback_production_ready) {
        ImGui::TextDisabled(
            "Rollback predicts late remote input and corrects it by restoring "
            "and replaying emulator state. RingOut rejects incompatible peers "
            "instead of silently falling back.");
      } else {
        ImGui::TextDisabled(
            "Experimental rollback is unavailable in this build until the "
            "runtime's production output-safety gate is complete. Fixed delay "
            "remains available.");
      }
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0.0f, 6.0f * scale));
      ImGui::TextUnformatted("Nickname");
      ImGui::SetNextItemWidth(300.0f * scale);
      ImGui::InputText("##nickname", netplay_nickname.data(),
                       netplay_nickname.size());
      if (netplay_direct_advanced) {
        ImGui::TextUnformatted("Host name or IPv4 address");
        ImGui::SetNextItemWidth(300.0f * scale);
        ImGui::InputText("##address", netplay_address.data(),
                         netplay_address.size());
        ImGui::TextUnformatted("UDP port");
        ImGui::SetNextItemWidth(145.0f * scale);
        ImGui::InputInt("##port", &netplay_port);
        netplay_port = std::clamp(netplay_port, 1, 65535);
      } else {
        ImGui::TextUnformatted("Room code (needed only to Join)");
        ImGui::SetNextItemWidth(220.0f * scale);
        ImGui::InputText("##room-code", netplay_room_code.data(),
                         netplay_room_code.size(),
                         ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_CharsUppercase);
      }
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
      if (netplay_direct_advanced) {
        ImGui::TextDisabled(
            "Direct UDP is for a trusted LAN or private VPN and may require "
            "manual port forwarding.");
      } else {
        ImGui::TextDisabled(
            "Trusted-friends beta: no relay, authentication, encryption, or "
            "IP hiding. Strict NATs may fail and your opponent learns your "
            "IP.");
      }
      ImGui::PopTextWrapPos();
      ImGui::Checkbox(netplay_mode == 1 ? "Use recommended base delay"
                                        : "Use default input delay",
                      &automatic_buffer);
      if (automatic_buffer) {
        ImGui::TextDisabled(netplay_mode == 1
                                ? "Rollback base delay: 2 SI samples."
                                : "Fixed-delay input delay: 5 SI samples.");
      }
      if (!automatic_buffer) {
        ImGui::TextUnformatted(netplay_mode == 1
                                   ? "Rollback base delay (SI samples)"
                                   : "Input delay (SI samples)");
        ImGui::SetNextItemWidth(250.0f * scale);
        ImGui::SliderInt("##buffer-samples", &manual_buffer, 1, 20);
        ImGui::TextDisabled("Typically about %.1f ms at 120 SI polls/second.",
                            manual_buffer * 1000.0 / 120.0);
      }
      ImGui::Dummy(ImVec2(0.0f, 6.0f * scale));
      const bool selected_mode_available =
          moderngekko::frontend::IsPlayerUsableNetplayMode(
              netplay_mode == 1
                  ? moderngekko::frontend::NetplayMode::Rollback
                  : moderngekko::frontend::NetplayMode::FixedDelay,
              rollback_production_ready);
      ImGui::BeginDisabled(!active_module || !selected_mode_available);
      if (PrimaryButton(netplay_direct_advanced ? "Host direct"
                                                : "Host online room",
                        ImVec2(190.0f * scale, 42.0f * scale)) &&
          ensure_controller() && save_netplay()) {
        launch_mode = LaunchMode::Host;
        done = true;
      }
      ImGui::SameLine();
      if (ImGui::Button(netplay_direct_advanced ? "Join direct"
                                                : "Join online room",
                        ImVec2(190.0f * scale, 42.0f * scale))) {
        const auto normalized = moderngekko::frontend::NormalizeNetplayRoomCode(
            netplay_room_code.data());
        if (!netplay_direct_advanced && !normalized) {
          std::lock_guard lock(dialog.mutex);
          dialog.error =
              "Room codes contain exactly eight hexadecimal characters.";
        } else if (ensure_controller() && save_netplay()) {
          if (normalized)
            std::snprintf(netplay_room_code.data(), netplay_room_code.size(),
                          "%s", normalized->c_str());
          launch_mode = LaunchMode::Join;
          done = true;
        }
      }
      ImGui::EndDisabled();
      ImGui::Dummy(ImVec2(0.0f, 12.0f * scale));
      ImGui::Separator();
      ImGui::Dummy(ImVec2(0.0f, 10.0f * scale));
      ImGui::TextUnformatted("Status");
      const char *netplay_status =
          !active_module
              ? "Game setup required"
              : (!selected_mode_available
                     ? "Rollback safety gate incomplete — choose fixed delay"
                     : (netplay_mode == 1 ? "Ready for experimental rollback"
                                          : "Ready for fixed-delay netplay"));
      StatusLine(netplay_status,
                 active_module && selected_mode_available
                     ? ImVec4(0.32f, 0.65f, 0.45f, 1.0f)
                     : ImVec4(0.85f, 0.64f, 0.27f, 1.0f),
                 scale, !active_module || !selected_mode_available);
    } else if (page == LauncherPage::Mods) {
      SectionHeading(fonts.heading, "Mods",
                     "Install, enable, and update supported mods.",
                     content_width, scale);
      ImGui::PushFont(fonts.brand);
      ImGui::TextUnformatted("Mod management is not available yet");
      ImGui::PopFont();
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
      ImGui::TextDisabled(
          "The game does not currently provide the compatibility and load "
          "order information needed for safe mod installation.");
      ImGui::Dummy(ImVec2(0.0f, 8.0f * scale));
      ImGui::TextDisabled(
          "Dolphin-format texture packs can still be installed manually.");
      ImGui::PopTextWrapPos();
    } else if (page == LauncherPage::Settings) {
      SectionHeading(fonts.heading, "Settings",
                     "Change video, controller, and file settings.",
                     content_width, scale);
      ImGui::TextUnformatted("Video");
      ImGui::TextUnformatted("Internal resolution");
      ImGui::SetNextItemWidth(260.0f * scale);
      if (ImGui::BeginCombo("##internal-resolution",
                            resolutions[resolution_index].text)) {
        for (std::size_t i = 0; i < resolutions.size(); ++i) {
          const bool selected = resolution_index == static_cast<int>(i);
          if (ImGui::Selectable(resolutions[i].text, selected)) {
            std::string error;
            if (moderngekko::frontend::SaveConfig(
                    user_directory, resolutions[i].text, show_fps_in_title,
                    selected_controller, &error))
              resolution_index = static_cast<int>(i);
            else {
              std::lock_guard lock(dialog.mutex);
              dialog.error = std::move(error);
            }
          }
        }
        ImGui::EndCombo();
      }
      const bool previous_show_fps = show_fps_in_title;
      if (ImGui::Checkbox("Show FPS in the window title", &show_fps_in_title)) {
        std::string error;
        if (!moderngekko::frontend::SaveConfig(
                user_directory, resolutions[resolution_index].text,
                show_fps_in_title, selected_controller, &error)) {
          show_fps_in_title = previous_show_fps;
          std::lock_guard lock(dialog.mutex);
          dialog.error = std::move(error);
        }
      }
      ImGui::Dummy(ImVec2(0.0f, 10.0f * scale));
      ImGui::Separator();
      ImGui::Dummy(ImVec2(0.0f, 10.0f * scale));
      ImGui::TextUnformatted("Controller");
      const char *controller_preview =
          controller_index >= 0 ? controllers[controller_index].label.c_str()
          : selected_controller.empty() ? "Keyboard fallback"
                                        : selected_controller.c_str();
      ImGui::TextUnformatted("Input device");
      ImGui::SetNextItemWidth(360.0f * scale);
      if (ImGui::BeginCombo("##gamecube-controller", controller_preview)) {
        for (std::size_t i = 0; i < controllers.size(); ++i) {
          const bool selected = controller_index == static_cast<int>(i);
          if (ImGui::Selectable(controllers[i].label.c_str(), selected)) {
            controller_index = static_cast<int>(i);
            selected_controller = controllers[i].device;
            controller_status =
                "Ready to write the GameCube controller profile";
          }
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::BeginDisabled(controller_index < 0);
      if (ImGui::Button("Use this controller"))
        select_controller(controller_index);
      ImGui::EndDisabled();
      ImGui::TextDisabled("%s", controller_status.c_str());
      ImGui::Dummy(ImVec2(0.0f, 10.0f * scale));
      ImGui::Separator();
      ImGui::Dummy(ImVec2(0.0f, 10.0f * scale));
      ImGui::TextUnformatted("Files");
      ImGui::TextWrapped("User data: %s", user_directory.string().c_str());
      ImGui::TextWrapped(
          "Setup log: %s",
          (user_directory / "Logs" / "setup.log").string().c_str());
      ImGui::TextWrapped(
          "Game log: %s",
          (user_directory / "Logs" / "RingOut.log").string().c_str());
    }

    ImGui::EndGroup();
    {
      std::lock_guard lock(dialog.mutex);
      if (!dialog.error.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
        ImGui::TextColored(ImVec4(0.86f, 0.36f, 0.36f, 1.0f), "%s",
                           dialog.error.c_str());
        ImGui::PopTextWrapPos();
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();

    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 17, 20, 27, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  if (extraction_thread.joinable())
    extraction_thread.join();
  if (module_thread.joinable())
    module_thread.join();

  int result = 0;
  if (launch_mode != LaunchMode::None) {
    std::string launch_error;
    if (!WriteDefaultGame(user_directory, current_game, &launch_error)) {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed",
                               launch_error.c_str(), window);
      result = 1;
    } else {
      std::vector<std::string> argument_storage = {
          SiblingRunner(argv[0]).string(), "--game", current_game.string(),
          "--user-dir", user_directory.string()};
      if (active_module) {
        argument_storage.emplace_back("--module");
        argument_storage.emplace_back(active_module->string());
      }
      if (launch_mode == LaunchMode::Host)
        argument_storage.emplace_back("--netplay-host");
      else if (launch_mode == LaunchMode::Join) {
        argument_storage.emplace_back("--netplay-join");
        argument_storage.emplace_back(
            netplay_direct_advanced ? config.netplay_address
                                    : std::string(netplay_room_code.data()));
      }
      if (launch_mode == LaunchMode::Host || launch_mode == LaunchMode::Join) {
        if (!netplay_direct_advanced)
          argument_storage.emplace_back("--netplay-traversal");
        argument_storage.emplace_back("--netplay-port");
        argument_storage.emplace_back(std::to_string(config.netplay_port));
        argument_storage.emplace_back("--nickname");
        argument_storage.emplace_back(config.netplay_nickname);
        argument_storage.emplace_back("--buffer");
        argument_storage.emplace_back(config.netplay_buffer);
        argument_storage.emplace_back("--netplay-mode");
        argument_storage.emplace_back(std::string(
            moderngekko::frontend::NetplayModeConfigValue(
                config.netplay_mode)));
        for (const std::string &controller : config.controllers) {
          argument_storage.emplace_back("--controller");
          argument_storage.emplace_back(controller);
        }
      }
      if (use_wayland)
        argument_storage.emplace_back("--wayland");
      std::vector<const char *> arguments;
      arguments.reserve(argument_storage.size() + 1);
      for (const std::string &argument : argument_storage)
        arguments.push_back(argument.c_str());
      arguments.push_back(nullptr);
      const std::filesystem::path log_path =
          user_directory / "Logs" / "RingOut.log";
      std::error_code log_error;
      std::filesystem::create_directories(log_path.parent_path(), log_error);
      SDL_IOStream *log_stream =
          log_error ? nullptr : SDL_IOFromFile(log_path.string().c_str(), "w");
      SDL_Process *process = nullptr;
      std::string process_error;
      if (log_stream) {
        const SDL_PropertiesID properties = SDL_CreateProperties();
        if (properties) {
          SDL_SetPointerProperty(properties,
                                 SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                                 arguments.data());
          SDL_SetNumberProperty(properties,
                                SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                                SDL_PROCESS_STDIO_REDIRECT);
          SDL_SetPointerProperty(
              properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log_stream);
          SDL_SetBooleanProperty(
              properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN,
              true);
          process = SDL_CreateProcessWithProperties(properties);
          SDL_DestroyProperties(properties);
        }
        if (!process)
          process_error = SDL_GetError();
        SDL_CloseIO(log_stream);
      } else {
        process = SDL_CreateProcess(arguments.data(), false);
        if (!process)
          process_error = SDL_GetError();
      }
      if (!process) {
        launch_error = "Could not start " + SiblingRunner(argv[0]).string() +
                       ": " + process_error;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed",
                                 launch_error.c_str(), window);
        result = 1;
      } else {
        SDL_HideWindow(window);
        int exit_code = 1;
        const bool waited = SDL_WaitProcess(process, true, &exit_code);
        SDL_DestroyProcess(process);
        if (!waited || exit_code != 0) {
          SDL_ShowWindow(window);
          if (!waited) {
            launch_error = "The game process could not be monitored: " +
                           std::string(SDL_GetError());
          } else if (exit_code == static_cast<int>(
                                      moderngekko::frontend::NetplayExitCode::
                                          RollbackUnavailable)) {
            launch_error =
                "Rollback is not available in this build because its runtime "
                "safety gate is incomplete. Start a new Fixed delay session "
                "or install a matching rollback-capable release.";
          } else if (exit_code == static_cast<int>(
                                      moderngekko::frontend::NetplayExitCode::
                                          RollbackStartRejected)) {
            launch_error =
                "Rollback could not start because the synchronized session "
                "did not meet its safety policy. RingOut supports folder "
                "memory cards or no card for rollback; raw memory-card images "
                "are refused. Check the log for the exact policy failure.";
          } else if (exit_code == static_cast<int>(
                                      moderngekko::frontend::NetplayExitCode::
                                          SessionDesynced)) {
            launch_error =
                "The peers produced different confirmed game state, so "
                "RingOut stopped the match instead of continuing desynchronized.";
          } else if (exit_code == static_cast<int>(
                                      moderngekko::frontend::NetplayExitCode::
                                          SessionRuntimeFailed)) {
            launch_error =
                "The synchronized game runtime failed. The session was stopped "
                "safely; check the log for the failing subsystem.";
          } else if (exit_code == static_cast<int>(
                                      moderngekko::frontend::NetplayExitCode::
                                          SessionConnectionLost)) {
            launch_error =
                "The connection to the other player was lost and the match "
                "was stopped.";
          } else if (exit_code ==
                     static_cast<int>(moderngekko::frontend::NetplayExitCode::
                                          TraversalServiceUnavailable)) {
            launch_error =
                "Dolphin's hosted room service did not respond. Try again "
                "later, or use Advanced Direct IP on a trusted LAN/VPN.";
          } else if (exit_code ==
                     static_cast<int>(moderngekko::frontend::NetplayExitCode::
                                          InvalidRoomCode)) {
            launch_error =
                "That room code is invalid, expired, or no longer registered.";
          } else if (exit_code ==
                     static_cast<int>(moderngekko::frontend::NetplayExitCode::
                                          TraversalFailed)) {
            launch_error =
                "The room was found, but direct peer-to-peer traversal failed. "
                "A strict NAT or firewall may be blocking it; this beta has no "
                "relay fallback.";
          } else if (launch_mode == LaunchMode::Join) {
            switch (static_cast<moderngekko::frontend::NetplayExitCode>(
                exit_code)) {
            case moderngekko::frontend::NetplayExitCode::VersionMismatch:
              launch_error =
                  "The host is running an incompatible netplay build. Both "
                  "players must use the same release.";
              break;
            case moderngekko::frontend::NetplayExitCode::CompatibilityMismatch:
              launch_error =
                  "The extracted game or recomp module does not match the "
                  "host. Both players need the same game revision and "
                  "release.";
              break;
            case moderngekko::frontend::NetplayExitCode::RoomFull:
              launch_error = "All four controller slots are already occupied.";
              break;
            case moderngekko::frontend::NetplayExitCode::GameRunning:
              launch_error = "The host has already started the game.";
              break;
            case moderngekko::frontend::NetplayExitCode::ServerFull:
              launch_error = "The netplay server is full.";
              break;
            case moderngekko::frontend::NetplayExitCode::NicknameRejected:
              launch_error = "The nickname was rejected by the host.";
              break;
            default:
              launch_error =
                  netplay_direct_advanced
                      ? "Could not reach the netplay host. Check the "
                        "host name, UDP port, and firewall."
                      : "Could not connect to that online room. "
                        "Check the code and ask the host to keep the "
                        "lobby open.";
              break;
            }
          } else if (launch_mode == LaunchMode::Host) {
            launch_error = netplay_direct_advanced
                               ? "Could not create the netplay session. Check "
                                 "the UDP port and firewall settings."
                               : "Could not create the online room through "
                                 "Dolphin's hosted traversal service.";
          } else {
            launch_error = "The game process exited with code " +
                           std::to_string(exit_code) + ".";
          }
          if (!log_error)
            launch_error +=
                "\n\nDetails were written to:\n" + log_path.string();
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Session failed",
                                   launch_error.c_str(), window);
          result = 1;
        }
      }
    }
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
