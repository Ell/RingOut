// Netplay coverage against the PUBLIC RecompCore tree.
//
// This test used to be written against an unpushed RecompCore fork, so it had
// never compiled here at all -- and nothing noticed, because CI runs no tests.
// The symbols it needed and this tree initially did not have:
//
//   NetPlayServer::CanStart, NetPlayServer::SetAdaptiveBuffer
//   NetPlayClient::SetLocalControllerCount / GetAssignedControllerCount /
//     GetPlayersSnapshot, and a 6th constructor argument for the local
//     controller count
//   MessageID::PadBufferRequest, NetPlay::INPUT_CHANNEL
//
// The unsupported assertions remain deleted rather than stubbed out -- a stub
// that always passes is worse than an absence.  Compatibility identity is now
// exercised through the real initial handshake, including malformed and
// legacy rollback rejection, as well as by its pure computation test.

// Needed again for `sf::Packet << MessageID`: the generic enum operator lives
// here, and the pad-routing case below is the only thing that serialises a
// message by hand. It was dropped when this test was ported off the fork's
// netplay API and no sf:: remained.
#include "Common/SFMLHelper.h"
#include "Common/Version.h"
#include "Core/Boot/Boot.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "Core/NetPlay/RollbackStateDigestProtocol.h"
#include "UICommon/UICommon.h"
#include "UICommon/GameFile.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/runtime.hpp"
#include "netplay_compatibility.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <enet/enet.h>
#include <filesystem>
#include <memory>
#include <optional>
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
  void OnPlayerDisconnect(const std::string &) override { disconnects++; }
  void OnPadBufferChanged(u32 value) override { buffer = value; }
  void OnHostInputAuthorityChanged(bool) override {}
  void OnDesync(u32 frame, const std::string &) override {
    desync_frame = frame;
    desyncs++;
  }
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
    return std::make_shared<const UICommon::GameFile>();
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
  std::atomic<u32> disconnects{0};
  std::atomic<u32> desyncs{0};
  std::atomic<u32> desync_frame{0};
};

bool WaitFor(const auto &condition) {
  for (int i = 0; i < 100; ++i) {
    if (condition())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

std::optional<NetPlay::ConnectionError>
ExchangeRawHello(u16 port, const sf::Packet &hello) {
  ENetHost *host = enet_host_create(nullptr, 1, NetPlay::CHANNEL_COUNT, 0, 0);
  if (!host)
    return std::nullopt;

  ENetAddress address{};
  enet_address_set_host(&address, "127.0.0.1");
  address.port = port;
  ENetPeer *peer = enet_host_connect(host, &address, NetPlay::CHANNEL_COUNT, 0);
  if (!peer) {
    enet_host_destroy(host);
    return std::nullopt;
  }

  ENetEvent event{};
  if (enet_host_service(host, &event, 5000) <= 0 ||
      event.type != ENET_EVENT_TYPE_CONNECT) {
    enet_host_destroy(host);
    return std::nullopt;
  }

  ENetPacket *packet = enet_packet_create(hello.getData(), hello.getDataSize(),
                                          ENET_PACKET_FLAG_RELIABLE);
  if (!packet || enet_peer_send(peer, NetPlay::DEFAULT_CHANNEL, packet) != 0) {
    if (packet)
      enet_packet_destroy(packet);
    enet_host_destroy(host);
    return std::nullopt;
  }
  enet_host_flush(host);

  std::optional<NetPlay::ConnectionError> result;
  while (enet_host_service(host, &event, 5000) > 0) {
    if (event.type != ENET_EVENT_TYPE_RECEIVE)
      continue;
    sf::Packet response;
    response.append(event.packet->data, event.packet->dataLength);
    enet_packet_destroy(event.packet);
    NetPlay::ConnectionError error{};
    response >> error;
    if (response && response.endOfPacket())
      result = error;
    break;
  }
  enet_host_destroy(host);
  return result;
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

  NetPlay::RollbackSIInputPacket ownership_packet;
  ownership_packet.session_generation = 1;
  ownership_packet.batch_count = 1;
  ownership_packet.batches[0].pad_mask = 0x01;
  const std::array<u8, 4> ownership_mapping{1, 2, 0, 0};
  if (!NetPlay::IsRollbackSIInputOwnedByPlayer(ownership_packet,
                                               ownership_mapping, 1))
    return 34;
  ownership_packet.batch_count = 2;
  ownership_packet.batches[1].pad_mask = 0x02;
  if (NetPlay::IsRollbackSIInputOwnedByPlayer(ownership_packet,
                                              ownership_mapping, 1))
    return 35;

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
      0, false, &host_ui, NetPlay::NetTraversalConfig{}, first_fingerprint);
  if (!server->is_connected)
    return 1;
  // A RingOut fixed-delay host still requires the exact DOL/module identity.
  // Extension-less legacy handshakes cannot bypass that contract.
  {
    auto legacy = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", server->GetPort(), &second_ui, "Legacy",
        NetPlay::NetTraversalConfig{});
    if (legacy->IsConnected() ||
        legacy->GetConnectionError() !=
            NetPlay::ConnectionError::CompatibilityMismatch)
      return 36;
  }
  // Matching modern fixed-delay peers still connect before rollback is armed.
  {
    auto modern_fixed = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", server->GetPort(), &second_ui, "ModernFixed",
        NetPlay::NetTraversalConfig{}, false, first_fingerprint);
    if (!modern_fixed->IsConnected())
      return 53;
    if (server->SetRollbackNetplayConfig({
            .enabled = true,
            .protocol_version = NetPlay::ROLLBACK_NETPLAY_VERSION,
            .base_delay_samples = 2,
            .rollback_horizon_frames = 8,
        }))
      return 40;
  }
  // Requested mode is part of the initial hello, not inferred later from
  // capability. A rollback-requesting guest must not enter a fixed-delay host
  // and discover the mismatch only after one side has started.
  {
    auto wrong_mode = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", server->GetPort(), &second_ui, "WrongMode",
        NetPlay::NetTraversalConfig{}, true, first_fingerprint);
    if (wrong_mode->IsConnected() ||
        wrong_mode->GetConnectionError() !=
            NetPlay::ConnectionError::CompatibilityMismatch)
      return 50;
  }

  if (NetPlay::ROLLBACK_INPUT_CHANNEL == NetPlay::DEFAULT_CHANNEL ||
      NetPlay::ROLLBACK_INPUT_CHANNEL == NetPlay::CHUNKED_DATA_CHANNEL)
    return 25;
  NetPlay::RollbackNetplayConfig rollback_config{
      .enabled = true,
      .protocol_version = NetPlay::ROLLBACK_NETPLAY_VERSION,
      .base_delay_samples = 2,
      .rollback_horizon_frames = 8,
  };
  // The fixed peer's disconnect is processed asynchronously. Retrying the
  // guarded mode transition observes the actual server roster instead of
  // relying on a timing sleep or a UI callback ordering detail.
  if (!WaitFor([&] { return server->SetRollbackNetplayConfig(rollback_config); }))
    return 54;
  auto invalid_rollback_config = rollback_config;
  invalid_rollback_config.rollback_horizon_frames = 0;
  if (server->SetRollbackNetplayConfig(invalid_rollback_config))
    return 27;
  invalid_rollback_config = rollback_config;
  invalid_rollback_config.base_delay_samples = 0;
  if (server->SetRollbackNetplayConfig(invalid_rollback_config))
    return 55;

  // The inverse cross-mode mismatch is rejected at the same pre-PID boundary:
  // a modern fixed-delay client cannot join a rollback host.
  {
    auto wrong_fixed = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", server->GetPort(), &second_ui, "WrongFixed",
        NetPlay::NetTraversalConfig{}, false, first_fingerprint);
    if (wrong_fixed->IsConnected() ||
        wrong_fixed->GetConnectionError() !=
            NetPlay::ConnectionError::CompatibilityMismatch)
      return 56;
  }

  // Any trailing extension bytes must form the complete bounded extension.
  // This raw client proves the production OnConnect parser rejects malformed
  // wire data before allocating a player ID.
  sf::Packet malformed_hello;
  malformed_hello << Common::GetScmRevGitStr() << Common::GetNetplayDolphinVer()
                  << std::string("Malformed") << u8{0x12};
  if (ExchangeRawHello(server->GetPort(), malformed_hello) !=
      NetPlay::ConnectionError::MalformedHandshake)
    return 37;

  auto mismatched = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &second_ui, "Mismatch",
      NetPlay::NetTraversalConfig{}, true,
      moderngekko::frontend::CompatibilityFingerprint(changed_config,
                                                      metadata));
  if (mismatched->IsConnected() ||
      mismatched->GetConnectionError() !=
          NetPlay::ConnectionError::CompatibilityMismatch ||
      second_ui.error.empty())
    return 38;
  mismatched.reset();

  // Once rollback is explicitly requested, an extension-less legacy peer is
  // rejected at connect time rather than silently downgrading the lobby.
  second_ui.error.clear();
  auto legacy_rollback = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &second_ui, "LegacyRollback",
      NetPlay::NetTraversalConfig{});
  if (legacy_rollback->IsConnected() ||
      legacy_rollback->GetConnectionError() !=
          NetPlay::ConnectionError::CompatibilityMismatch)
    return 39;
  legacy_rollback.reset();

  auto first = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &first_ui, "First",
      NetPlay::NetTraversalConfig{}, true, first_fingerprint);
  if (!first->IsConnected())
    return 2;
  if (!WaitFor([&] { return server->CanUseRollbackNetplay(); }))
    return 28;
  if (first->GetRollbackNetplaySession().enabled)
    return 29;

  NetPlay::RollbackSIInputPacket inactive_input;
  inactive_input.session_generation = 1;
  inactive_input.batch_count = 1;
  inactive_input.batches[0].pad_mask = 1;
  if (first->SendRollbackSIInput(inactive_input))
    return 30;

  // A structurally valid stale unreliable packet is retired outside an active
  // generation rather than disconnecting a player before the next match.
  std::array<u8, NetPlay::ROLLBACK_SI_MAX_PACKET_SIZE> stale_bytes{};
  const auto stale_encoded =
      NetPlay::EncodeRollbackSIInputPacket(inactive_input, stale_bytes);
  if (!stale_encoded)
    return 51;
  sf::Packet stale_wire;
  stale_wire << NetPlay::MessageID::RollbackSIInput <<
      static_cast<u16>(stale_encoded.size);
  for (std::size_t i = 0; i < stale_encoded.size; ++i)
    stale_wire << stale_bytes[i];
  first->SendAsync(std::move(stale_wire), NetPlay::ROLLBACK_INPUT_CHANNEL);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (!first->IsConnected())
    return 52;

  // A second matching modern peer preserves rollback eligibility.
  auto second = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &second_ui, "Second",
      NetPlay::NetTraversalConfig{}, true, first_fingerprint);
  if (!second->IsConnected())
    return 3;
  // The client constructor returns once its own connection is up, but the
  // roster is filled by the receive thread, so both peers have to be waited
  // for rather than read straight away.
  if (!WaitFor([&] { return first->GetPlayers().size() == 2; }) ||
      !WaitFor([&] { return second->GetPlayers().size() == 2; }))
    return 4;
  if (!WaitFor([&] { return server->CanUseRollbackNetplay(); }))
    return 31;

  if (!server->SetRequireReady(true))
    return 41;
  NetPlay::PadMappingArray ready_mapping{};
  ready_mapping[0] = first->GetLocalPlayerId();
  ready_mapping[1] = second->GetLocalPlayerId();
  server->SetPadMapping(ready_mapping);
  if (server->AreAllMappedPlayersReady())
    return 42;
  first->SetReady(true);
  if (!WaitFor([&] {
        const auto players = first->GetPlayers();
        return players.size() == 2 && players[0].ready && !players[1].ready;
      }) ||
      server->AreAllMappedPlayersReady())
    return 43;
  second->SetReady(true);
  if (!WaitFor([&] { return server->AreAllMappedPlayersReady(); }) ||
      !WaitFor([&] {
        const auto players = first->GetPlayers();
        return players.size() == 2 && players[0].ready && players[1].ready;
      }))
    return 44;
  server->ClearReady();
  if (!WaitFor([&] {
        const auto players = first->GetPlayers();
        return players.size() == 2 && !players[0].ready && !players[1].ready;
      }) ||
      server->AreAllMappedPlayersReady())
    return 47;
  if (server->RequestStartGame())
    return 48;
  first->SetReady(true);
  second->SetReady(true);
  if (!WaitFor([&] { return server->AreAllMappedPlayersReady(); }))
    return 49;
  first->SetReady(false);
  if (!WaitFor([&] { return !server->AreAllMappedPlayersReady(); }))
    return 45;
  first->SetReady(true);
  if (!WaitFor([&] { return server->AreAllMappedPlayersReady(); }))
    return 46;

  const NetPlay::RollbackNetplaySession valid_session{
      .enabled = true,
      .protocol_version = NetPlay::ROLLBACK_NETPLAY_VERSION,
      .generation = 1,
      .base_delay_samples = 2,
      .rollback_horizon_frames = 8,
  };
  if (!NetPlay::IsValidRollbackNetplaySession(valid_session))
    return 32;
  auto zero_generation = valid_session;
  zero_generation.generation = 0;
  if (NetPlay::IsValidRollbackNetplaySession(zero_generation))
    return 33;

  // The lobby roster must own its values. The receive thread can erase a
  // departed player immediately after GetPlayers() returns; keeping pointers
  // into the client's map used to make that a use-after-free in lobby UIs.
  const std::vector<NetPlay::Player> initial_players = first->GetPlayers();
  if (initial_players.size() != 2 || initial_players[0].name != "First" ||
      initial_players[1].name != "Second")
    return 23;

  // Round-trips a real message: AdjustPadBufferSize broadcasts MessageID::
  // PadBuffer, which each client turns back into OnPadBufferChanged. It fails
  // if the lobby is connected but not actually exchanging packets.
  server->AdjustPadBufferSize(2);
  if (!WaitFor([&] { return first_ui.buffer == 2 && second_ui.buffer == 2; }))
    return 18;
  // Changing synchronized launch settings clears readiness by contract.
  first->SetReady(true);
  second->SetReady(true);
  if (!WaitFor([&] { return server->AreAllMappedPlayersReady(); }))
    return 69;

  // ---- confirmed rollback state ------------------------------------------
  // Periodic state evidence is client->server only. Matching reports keep the
  // generation alive; a mismatch is announced and fails the whole game closed
  // without pretending that either of two disagreeing peers can be blamed.
  if (!server->StartGame())
    return 70;
  if (!WaitFor([&] {
        const auto a = first->GetRollbackNetplaySession();
        const auto b = second->GetRollbackNetplaySession();
        return a.enabled && b.enabled && a.generation == b.generation;
      }))
    return 71;
  const NetPlay::RollbackNetplaySession first_digest_session =
      first->GetRollbackNetplaySession();
  const auto state_digest = [](const u64 generation, const u32 mem1) {
    return NetPlay::RollbackStateDigest{.session_generation = generation,
                                        .logical_frame = 60,
                                        .mem1_crc32 = mem1,
                                        .locked_l1_crc32 = 0x22222222,
                                        .emulated_timebase = 0x3333333333333333};
  };
  if (!first->SendRollbackStateDigest(state_digest(first_digest_session.generation, 0x11111111)) ||
      !second->SendRollbackStateDigest(state_digest(first_digest_session.generation, 0x11111111)))
    return 72;
  if (!WaitFor([&] {
        const auto retired = server->GetRollbackDigestRetiredThroughFrame();
        return retired && *retired >= 60;
      }) ||
      !server->GetRollbackNetplaySession().enabled || !first->IsConnected() ||
      !second->IsConnected())
    return 73;

  first->RequestStopGame();
  if (!WaitFor([&] {
        return !first->GetRollbackNetplaySession().enabled &&
               !second->GetRollbackNetplaySession().enabled;
      }))
    return 74;

  if (!server->StartGame())
    return 75;
  if (!WaitFor([&] {
        const auto a = first->GetRollbackNetplaySession();
        const auto b = second->GetRollbackNetplaySession();
        return a.enabled && b.enabled && a.generation == b.generation &&
               a.generation != first_digest_session.generation;
      }))
    return 76;
  const u64 mismatch_generation = first->GetRollbackNetplaySession().generation;

  // A structurally valid report from the previous generation is retired even
  // though it arrives on the reliable channel after the next match started.
  std::array<u8, NetPlay::ROLLBACK_STATE_DIGEST_PACKET_SIZE> old_digest_bytes{};
  const auto old_digest_size = NetPlay::EncodeRollbackStateDigest(
      state_digest(first_digest_session.generation, 0x11111111), old_digest_bytes);
  if (!old_digest_size)
    return 77;
  sf::Packet old_digest_packet;
  old_digest_packet << NetPlay::MessageID::RollbackStateDigest <<
      static_cast<u16>(*old_digest_size);
  for (const u8 byte : old_digest_bytes)
    old_digest_packet << byte;
  first->SendAsync(std::move(old_digest_packet), NetPlay::DEFAULT_CHANNEL);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (!first->IsConnected() || !server->GetRollbackNetplaySession().enabled)
    return 78;

  if (!first->SendRollbackStateDigest(state_digest(mismatch_generation, 0x11111111)) ||
      !second->SendRollbackStateDigest(state_digest(mismatch_generation, 0xdeadbeef)))
    return 79;
  if (!WaitFor([&] {
        return first_ui.desyncs == 1 && second_ui.desyncs == 1 &&
               !first->GetRollbackNetplaySession().enabled &&
               !second->GetRollbackNetplaySession().enabled;
      }))
    return 80;
  if (first_ui.desync_frame != 60 || second_ui.desync_frame != 60 ||
      !first->IsConnected() || !second->IsConnected())
    return 81;

  // The legacy routing check below deliberately sends gameplay data without
  // booting a core. Recreate a pristine lobby so its current-game discriminator
  // remains zero on both ends instead of treating the packet as stale data from
  // the just-finished digest sessions.
  second.reset();
  first.reset();
  server.reset();
  server = std::make_unique<NetPlay::NetPlayServer>(
      0, false, &host_ui, NetPlay::NetTraversalConfig{}, first_fingerprint);
  if (!server->is_connected || !server->SetRollbackNetplayConfig(rollback_config))
    return 82;
  first = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &first_ui, "First",
      NetPlay::NetTraversalConfig{}, true, first_fingerprint);
  second = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &second_ui, "Second",
      NetPlay::NetTraversalConfig{}, true, first_fingerprint);
  if (!first->IsConnected() || !second->IsConnected() ||
      !WaitFor([&] { return first->GetPlayers().size() == 2; }))
    return 83;

  // ---- input routing -------------------------------------------------------
  //
  // The gap left open in #13. The server relays MessageID::PadData only from
  // the player who OWNS that port, and DISCONNECTS anyone sending for a port
  // they do not:
  //
  //     if (!IsValidPadIndex(m_pad_map, map) || m_pad_map.at(map) !=
  //     player.pid)
  //         return 1;                       // NetPlayServer.cpp
  //
  // That guard is the whole reason a peer cannot inject input on someone else's
  // controller, and nothing exercised it. It is also what made this test
  // awkward to write: sending unmapped pad data does not fail politely, it
  // drops the connection.
  //
  // The RECEIVED pad data is deliberately not asserted here. GetNetPads()
  // returns false unless m_is_running is set, so observing the far side's
  // buffer needs a booted core -- an integration test, not this. What is
  // checked is the routing decision itself, which is the part that protects
  // the port.
  {
    NetPlay::PadMappingArray pads{}; // 0 = unassigned
    pads[0] = first->GetLocalPlayerId();
    server->SetPadMapping(pads);
    if (!WaitFor([&] {
          return first->GetPadMapping()[0] == first->GetLocalPlayerId();
        }))
      return 19;

    const auto pad_packet = [](NetPlay::PadIndex map) {
      sf::Packet p;
      p << NetPlay::MessageID::PadData << map;
      p << static_cast<u16>(0x0100);            // button
      p << u8(0) << u8(0) << u8(128) << u8(128) // analogA/B, stickX/Y
        << u8(128) << u8(128) << u8(0) << u8(0)
        << u8(1); // substick, triggers, isConnected
      return p;
    };

    // The owner may send for its port, and must stay connected.
    first->SendAsync(pad_packet(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!first->IsConnected())
      return 20;

    // A peer that does NOT own port 0 must be dropped for claiming it. If this
    // ever stops disconnecting, one client can drive another's character.
    second->SendAsync(pad_packet(0));
    // Observed through the OTHER peer's roster: the server drops the offender
    // (OnData != 0 -> OnDisconnect), and that is what the rest of the lobby
    // sees. The offender's own IsConnected() is a less reliable witness.
    if (!WaitFor([&] { return first->GetPlayers().size() == 1; }))
      return 21;
    // A previously acquired snapshot remains valid and unchanged after the
    // network thread removes its corresponding live roster entry.
    if (initial_players.size() != 2 || initial_players[1].name != "Second")
      return 24;
    // ...and dropping the impostor must not take the legitimate peer with it.
    if (!first->IsConnected())
      return 22;
  }

  second.reset();
  first.reset();
  server.reset();
  UICommon::Shutdown();
  std::filesystem::remove_all(directory);
  return 0;
}
