#include "frontend_config.hpp"
#include "moderngekko/game.hpp"
#include "moderngekko/runtime.hpp"
#include "netplay_session.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
#ifndef MODERNGEKKO_RUNNER_NAME
#define MODERNGEKKO_RUNNER_NAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

volatile std::sig_atomic_t s_stop_requested = 0;

void HandleStopSignal(int) { s_stop_requested = 1; }

void Usage() {
  std::cerr << "usage: " MODERNGEKKO_RUNNER_NAME
               " [--game <extracted-root>] [--module <path>]\n"
               "       [--user-dir <path>] [--title <text>]\n"
               "       [--graphics <backend>] [--audio <backend>]\n"
               "       [--wayland] [-X11] [--headless] [--allow-interpreter]\n"
               "       [--widescreen]   (16:9; also Alt+W in-game)\n"
               "       [--netplay-host | --netplay-join <host>] "
               "[--netplay-port <port>] [--netplay-traversal]\n"
               "       [--traversal-server <host>] [--traversal-port <port>] "
               "[--traversal-alt-port <port>]\n"
               "       [--nickname <name>] [--buffer <auto|1-20>] "
               "[--netplay-mode <fixed-delay|rollback>] "
               "[--netplay-diagnostics] [--controller <device>]...\n"
               "       [--netplay-players <n>]   (host: machines to wait for, "
               "default 2)\n"
               "       [--netplay-timeout <s>]   (lobby wait, default 120)\n"
               "       [--keyboard <1|2>]        (rewrite the pad profile as "
               "keyboard;\n"
               "                                  1 = arrows+ZXCV, 2 = "
               "IJKL+BNM)\n"
               "       With no --game, boots the path in "
               "<user-dir>/default-game.txt.\n";
}

std::filesystem::path
ReadDefaultGame(const std::filesystem::path &user_directory) {
  std::ifstream file(user_directory / "default-game.txt");
  std::string path;
  std::getline(file, path);
  if (!path.empty() && path.back() == '\r')
    path.pop_back();
  return path;
}

std::filesystem::path DefaultUserDirectory() {
#if defined(_WIN32)
  if (const char *local_app_data = std::getenv("LOCALAPPDATA"))
    return std::filesystem::path(local_app_data) /
           MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char *xdg = std::getenv("XDG_DATA_HOME"))
    return std::filesystem::path(xdg) / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const char *home = std::getenv("HOME"))
    return std::filesystem::path(home) / ".local" / "share" /
           MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
}

std::string LibrarySuffix() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

std::filesystem::path ExecutableDirectory(const char *argv0) {
  std::error_code ec;
#if defined(__linux__)
  const std::filesystem::path proc_executable =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const std::filesystem::path executable =
      std::filesystem::weakly_canonical(argv0, ec);
  return ec ? std::filesystem::current_path() : executable.parent_path();
}
} // namespace

int RunMain(int argc, char **argv) {
  moderngekko::RuntimeConfig config;
  config.user_directory = DefaultUserDirectory();
#ifdef MODERNGEKKO_DEFAULT_WINDOW_TITLE
  config.window_title = MODERNGEKKO_DEFAULT_WINDOW_TITLE;
#endif
  std::filesystem::path module_path;
  std::optional<moderngekko::frontend::NetplayRole> netplay_role;
  std::string netplay_address;
  std::optional<std::uint16_t> netplay_port;
  bool netplay_traversal = false;
  std::string traversal_server;
  std::optional<std::uint16_t> traversal_port;
  std::optional<std::uint16_t> traversal_alt_port;
  std::string netplay_nickname;
  std::string netplay_buffer;
  std::optional<moderngekko::frontend::NetplayMode> netplay_mode;
  bool netplay_diagnostics = false;
  std::vector<std::string> netplay_controllers;
  std::optional<unsigned> netplay_players;
  std::optional<unsigned> netplay_timeout;
  std::optional<int> keyboard_layout;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&](const char *option) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << option << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--game")
      config.game_root = value("--game");
    else if (arg == "--module")
      module_path = value("--module");
    else if (arg == "--user-dir")
      config.user_directory = value("--user-dir");
    else if (arg == "--title")
      config.window_title = value("--title");
    else if (arg == "--graphics")
      config.graphics.backend = value("--graphics");
    else if (arg == "--audio")
      config.audio.backend = value("--audio");
    else if (arg == "-X11" || arg == "--x11")
      config.window_system = moderngekko::WindowSystem::X11;
    else if (arg == "--wayland")
      config.window_system = moderngekko::WindowSystem::Wayland;
    else if (arg == "--widescreen")
      config.graphics.widescreen = true;
    else if (arg == "--headless")
      config.headless = true;
    else if (arg == "--allow-interpreter")
      config.allow_interpreter = true;
    else if (arg == "--netplay-host") {
      if (netplay_role) {
        std::cerr << "choose exactly one of --netplay-host or --netplay-join\n";
        return 2;
      }
      netplay_role = moderngekko::frontend::NetplayRole::Host;
    } else if (arg == "--netplay-join") {
      if (netplay_role) {
        std::cerr << "choose exactly one of --netplay-host or --netplay-join\n";
        return 2;
      }
      netplay_role = moderngekko::frontend::NetplayRole::Join;
      netplay_address = value("--netplay-join");
    } else if (arg == "--netplay-port") {
      const std::string port_value = value("--netplay-port");
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          port_value.data(), port_value.data() + port_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != port_value.data() + port_value.size() || port == 0 ||
          port > 65535) {
        std::cerr << "--netplay-port must be between 1 and 65535\n";
        return 2;
      }
      netplay_port = static_cast<std::uint16_t>(port);
    } else if (arg == "--netplay-traversal") {
      netplay_traversal = true;
    } else if (arg == "--traversal-server") {
      traversal_server = value("--traversal-server");
    } else if (arg == "--traversal-port" || arg == "--traversal-alt-port") {
      const std::string port_value = value(arg.c_str());
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          port_value.data(), port_value.data() + port_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != port_value.data() + port_value.size() || port == 0 ||
          port > 65535) {
        std::cerr << arg << " must be between 1 and 65535\n";
        return 2;
      }
      if (arg == "--traversal-port")
        traversal_port = static_cast<std::uint16_t>(port);
      else
        traversal_alt_port = static_cast<std::uint16_t>(port);
    } else if (arg == "--nickname")
      netplay_nickname = value("--nickname");
    else if (arg == "--buffer")
      netplay_buffer = value("--buffer");
    else if (arg == "--netplay-mode") {
      moderngekko::frontend::NetplayMode parsed{};
      if (!moderngekko::frontend::ParseNetplayMode(value("--netplay-mode"),
                                                    &parsed)) {
        std::cerr << "--netplay-mode must be fixed-delay or rollback\n";
        return 2;
      }
      netplay_mode = parsed;
    } else if (arg == "--netplay-diagnostics") {
      netplay_diagnostics = true;
    } else if (arg == "--controller")
      netplay_controllers.emplace_back(value("--controller"));
    else if (arg == "--netplay-players") {
      const std::string players_value = value("--netplay-players");
      unsigned players = 0;
      const auto parsed = std::from_chars(
          players_value.data(), players_value.data() + players_value.size(),
          players);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != players_value.data() + players_value.size() ||
          players < 1 || players > 4) {
        std::cerr << "--netplay-players must be between 1 and 4\n";
        return 2;
      }
      netplay_players = players;
    } else if (arg == "--netplay-timeout") {
      const std::string timeout_value = value("--netplay-timeout");
      unsigned timeout = 0;
      const auto parsed = std::from_chars(
          timeout_value.data(), timeout_value.data() + timeout_value.size(),
          timeout);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != timeout_value.data() + timeout_value.size() ||
          timeout < 1) {
        std::cerr << "--netplay-timeout must be at least 1 second\n";
        return 2;
      }
      netplay_timeout = timeout;
    } else if (arg == "--keyboard") {
      const std::string layout_value = value("--keyboard");
      if (layout_value != "1" && layout_value != "2") {
        std::cerr << "--keyboard must be 1 or 2\n";
        return 2;
      }
      keyboard_layout = layout_value == "1" ? 1 : 2;
    }
    else if (arg == "--help" || arg == "-h") {
      Usage();
      return 0;
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      Usage();
      return 2;
    }
  }
  if (config.game_root.empty())
    config.game_root = ReadDefaultGame(config.user_directory);
  if (config.game_root.empty()) {
    std::cerr << "no game configured; use --game once or create "
              << (config.user_directory / "default-game.txt") << '\n';
    Usage();
    return 2;
  }

  auto frontend_config =
      moderngekko::frontend::LoadConfig(config.user_directory, true);
  if (!frontend_config) {
    std::cerr << "invalid config.ini: " << frontend_config.error << '\n';
    return 2;
  }
  config.graphics.internal_resolution_scale = frontend_config.dolphin_scale;
  config.show_fps_in_title = frontend_config.show_fps_in_title;

  // --keyboard rewrites the pad profile, so it overrides an existing one; the
  // implicit default below only ever fills in a missing profile. Layout 2 is a
  // disjoint key set, which is what lets two local netplay peers share one
  // keyboard -- unavoidable there, since input must work without focus and so
  // both instances see every key.
  if (keyboard_layout) {
    std::string keyboard_message;
    const auto layout = *keyboard_layout == 1
                            ? moderngekko::frontend::KeyboardLayout::Player1
                            : moderngekko::frontend::KeyboardLayout::Player2;
    if (!moderngekko::frontend::WriteKeyboardGCPadConfig(
            config.user_directory, layout, &keyboard_message)) {
      std::cerr << "keyboard configuration: " << keyboard_message << '\n';
      return 2;
    }
    std::cout << "controller: " << keyboard_message << '\n';
  }

  // Not gated on a controller already being configured. It used to be, so a
  // fresh user directory with no controller= line got no GCPadNew.ini at all
  // and the game read no input -- invisible on a desktop, where you would set a
  // controller or pass --keyboard, and fatal on a Steam Deck in Game Mode,
  // which has a pad, no keyboard, and no way to reach the CONTROLS tab without
  // input already working. With an empty list EnsureControllerConfig asks the
  // hardware and falls back to a keyboard profile.
  if (!netplay_role) {
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, frontend_config.controllers,
            &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::cout << "controller configuration: " << controller_message << '\n';
  }

  const auto inspected = moderngekko::InspectGame(config.game_root);
  if (!inspected) {
    std::cerr << "invalid game: " << inspected.error << '\n';
    return 2;
  }

#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (inspected.metadata->disc_id != MODERNGEKKO_REQUIRED_DISC_ID) {
    std::cerr << "unsupported disc ID: expected "
              << MODERNGEKKO_REQUIRED_DISC_ID << ", got "
              << inspected.metadata->disc_id << '\n';
    return 2;
  }
#endif

  // Compatibility discovery belongs to the runner, never the runtime library.
  if (module_path.empty()) {
    if (const char *env = std::getenv("STATICRECOMP_MODULE"))
      module_path = env;
    else {
      const std::string module_name =
          "g" + inspected.metadata->disc_id + "_recomp" + LibrarySuffix();
      const auto bundled = ExecutableDirectory(argv[0]) / module_name;
      const auto user_module =
          config.user_directory / "StaticRecompModules" / module_name;
      if (std::filesystem::is_regular_file(bundled))
        module_path = bundled;
      else if (std::filesystem::is_regular_file(user_module))
        module_path = user_module;
    }
  }
  if (!module_path.empty())
    config.module =
        moderngekko::ModuleSource::DynamicPath(std::move(module_path));

#if defined(__linux__) || defined(_WIN32)
  if (!config.headless && config.graphics.backend.empty())
    config.graphics.backend = "Vulkan";
#endif

  if (netplay_role) {
    moderngekko::frontend::NetplayOptions options;
    options.role = *netplay_role;
    options.connection =
        netplay_traversal ? moderngekko::frontend::NetplayConnection::OnlineRoom
                          : moderngekko::frontend::NetplayConnection::Direct;
    options.address = netplay_address.empty() ? frontend_config.netplay_address
                                              : netplay_address;
    options.port = netplay_port.value_or(frontend_config.netplay_port);
    options.nickname = netplay_nickname.empty()
                           ? frontend_config.netplay_nickname
                           : netplay_nickname;
    options.buffer = netplay_buffer.empty() ? frontend_config.netplay_buffer
                                            : netplay_buffer;
    options.mode = netplay_mode.value_or(frontend_config.netplay_mode);
    options.diagnostic_logging =
        netplay_diagnostics || frontend_config.netplay_diagnostic_logging;
    if (!traversal_server.empty())
      options.traversal_server = traversal_server;
    if (traversal_port)
      options.traversal_port = *traversal_port;
    if (traversal_alt_port)
      options.traversal_alt_port = *traversal_alt_port;
    if (netplay_players)
      options.players = *netplay_players;
    if (netplay_timeout)
      options.lobby_timeout = *netplay_timeout;
    const std::vector<std::string> configured_controllers =
        moderngekko::frontend::ReadConfiguredControllers(config.user_directory);
    options.controllers =
        netplay_controllers.empty()
            ? (configured_controllers.empty() ? frontend_config.controllers
                                              : configured_controllers)
            : netplay_controllers;
    if (options.controllers.empty() && !frontend_config.controller.empty())
      options.controllers.push_back(frontend_config.controller);
    // This list is an SDL gamepad selection, and netplay used to refuse to
    // start without one. That predates the keyboard pad profile: with no
    // gamepad plugged in the list is empty, and a keyboard player would be
    // turned away from netplay entirely. The GC pad profile is what actually
    // decides whether input reaches the game, and it always exists now.
    if (options.controllers.empty())
      options.controllers.push_back("Keyboard");
    if (options.controllers.empty()) {
      std::cerr << "netplay requires at least one selected controller\n";
      return 2;
    }
    frontend_config.netplay_address = options.address;
    frontend_config.netplay_port = options.port;
    frontend_config.netplay_nickname = options.nickname;
    frontend_config.netplay_buffer = options.buffer;
    frontend_config.netplay_mode = options.mode;
    frontend_config.controllers = options.controllers;
    frontend_config.controller = options.controllers.front();
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, options.controllers, &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::string save_error;
    if (!moderngekko::frontend::SaveConfig(config.user_directory,
                                           frontend_config, &save_error)) {
      std::cerr << "configuration: " << save_error << '\n';
      return 2;
    }
    std::cerr << "netplay: runner started\n";
    return moderngekko::frontend::RunNetplayLobby(
        std::move(config), std::move(frontend_config), std::move(options));
  }

  // Runtime::Create takes the config by move, so anything needed after the
  // session ends has to be kept here. The in-game menu's "Start Netplay" needs
  // both: the user directory to find the request it wrote, and the whole config
  // to bring the lobby up. Reading config.user_directory after the move silently
  // yields an empty path, which resolves the request file relative to the
  // working directory and makes the restart look like it never fired.
  const moderngekko::RuntimeConfig session_config = config;

  auto created = moderngekko::Runtime::Create(std::move(config));
  if (!created) {
    std::cerr << "initialization failed: " << created.error->message << '\n';
    return 1;
  }
  std::cout << "audio backend: " << created.runtime->GetConfig().audio.backend
            << '\n';

  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);
  std::jthread signal_watcher([&](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      if (s_stop_requested) {
        s_stop_requested = 0;
        created.runtime->RequestStop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  const moderngekko::RuntimeRunResult result = created.runtime->Run();
  signal_watcher.request_stop();
  // The watcher captures created.runtime by reference. A menu-started netplay
  // session resets that runtime below before constructing the lobby/runtime
  // pair, so merely requesting stop leaves a window where the watcher can
  // dereference a destroyed object. Join before any error return or reset.
  signal_watcher.join();
  if (result.error) {
    std::cerr << "runtime failed: " << result.error->message << '\n';
    return 1;
  }

  // The in-game menu cannot join a session in place -- the lobby runs before
  // the core boots and NetPlay_Enable happens inside NetPlayClient::StartGame.
  // So "Start Netplay" writes a request and quits, and the session is rebuilt
  // here, in this process, once the runtime has been torn down.
  {
    const std::filesystem::path request_path =
        session_config.user_directory / "netplay-request.ini";
    std::error_code ec;
    if (std::filesystem::is_regular_file(request_path, ec)) {
      std::string mode;
      std::string address;
      int port = 0;
      {
        std::ifstream request(request_path);
        std::string line;
        while (std::getline(request, line)) {
          const auto eq = line.find('=');
          if (eq == std::string::npos)
            continue;
          auto trim = [](std::string s) {
            const auto b = s.find_first_not_of(" \t\r");
            const auto e = s.find_last_not_of(" \t\r");
            return b == std::string::npos ? std::string()
                                          : s.substr(b, e - b + 1);
          };
          const std::string key = trim(line.substr(0, eq));
          const std::string value = trim(line.substr(eq + 1));
          if (key == "mode")
            mode = value;
          else if (key == "address")
            address = value;
          else if (key == "port")
            port = std::atoi(value.c_str());
        }
      }
      // Delete before acting: a request that survives a crash would trap the
      // user in a relaunch loop with no way back to single player.
      std::filesystem::remove(request_path, ec);

      if (mode == "host" || mode == "join") {
        created.runtime.reset();   // release the single-runtime guard

        moderngekko::frontend::NetplayOptions netplay;
        netplay.role = mode == "host"
                           ? moderngekko::frontend::NetplayRole::Host
                           : moderngekko::frontend::NetplayRole::Join;
        // The menu writes an address only when joining. Falling back to the
        // configured one keeps a host request -- and any older request file --
        // behaving exactly as before.
        netplay.address =
            address.empty() ? frontend_config.netplay_address : address;
        netplay.port = port > 0 ? static_cast<std::uint16_t>(port)
                                : frontend_config.netplay_port;
        netplay.nickname = frontend_config.netplay_nickname;
        netplay.buffer = frontend_config.netplay_buffer;
        netplay.mode = frontend_config.netplay_mode;
        // A session started from the in-game menu is a person walking between
        // two machines: quit here, wait for the game to tear down, boot the
        // other one, find the row, join. The 120 s default is a scripted-test
        // figure and it expires in the middle of that, leaving a host that has
        // already given up by the time the joiner arrives -- which looks like
        // "joining does nothing" from the other end rather than a timeout.
        // Scripted runs pass --netplay-timeout and are unaffected.
        netplay.lobby_timeout = 600;
        netplay.controllers = frontend_config.controllers;
        if (netplay.controllers.empty())
          netplay.controllers.push_back("Keyboard");
        // Persist what the menu just chose, so it is the default next time and
        // the menu can seed itself from it. Entering an address an octet at a
        // time is tolerable once and tedious every launch. A failure here is
        // not worth refusing the session over -- the address is already in
        // netplay.address and this session will connect either way.
        frontend_config.netplay_address = netplay.address;
        frontend_config.netplay_port = netplay.port;
        std::string save_error;
        if (!moderngekko::frontend::SaveConfig(session_config.user_directory,
                                               frontend_config, &save_error)) {
          std::cerr << "netplay: could not save the address: " << save_error
                    << '\n';
        }
        std::cerr << "netplay: restarting from the in-game menu\n";
        return moderngekko::frontend::RunNetplayLobby(
            session_config, std::move(frontend_config), std::move(netplay));
      }
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  try {
    return RunMain(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "fatal error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "fatal error: unknown exception\n";
  }
  return 1;
}
