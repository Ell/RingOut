// Netplay coverage against the PUBLIC RecompCore tree.
//
// This test used to be written against an unpushed RecompCore fork, so it had
// never compiled here at all -- and nothing noticed, because CI runs no tests.
// The symbols it needed and this tree does not have:
//
//   NetPlay::SetCompatibilityFingerprint, NetPlayClient::GetConnectionError,
//   ConnectionError::CompatibilityMismatch  -- a build-hash handshake that
//     rejects a mismatched peer at connect time
//   NetPlayServer::CanStart, NetPlayServer::SetAdaptiveBuffer
//   NetPlayClient::SetLocalControllerCount / GetAssignedControllerCount /
//     GetPlayersSnapshot, and a 6th constructor argument for the local
//     controller count
//   MessageID::PadBufferRequest, NetPlay::INPUT_CHANNEL
//
// vendor/dolphin carries stock netplay, which has none of that. Those
// assertions are deleted rather than stubbed out -- a stub that always passes
// is worse than an absence. They are in git history if the fork ever lands.
//
// What survives is what this tree can genuinely assert: the ModernGekko-side
// compatibility fingerprint, which is pure computation, and a two-peer
// localhost lobby over the stock protocol. Exit codes are unchanged from the
// original for the checks that survived, so an old failure number still means
// the same thing.

#include "Core/Boot/Boot.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "UICommon/UICommon.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/runtime.hpp"
#include "netplay_compatibility.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
int DispatchA(CPUState *, std::uint32_t) { return 0; }
int DispatchB(CPUState *, std::uint32_t) { return 0; }

constexpr ModernGekkoRange module_ranges[] = {
    {0x80003100u, 0x80003120u},
};
constexpr std::uint64_t first_hashes[] = {0x123456789abcdef0u};
constexpr std::uint64_t second_hashes[] = {0x123456789abcdef0u};
constexpr std::uint64_t changed_hashes[] = {0x123456789abcdef1u};

const ModernGekkoModuleDesc first_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    DispatchA,
    nullptr,
    module_ranges,
    1,
    nullptr,
    0,
    module_ranges,
    1,
    first_hashes,
};

const ModernGekkoModuleDesc second_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    DispatchB,
    nullptr,
    module_ranges,
    1,
    nullptr,
    0,
    module_ranges,
    1,
    second_hashes,
};

const ModernGekkoModuleDesc changed_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    DispatchB,
    nullptr,
    module_ranges,
    1,
    nullptr,
    0,
    module_ranges,
    1,
    changed_hashes,
};
} // namespace

class TestUI final : public NetPlay::NetPlayUI {
public:
  void BootGame(const std::string &,
                std::unique_ptr<BootSessionData>) override {}
  void StopGame() override {}
  bool IsHosting() const override { return false; }
  void Update() override {}
  void AppendChat(const std::string &) override {}
  void OnMsgChangeGame(const NetPlay::SyncIdentifier &,
                       const std::string &) override {}
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig &) override {}
  void OnMsgStartGame() override {}
  void OnMsgStopGame() override {}
  void OnMsgPowerButton() override {}
  void OnPlayerConnect(const std::string &) override {}
  void OnPlayerDisconnect(const std::string &) override {}
  void OnPadBufferChanged(u32 value) override { buffer = value; }
  void OnHostInputAuthorityChanged(bool) override {}
  void OnDesync(u32, const std::string &) override {}
  void OnConnectionLost() override {}
  void OnConnectionError(const std::string &message) override {
    error = message;
  }
  void OnTraversalError(Common::TraversalClient::FailureReason) override {}
  void OnTraversalStateChanged(Common::TraversalClient::State) override {}
  void OnGameStartAborted() override {}
  void OnGolferChanged(bool, const std::string &) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }
  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier &,
               NetPlay::SyncIdentifierComparison *found) override {
    if (found)
      *found = NetPlay::SyncIdentifierComparison::DifferentGame;
    return {};
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
  void ShowChunkedProgressDialog(const std::string &, u64,
                                 std::span<const int>) override {}
  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64>, std::string) override {}

  std::string error;
  std::atomic<u32> buffer{0};
};

bool WaitFor(const auto &condition) {
  for (int i = 0; i < 100; ++i) {
    if (condition())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

int main() {
  moderngekko::GameMetadata metadata;
  metadata.disc_id = "TEST01";
  metadata.dol_sha256 = "dol";
  moderngekko::RuntimeConfig first_config;
  first_config.module =
      moderngekko::ModuleSource::AttachedDescriptor(&first_descriptor);
  moderngekko::RuntimeConfig second_config;
  second_config.module =
      moderngekko::ModuleSource::AttachedDescriptor(&second_descriptor);
  moderngekko::RuntimeConfig changed_config;
  changed_config.module =
      moderngekko::ModuleSource::AttachedDescriptor(&changed_descriptor);
  const std::string first_fingerprint =
      moderngekko::frontend::CompatibilityFingerprint(first_config, metadata);
  if (first_fingerprint !=
      moderngekko::frontend::CompatibilityFingerprint(second_config, metadata))
    return 11;
  if (first_fingerprint ==
      moderngekko::frontend::CompatibilityFingerprint(changed_config, metadata))
    return 12;

  const auto directory =
      std::filesystem::temp_directory_path() / "moderngekko-netplay-test";
  std::filesystem::remove_all(directory);
  UICommon::SetUserDirectory(directory.string());
  UICommon::Init();

  TestUI host_ui;
  TestUI first_ui;
  TestUI second_ui;
  TestUI invalid_ui;
  auto invalid = std::make_unique<NetPlay::NetPlayClient>(
      "invalid host", 2626, &invalid_ui, "Invalid",
      NetPlay::NetTraversalConfig{});
  if (invalid->IsConnected() || invalid_ui.error.empty())
    return 10;
  invalid.reset();

  // Port 0 asks the OS for a free one, so concurrent test runs cannot collide.
  auto server = std::make_unique<NetPlay::NetPlayServer>(
      0, false, &host_ui, NetPlay::NetTraversalConfig{});
  if (!server->is_connected)
    return 1;
  auto first = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &first_ui, "First",
      NetPlay::NetTraversalConfig{});
  if (!first->IsConnected())
    return 2;
  auto second = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &second_ui, "Second",
      NetPlay::NetTraversalConfig{});
  if (!second->IsConnected())
    return 3;
  // The client constructor returns once its own connection is up, but the
  // roster is filled by the receive thread, so both peers have to be waited
  // for rather than read straight away.
  if (!WaitFor([&] { return first->GetPlayers().size() == 2; }) ||
      !WaitFor([&] { return second->GetPlayers().size() == 2; }))
    return 4;

  // Round-trips a real message: AdjustPadBufferSize broadcasts MessageID::
  // PadBuffer, which each client turns back into OnPadBufferChanged. It fails
  // if the lobby is connected but not actually exchanging packets.
  server->AdjustPadBufferSize(2);
  if (!WaitFor([&] { return first_ui.buffer == 2 && second_ui.buffer == 2; }))
    return 18;

  second.reset();
  first.reset();
  server.reset();
  UICommon::Shutdown();
  std::filesystem::remove_all(directory);
  return 0;
}
