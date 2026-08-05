// netplay_session.cpp — headless netplay lobby.
//
// The previous implementation was written against a private RecompCore fork of
// NetPlayClient/NetPlayServer (SetReady, CanStart, SetLocalControllerCount,
// GetPlayersSnapshot, GetConnectionError, SetAdaptiveBuffer, ...) that is not in
// the vendored Dolphin, so it could not be compiled here and was replaced by a
// stub that always reported "netplay is unavailable". This is a rewrite against
// the vendored API only. The original is kept at
// work/out/netplay_session.cpp.orig; its SessionUI carried over nearly intact,
// while its ImGui/SDL3 lobby window is gone -- this runs headless and is driven
// by flags, which is what a scripted two-instance test needs.
//
// Differences forced by the vendored API, all of them deliberate:
//
//   * There is no ready protocol. The fork had per-player SetReady/CanStart;
//     upstream Dolphin has neither, and the host simply starts. So the host
//     waits for --netplay-players machines to connect and for every one of them
//     to report it has the game, then starts.
//   * Pads must be assigned explicitly. NetPlayServer initialises its map with
//     m_pad_map.fill(0), and 0 means "unassigned" (player ids start at 1), so
//     without this nobody's controller reaches the game and both sides sit at a
//     dead title screen looking like a sync failure.
//   * The client exposes no GetConnectionError(), so the failure reason is
//     captured from the OnConnectionError callback instead.

#include "netplay_session.hpp"

#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "Core/PowerPC/PowerPC.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"
#include "runtime/dolphin_runtime_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace moderngekko::frontend {
namespace {

using Clock = std::chrono::steady_clock;

void Log(const std::string &message) {
  std::cerr << "netplay: " << message << '\n';
  std::cerr.flush();
}

// Bridges Dolphin's netplay callbacks to a headless session. Every method is
// called from the netplay thread, so shared fields are mutex- or atomic-guarded
// and the lobby loop only polls them.
class SessionUI final : public NetPlay::NetPlayUI {
public:
  explicit SessionUI(std::shared_ptr<UICommon::GameFile> game)
      : m_game(std::move(game)) {}

  void SetHosting(bool hosting) { m_hosting = hosting; }
  void SetRuntime(Runtime *runtime) {
    std::lock_guard lock(m_mutex);
    m_runtime = runtime;
  }

  bool TakeStartRequest() { return m_start_requested.exchange(false); }

  std::unique_ptr<BootSessionData> TakeBootData() {
    std::lock_guard lock(m_mutex);
    return std::move(m_boot_data);
  }

  std::string Error() {
    std::lock_guard lock(m_mutex);
    return m_error;
  }

  bool ConnectionLost() const { return m_connection_lost.load(); }
  bool Desynced() const { return m_desynced.load(); }
  u32 DesyncFrame() const { return m_desync_frame.load(); }

  // --- NetPlayUI -----------------------------------------------------------
  // Dolphin hands us the boot data instead of booting itself; the lobby picks
  // it up and feeds it to the runtime through detail::SetBootSessionData.
  void BootGame(const std::string &,
                std::unique_ptr<BootSessionData> boot_session_data) override {
    std::lock_guard lock(m_mutex);
    m_boot_data = std::move(boot_session_data);
  }

  void StopGame() override {
    std::lock_guard lock(m_mutex);
    if (m_runtime)
      m_runtime->RequestStop();
  }

  bool IsHosting() const override { return m_hosting; }
  void Update() override {}

  void AppendChat(const std::string &msg) override { Log("chat: " + msg); }

  void OnMsgChangeGame(const NetPlay::SyncIdentifier &,
                       const std::string &name) override {
    Log("game selected: " + name);
  }

  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig &) override {}

  void OnMsgStartGame() override {
    Log("start signalled by host");
    m_start_requested = true;
  }

  void OnMsgStopGame() override { StopGame(); }
  void OnMsgPowerButton() override { StopGame(); }

  void OnPlayerConnect(const std::string &player) override {
    Log("player joined: " + player);
  }

  void OnPlayerDisconnect(const std::string &player) override {
    Log("player left: " + player);
  }

  void OnPadBufferChanged(u32 buffer) override {
    Log("pad buffer = " + std::to_string(buffer) + " frames");
  }

  void OnHostInputAuthorityChanged(bool) override {}

  // The whole point of the determinism work: if the two cores ever disagree,
  // Dolphin says so here, with the frame it happened on.
  void OnDesync(u32 frame, const std::string &player) override {
    m_desynced = true;
    m_desync_frame = frame;
    {
      std::lock_guard lock(m_mutex);
      m_error = "desync at frame " + std::to_string(frame);
      if (!player.empty())
        m_error += " reported for " + player;
    }
    Log("DESYNC at frame " + std::to_string(frame) +
        (player.empty() ? std::string() : " (" + player + ")"));
    StopGame();
  }

  void OnConnectionLost() override {
    m_connection_lost = true;
    {
      std::lock_guard lock(m_mutex);
      m_error = "connection to the host was lost";
    }
    Log("connection lost");
    StopGame();
  }

  void OnConnectionError(const std::string &message) override {
    {
      std::lock_guard lock(m_mutex);
      m_error = message;
    }
    Log("connection error: " + message);
  }

  void OnTraversalError(Common::TraversalClient::FailureReason) override {
    OnConnectionError("direct connection failed");
  }

  void OnTraversalStateChanged(Common::TraversalClient::State) override {}

  void OnGameStartAborted() override {
    {
      std::lock_guard lock(m_mutex);
      m_error = "game start was aborted";
    }
    Log("game start aborted");
  }

  void OnGolferChanged(bool, const std::string &) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }

  // One game per package, so every sync identifier resolves to it; the
  // comparison result is what tells the peer whether we match.
  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier &sync_identifier,
               NetPlay::SyncIdentifierComparison *found) override {
    const auto comparison = m_game->CompareSyncIdentifier(sync_identifier);
    if (found)
      *found = comparison;
    return m_game;
  }

  std::string FindGBARomPath(const std::array<u8, 20> &, std::string_view,
                             int) override {
    return {};
  }

  void ShowGameDigestDialog(const std::string &) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string &) override {}
  void AbortGameDigest() override {}
  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}

  void ShowChunkedProgressDialog(const std::string &title, u64,
                                 std::span<const int>) override {
    Log("transfer: " + title);
  }

  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64>, std::string) override {}

private:
  std::shared_ptr<UICommon::GameFile> m_game;
  mutable std::mutex m_mutex;
  Runtime *m_runtime = nullptr;
  std::atomic<bool> m_hosting{false};
  std::atomic<bool> m_start_requested{false};
  std::atomic<bool> m_connection_lost{false};
  std::atomic<bool> m_desynced{false};
  std::atomic<u32> m_desync_frame{0};
  std::unique_ptr<BootSessionData> m_boot_data;
  std::string m_error;
};

// Give every connected player one controller port, lowest player id first, so
// the host is always pad 1 and the assignment does not depend on join order.
// Without this the map stays all-zero and no input reaches the game.
void AssignPads(NetPlay::NetPlayServer &server,
                const std::vector<const NetPlay::Player *> &players) {
  std::vector<NetPlay::PlayerId> ids;
  ids.reserve(players.size());
  for (const NetPlay::Player *player : players)
    ids.push_back(player->pid);
  std::sort(ids.begin(), ids.end());

  NetPlay::PadMappingArray mapping{};
  mapping.fill(0);
  for (size_t i = 0; i < ids.size() && i < mapping.size(); ++i)
    mapping[i] = ids[i];
  server.SetPadMapping(mapping);

  std::string summary;
  for (size_t i = 0; i < mapping.size(); ++i) {
    if (mapping[i] == 0)
      continue;
    if (!summary.empty())
      summary += ", ";
    summary += "pad " + std::to_string(i + 1) + " -> player " +
               std::to_string(static_cast<int>(mapping[i]));
  }
  Log("controllers: " + (summary.empty() ? std::string("none") : summary));
}

// Poll until the predicate holds, the connection dies, or we run out of
// patience. Headless runs must never block forever: a hung lobby in a script is
// indistinguishable from a slow one.
template <typename Predicate>
bool WaitFor(SessionUI &ui, NetPlay::NetPlayClient &client,
             std::chrono::seconds timeout, Predicate predicate) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (ui.ConnectionLost() || !client.IsConnected())
      return false;
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

} // namespace

int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options) {
  (void)frontend_config;
  if (options.controllers.empty())
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);

  Log("initializing Dolphin services");
  UICommon::SetUserDirectory(runtime_config.user_directory.string());
  UICommon::Init();
  detail::SetExternalUICommon(true);

  // A dual-core split would let the CPU and GPU threads interleave differently
  // on each peer; netplay needs the deterministic single-thread shape, and the
  // StaticRecomp core is the thing being synchronised.
  Config::SetBase(Config::MAIN_CPU_THREAD, false);
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII, false);
  Config::SetBase(Config::NETPLAY_SYNC_CODES, false);
  Config::SetBase(Config::NETPLAY_STRICT_SETTINGS_SYNC, true);
  Config::SetBase(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
  Config::SetBase(Config::NETPLAY_USE_INDEX, false);

  // Dolphin drops all input, pipe included, whenever the render window lacks
  // focus, and it defaults to off. That is wrong for netplay in two ways: a
  // peer that loses focus mid-match silently sends neutral input while its
  // opponent keeps playing, and two peers on one machine can never both be
  // focused -- which presents exactly as "the guest's controller does nothing"
  // while the host's works, with no error anywhere.
  //
  // This has to go on the RuntimeConfig, not through Config::SetBase: writing
  // it into Dolphin.ini does not survive init, and Runtime::Create then does
  // SetBase(MAIN_INPUT_BACKGROUND_INPUT, config.input.background_input)
  // unconditionally, which overwrites anything set here beforehand.
  runtime_config.input.background_input = true;

  auto game = std::make_shared<UICommon::GameFile>(
      (runtime_config.game_root / "sys/main.dol").string());
  if (!game->IsValid()) {
    Log("the game at " + runtime_config.game_root.string() + " is not valid");
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);
  }

  const GameInspectResult inspected = InspectGame(runtime_config.game_root);
  if (!inspected) {
    Log("could not inspect the game: " + inspected.error);
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);
  }

  SessionUI ui(game);
  const NetPlay::NetTraversalConfig direct{};
  std::unique_ptr<NetPlay::NetPlayServer> server;
  std::unique_ptr<NetPlay::NetPlayClient> client;

  if (options.role == NetplayRole::Host) {
    Log("hosting on port " + std::to_string(options.port));
    ui.SetHosting(true);
    server =
        std::make_unique<NetPlay::NetPlayServer>(options.port, false, &ui, direct);
    if (!server->is_connected) {
      Log("could not open port " + std::to_string(options.port) +
          " (already in use?)");
      server.reset();
      detail::SetExternalUICommon(false);
      UICommon::Shutdown();
      return static_cast<int>(NetplayExitCode::Failed);
    }
    // Fixed-delay, not host input authority: both peers run the same inputs on
    // the same frame, which is the model the determinism work validated.
    server->SetHostInputAuthority(false);
    if (options.buffer != "auto") {
      try {
        server->AdjustPadBufferSize(
            static_cast<unsigned int>(std::stoul(options.buffer)));
      } catch (const std::exception &) {
        Log("ignoring unparsable --buffer '" + options.buffer + "'");
      }
    }
    server->ChangeGame(game->GetSyncIdentifier(), inspected.metadata->game_name);
    // The host plays through a local client too, so it shares one code path
    // with the joiners.
    client = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", server->GetPort(), &ui, options.nickname, direct);
  } else {
    Log("connecting to " + options.address + ":" + std::to_string(options.port));
    client = std::make_unique<NetPlay::NetPlayClient>(
        options.address, options.port, &ui, options.nickname, direct);
  }

  if (!client->IsConnected()) {
    const std::string error = ui.Error();
    Log(error.empty() ? "could not connect to the host" : error);
    client.reset();
    server.reset();
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(NetplayExitCode::HostUnavailable);
  }
  Log("connected as '" + options.nickname + "'");

  const std::chrono::seconds lobby_timeout(options.lobby_timeout);
  int result = 0;

  if (server) {
    const size_t expected = std::max<size_t>(options.players, 1);
    Log("waiting for " + std::to_string(expected) + " player(s)");
    if (!WaitFor(ui, *client, lobby_timeout,
                 [&] { return client->GetPlayers().size() >= expected; })) {
      Log("timed out waiting for players");
      result = static_cast<int>(NetplayExitCode::Failed);
    } else if (!WaitFor(ui, *client, std::chrono::seconds(30),
                        [&] { return client->DoAllPlayersHaveGame(); })) {
      Log("not every player has this game; refusing to start");
      result = static_cast<int>(NetplayExitCode::CompatibilityMismatch);
    } else {
      AssignPads(*server, client->GetPlayers());
      // The mapping is broadcast asynchronously; starting in the same breath
      // can race it, and a player whose pad has not landed yet is dropped from
      // the start.
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      Log("starting game");
      if (!server->RequestStartGame()) {
        Log("the host refused to start the game");
        result = static_cast<int>(NetplayExitCode::Failed);
      }
    }
  } else {
    Log("waiting for the host to start");
  }

  if (result == 0 && !WaitFor(ui, *client, lobby_timeout,
                              [&] { return ui.TakeStartRequest(); })) {
    Log("no start signal arrived");
    result = static_cast<int>(NetplayExitCode::Failed);
  }

  if (result == 0) {
    // OnMsgStartGame only *signals* the start. NetPlayClient::StartGame is what
    // calls NetPlay_Enable -- and until that happens NetPlay::IsNetPlayRunning()
    // is false, which means SI reads local pads instead of GetNetPads and
    // Dolphin's desync detection never arms. Booting straight from the signal
    // gives two independent single-player sessions that look like a clean
    // netplay run: it reports no desync precisely because nothing was checking.
    // That false pass is why the assertion below exists.
    if (!client->StartGame(game->GetFilePath())) {
      Log("the client refused to start the game");
      result = static_cast<int>(NetplayExitCode::Failed);
    }
  }

  if (result == 0) {
    // What this peer believes it owns. A client with zero local pads still
    // boots and runs perfectly happily -- it just never sends any input, and
    // the game sits in attract mode looking like it ignores the controller.
    const NetPlay::PadMappingArray &map = client->GetPadMapping();
    std::string map_text;
    for (size_t i = 0; i < map.size(); ++i)
      map_text += (i ? "," : "") + std::to_string(static_cast<int>(map[i]));
    Log("pad map [" + map_text + "], local pads = " +
        std::to_string(client->NumLocalPads()) +
        ", local player has a controller = " +
        (client->LocalPlayerHasControllerMapped() ? "yes" : "no"));
    if (client->NumLocalPads() == 0)
      Log("WARNING: no local pad -- this peer will send no input");
  }

  if (result == 0) {
    if (!NetPlay::IsNetPlayRunning()) {
      Log("netplay did not arm; refusing to boot (this would silently run as "
          "two unsynchronised single-player sessions)");
      result = static_cast<int>(NetplayExitCode::Failed);
    }
  }

  if (result == 0) {
    // StartGame produced this via BootGame; the runtime boots through it so the
    // session's synced settings and save data apply.
    if (auto boot_data = ui.TakeBootData()) {
      detail::SetBootSessionData(std::move(boot_data));
    } else {
      Log("no boot data arrived from netplay; refusing to boot");
      result = static_cast<int>(NetplayExitCode::Failed);
    }
  }

  if (result == 0) {
    Log("netplay armed; booting");
    auto created = Runtime::Create(std::move(runtime_config));
    if (!created) {
      Log("initialization failed: " + created.error->message);
      result = 1;
    } else {
      ui.SetRuntime(created.runtime.get());
      const RuntimeRunResult run_result = created.runtime->Run();
      ui.SetRuntime(nullptr);
      if (run_result.error) {
        Log("run failed: " + run_result.error->message);
        result = 1;
      }
      created.runtime.reset();
    }
  }

  if (ui.Desynced()) {
    Log("session ended DESYNCED at frame " + std::to_string(ui.DesyncFrame()));
    result = 1;
  } else if (result == 0) {
    Log("session ended cleanly, no desync reported");
  }

  client->Stop();
  client->StopGame();
  client.reset();
  server.reset();
  detail::SetExternalUICommon(false);
  UICommon::Shutdown();
  return result;
}
} // namespace moderngekko::frontend
