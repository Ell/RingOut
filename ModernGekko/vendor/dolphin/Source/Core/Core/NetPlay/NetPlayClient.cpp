// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayConnectProtocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/ENet.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/QoSSession.h"
#include "Common/SFMLHelper.h"
#include "Common/Timer.h"
#include "Common/Version.h"

#include "Core/Boot/Boot.h"
#include "Core/Cheats/ActionReplay.h"
#include "Core/Cheats/GeckoCode.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SI/SI_DeviceAMBaseboard.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/Sram.h"
#include "Core/HW/WiiSave.h"
#include "Core/HW/WiiSaveStructs.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/Uids.h"
#include "Core/Movie.h"
#include "Core/NetPlay/NetPlayClientRollback.h"
#include "Core/NetPlay/NetPlayCommon.h"
#include "Core/SyncIdentifier.h"
#include "Core/System.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"

#include "InputCommon/GCAdapter.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace NetPlay
{
using namespace WiimoteCommon;

static std::mutex crit_netplay_client;
static NetPlayClient* netplay_client = nullptr;
static bool s_si_poll_batching = false;

namespace
{
constexpr std::size_t SC2_ENGINE_INPUT_POLL_LIMIT = 4096;

enum class Sc2EngineInputMode
{
  Disabled,
  Capture,
  Replay,
};

// All users are on Dolphin's CPU thread. Keeping the probe journal thread-local
// also makes it impossible for the netplay socket thread to observe a partial
// capture or consume a replay entry.
thread_local Sc2EngineInputMode s_sc2_engine_input_mode = Sc2EngineInputMode::Disabled;
thread_local std::vector<Sc2EngineInputPoll> s_sc2_engine_input_polls;
thread_local std::size_t s_sc2_engine_input_cursor = 0;
thread_local bool s_sc2_engine_input_valid = true;
thread_local bool s_sc2_engine_input_perturb_remote_a = false;
thread_local std::size_t s_sc2_engine_input_perturbed_polls = 0;
thread_local std::optional<u64> s_sc2_engine_input_batch;

bool TryReplaySc2EngineInput(const int pad_num, const bool batching, GCPadStatus* const status,
                             bool* const result)
{
  if (s_sc2_engine_input_mode != Sc2EngineInputMode::Replay)
    return false;

  if (s_sc2_engine_input_cursor >= s_sc2_engine_input_polls.size())
  {
    s_sc2_engine_input_valid = false;
    *status = {};
    *result = false;
    return true;
  }

  const Sc2EngineInputPoll& poll = s_sc2_engine_input_polls[s_sc2_engine_input_cursor++];
  if (poll.pad_num != pad_num || poll.batching != batching)
    s_sc2_engine_input_valid = false;
  *status = poll.status;
  if (s_sc2_engine_input_perturb_remote_a && poll.pad_num == 1 && poll.result)
  {
    status->button ^= PAD_BUTTON_A;
    ++s_sc2_engine_input_perturbed_polls;
  }
  *result = poll.result;
  return true;
}

void RecordSc2EngineInput(const int pad_num, const bool batching, const GCPadStatus& status,
                          const bool result)
{
  if (s_sc2_engine_input_mode != Sc2EngineInputMode::Capture)
    return;
  if (s_sc2_engine_input_polls.size() >= SC2_ENGINE_INPUT_POLL_LIMIT)
  {
    s_sc2_engine_input_valid = false;
    return;
  }
  s_sc2_engine_input_polls.push_back(
      {.pad_num = pad_num,
       .batching = batching,
       .result = result,
       .status = status,
       .batch_id = s_sc2_engine_input_batch});
}
}  // namespace

void BeginSc2EngineInputCapture()
{
  s_sc2_engine_input_polls.clear();
  s_sc2_engine_input_cursor = 0;
  s_sc2_engine_input_valid = true;
  s_sc2_engine_input_mode = Sc2EngineInputMode::Capture;
}

bool FinishSc2EngineInputCapture(std::size_t* const captured_polls,
                                 std::vector<u64>* const consumed_batches,
                                 std::vector<Sc2EngineInputPoll>* const polls)
{
  const bool valid = s_sc2_engine_input_mode == Sc2EngineInputMode::Capture &&
                     s_sc2_engine_input_valid;
  s_sc2_engine_input_mode = Sc2EngineInputMode::Disabled;
  if (captured_polls)
    *captured_polls = s_sc2_engine_input_polls.size();
  if (consumed_batches)
  {
    consumed_batches->clear();
    for (const Sc2EngineInputPoll& poll : s_sc2_engine_input_polls)
    {
      if (poll.batch_id &&
          (consumed_batches->empty() || consumed_batches->back() != *poll.batch_id))
      {
        consumed_batches->push_back(*poll.batch_id);
      }
    }
  }
  if (polls)
    *polls = s_sc2_engine_input_polls;
  return valid;
}

void SetSc2EngineInputBatch(const std::optional<u64> batch_id)
{
  s_sc2_engine_input_batch = batch_id;
}

bool BeginSc2EngineInputReplay(const bool perturb_remote_a)
{
  if (s_sc2_engine_input_mode != Sc2EngineInputMode::Disabled || !s_sc2_engine_input_valid)
    return false;
  s_sc2_engine_input_cursor = 0;
  s_sc2_engine_input_perturb_remote_a = perturb_remote_a;
  s_sc2_engine_input_perturbed_polls = 0;
  s_sc2_engine_input_mode = Sc2EngineInputMode::Replay;
  return true;
}

bool BeginSc2EngineInputReplayFrom(const std::span<const Sc2EngineInputPoll> polls,
                                   const bool perturb_remote_a)
{
  if (s_sc2_engine_input_mode != Sc2EngineInputMode::Disabled || polls.empty() ||
      polls.size() > SC2_ENGINE_INPUT_POLL_LIMIT)
  {
    return false;
  }
  s_sc2_engine_input_polls.assign(polls.begin(), polls.end());
  s_sc2_engine_input_valid = true;
  return BeginSc2EngineInputReplay(perturb_remote_a);
}

bool ConsumeSc2EngineInputReplay(const int pad_num, const bool batching,
                                 GCPadStatus* const status, bool* const result)
{
  if (status == nullptr || result == nullptr)
    return false;
  return TryReplaySc2EngineInput(pad_num, batching, status, result);
}

bool FinishSc2EngineInputReplay(std::size_t* const perturbed_polls)
{
  const bool valid = s_sc2_engine_input_mode == Sc2EngineInputMode::Replay &&
                     s_sc2_engine_input_valid &&
                     s_sc2_engine_input_cursor == s_sc2_engine_input_polls.size() &&
                     (!s_sc2_engine_input_perturb_remote_a ||
                      s_sc2_engine_input_perturbed_polls != 0);
  if (perturbed_polls)
    *perturbed_polls = s_sc2_engine_input_perturbed_polls;
  s_sc2_engine_input_mode = Sc2EngineInputMode::Disabled;
  s_sc2_engine_input_perturb_remote_a = false;
  return valid;
}

void EndSc2EngineInputReplay()
{
  s_sc2_engine_input_mode = Sc2EngineInputMode::Disabled;
  s_sc2_engine_input_polls.clear();
  s_sc2_engine_input_cursor = 0;
  s_sc2_engine_input_valid = true;
  s_sc2_engine_input_perturb_remote_a = false;
  s_sc2_engine_input_perturbed_polls = 0;
}

// called from ---GUI--- thread
NetPlayClient::~NetPlayClient()
{
  // not perfect
  if (m_is_running.IsSet())
    StopGame();
  {
    std::lock_guard lk(crit_netplay_client);
    ResetLiveRollbackImpl();
    if (netplay_client == this)
      netplay_client = nullptr;
  }

  if (m_is_connected)
  {
    m_should_compute_game_digest = false;
    m_dialog->AbortGameDigest();
    if (m_game_digest_thread.joinable())
      m_game_digest_thread.join();
    m_do_loop.Clear();
    m_thread.join();

    m_chunked_data_receive_queue.clear();
    m_dialog->HideChunkedProgressDialog();
  }

  if (m_server)
  {
    Disconnect();
  }

  if (Common::g_MainNetHost.get() == m_client)
  {
    Common::g_MainNetHost.release();
  }
  if (m_client)
  {
    enet_host_destroy(m_client);
    m_client = nullptr;
  }

  if (m_traversal_client)
  {
    Common::ReleaseTraversalClient();
  }
}

// called from ---GUI--- thread
NetPlayClient::NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog,
                             std::string name, const NetTraversalConfig& traversal_config,
                             const bool advertise_rollback_capability,
                             std::string compatibility_fingerprint,
                             const bool rollback_gamecube_title_verified)
    : m_dialog(dialog), m_player_name(std::move(name)),
      m_advertise_rollback_capability(advertise_rollback_capability),
      m_rollback_gamecube_title_verified(rollback_gamecube_title_verified),
      m_compatibility_fingerprint(std::move(compatibility_fingerprint))
{
  if (m_advertise_rollback_capability && !LoadRollbackFaultScript())
  {
    std::fprintf(stderr, "[rollback live] invalid fault script; capability disabled\n");
    m_advertise_rollback_capability = false;
  }
  ClearBuffers();

  if (!traversal_config.use_traversal)
  {
    // Direct Connection
    m_client = enet_host_create(nullptr, 1, CHANNEL_COUNT, 0, 0);

    if (m_client == nullptr)
    {
      m_dialog->OnConnectionError(_trans("Could not create client."));
      return;
    }

    m_client->mtu = std::min(m_client->mtu, NetPlay::MAX_ENET_MTU);

    ENetAddress addr;
    enet_address_set_host(&addr, address.c_str());
    addr.port = port;

    m_server = enet_host_connect(m_client, &addr, CHANNEL_COUNT, 0);

    if (m_server == nullptr)
    {
      m_dialog->OnConnectionError(_trans("Could not create peer."));
      return;
    }

    // Update time in milliseconds of no acknowledgment of
    // sent packets before a connection is deemed disconnected
    enet_peer_timeout(m_server, 0, PEER_TIMEOUT.count(), PEER_TIMEOUT.count());

    ENetEvent netEvent;
    int net = enet_host_service(m_client, &netEvent, 5000);
    if (net > 0 && netEvent.type == ENET_EVENT_TYPE_CONNECT)
    {
      if (Connect())
      {
        m_client->intercept = Common::ENet::InterceptCallback;
        m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
      }
    }
    else
    {
      m_dialog->OnConnectionError(_trans("Could not communicate with host."));
    }
  }
  else
  {
    if (address.size() > Common::NETPLAY_CODE_SIZE)
    {
      m_dialog->OnConnectionError(
          _trans("The host code is too long.\nPlease recheck that you have the correct code."));
      return;
    }

    if (!Common::EnsureTraversalClient(traversal_config.traversal_host,
                                       traversal_config.traversal_port,
                                       traversal_config.traversal_port_alt))
    {
      return;
    }
    m_client = Common::g_MainNetHost.get();

    m_traversal_client = Common::g_TraversalClient.get();

    // If we were disconnected in the background, reconnect.
    if (m_traversal_client->HasFailed())
      m_traversal_client->ReconnectToServer();
    m_traversal_client->m_Client = this;
    m_host_spec = address;
    m_connection_state = ConnectionState::WaitingForTraversalClientConnection;
    OnTraversalStateChanged();
    m_connecting = true;

    Common::Timer connect_timer;
    connect_timer.Start();

    while (m_connecting)
    {
      ENetEvent netEvent;
      if (m_traversal_client)
        m_traversal_client->HandleResends();

      while (enet_host_service(m_client, &netEvent, 4) > 0)
      {
        sf::Packet rpac;
        switch (netEvent.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
          m_server = netEvent.peer;

          // Update time in milliseconds of no acknowledgment of
          // sent packets before a connection is deemed disconnected
          enet_peer_timeout(m_server, 0, PEER_TIMEOUT.count(), PEER_TIMEOUT.count());

          if (Connect())
          {
            m_connection_state = ConnectionState::Connected;
            m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
          }
          return;
        default:
          break;
        }
      }
      if (connect_timer.ElapsedMs() > 5000)
        break;
    }
    if (m_connection_state != ConnectionState::Failure)
    {
      INFO_LOG_FMT(NETPLAY, "ENet did not connect after traversal rendezvous.");
      m_dialog->OnConnectionError(_trans("Could not communicate with host."));
    }
  }
}

bool NetPlayClient::Connect()
{
  INFO_LOG_FMT(NETPLAY, "Connecting to server.");

  // send connect message
  sf::Packet packet;
  packet << Common::GetScmRevGitStr();
  packet << Common::GetNetplayDolphinVer();
  packet << m_player_name;
  if (!m_compatibility_fingerprint.empty() || m_advertise_rollback_capability)
  {
    if (!AppendNetPlayConnectExtension(packet, m_advertise_rollback_capability,
                                       m_advertise_rollback_capability,
                                       m_compatibility_fingerprint))
    {
      m_connection_error = ConnectionError::MalformedHandshake;
      m_dialog->OnConnectionError(_trans("The local NetPlay compatibility identity is invalid."));
      return false;
    }
  }
  Send(packet);
  enet_host_flush(m_client);
  sf::Packet rpac;
  // TODO: make this not hang
  ENetEvent netEvent;
  int net;
  while ((net = enet_host_service(m_client, &netEvent, 5000)) > 0 &&
         static_cast<int>(netEvent.type) == Common::ENet::SKIPPABLE_EVENT)
  {
    // ignore packets from traversal server
  }
  if (net > 0 && netEvent.type == ENET_EVENT_TYPE_RECEIVE)
  {
    rpac.append(netEvent.packet->data, netEvent.packet->dataLength);
    enet_packet_destroy(netEvent.packet);
  }
  else
  {
    return false;
  }

  ConnectionError error;
  rpac >> error;
  if (!rpac || static_cast<u8>(error) > static_cast<u8>(ConnectionError::MalformedHandshake) ||
      (error != ConnectionError::NoError && !rpac.endOfPacket()))
  {
    m_connection_error = ConnectionError::MalformedHandshake;
    m_dialog->OnConnectionError(_trans("The server sent a malformed handshake response."));
    Disconnect();
    return false;
  }
  m_connection_error = error;

  // got error message
  if (error != ConnectionError::NoError)
  {
    switch (error)
    {
    case ConnectionError::ServerFull:
      m_dialog->OnConnectionError(_trans("The server is full."));
      break;
    case ConnectionError::VersionMismatch:
      m_dialog->OnConnectionError(
          _trans("The server and client's NetPlay versions are incompatible."));
      break;
    case ConnectionError::GameRunning:
      m_dialog->OnConnectionError(_trans("The game is currently running."));
      break;
    case ConnectionError::NameTooLong:
      m_dialog->OnConnectionError(_trans("Nickname is too long."));
      break;
    case ConnectionError::CompatibilityMismatch:
      m_dialog->OnConnectionError(
          _trans("The extracted game or recomp module does not match the host."));
      break;
    case ConnectionError::MalformedHandshake:
      m_dialog->OnConnectionError(_trans("The host rejected a malformed NetPlay handshake."));
      break;
    default:
      m_dialog->OnConnectionError(_trans("The server sent an unknown error message."));
      break;
    }

    Disconnect();
    return false;
  }
  else
  {
    rpac >> m_pid;
    if (!rpac || !rpac.endOfPacket())
    {
      m_connection_error = ConnectionError::MalformedHandshake;
      m_dialog->OnConnectionError(_trans("The server sent a malformed handshake response."));
      Disconnect();
      return false;
    }

    Player player;
    player.name = m_player_name;
    player.pid = m_pid;
    player.revision = Common::GetNetplayDolphinVer();

    // add self to player list
    m_players[m_pid] = player;
    m_local_player = &m_players[m_pid];

    m_dialog->Update();

    m_is_connected = true;

    return true;
  }
}

static void ReceiveSyncIdentifier(sf::Packet& spac, SyncIdentifier& sync_identifier)
{
  // We use a temporary variable here due to a potential long vs long long mismatch
  u64 dol_elf_size;
  spac >> dol_elf_size;
  sync_identifier.dol_elf_size = dol_elf_size;

  spac >> sync_identifier.game_id;
  spac >> sync_identifier.revision;
  spac >> sync_identifier.disc_number;
  spac >> sync_identifier.is_datel;

  for (u8& x : sync_identifier.sync_hash)
    spac >> x;
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnData(sf::Packet& packet, const u8 channel_id)
{
  MessageID mid{};
  packet >> mid;

  if (!packet || (channel_id == ROLLBACK_INPUT_CHANNEL) != (mid == MessageID::RollbackSIInput))
  {
    FailRollbackProtocol();
    return;
  }

  INFO_LOG_FMT(NETPLAY, "Got server message: {:x}", static_cast<u8>(mid));

  switch (mid)
  {
  case MessageID::PlayerJoin:
    OnPlayerJoin(packet);
    break;

  case MessageID::PlayerLeave:
    OnPlayerLeave(packet);
    break;

  case MessageID::Ready:
    OnPlayerReady(packet, true);
    break;

  case MessageID::NotReady:
    OnPlayerReady(packet, false);
    break;

  case MessageID::ChatMessage:
    OnChatMessage(packet);
    break;

  case MessageID::ChunkedDataStart:
    OnChunkedDataStart(packet);
    break;

  case MessageID::ChunkedDataEnd:
    OnChunkedDataEnd(packet);
    break;

  case MessageID::ChunkedDataPayload:
    OnChunkedDataPayload(packet);
    break;

  case MessageID::ChunkedDataAbort:
    OnChunkedDataAbort(packet);
    break;

  case MessageID::PadMapping:
    OnPadMapping(packet);
    break;

  case MessageID::GBAConfig:
    OnGBAConfig(packet);
    break;

  case MessageID::WiimoteMapping:
    OnWiimoteMapping(packet);
    break;

  case MessageID::PadData:
    OnPadData(packet);
    break;

  case MessageID::PadHostData:
    OnPadHostData(packet);
    break;

  case MessageID::WiimoteData:
    OnWiimoteData(packet);
    break;

  case MessageID::PadBuffer:
    OnPadBuffer(packet);
    break;

  case MessageID::HostInputAuthority:
    OnHostInputAuthority(packet);
    break;

  case MessageID::RollbackCapabilityQuery:
    OnRollbackCapabilityQuery(packet);
    break;

  case MessageID::RollbackSession:
    if (!OnRollbackSession(packet))
      FailRollbackProtocol();
    break;

  case MessageID::RollbackSIInput:
    if (!OnRollbackSIInput(packet))
      FailRollbackProtocol();
    break;

  case MessageID::GolfSwitch:
    OnGolfSwitch(packet);
    break;

  case MessageID::GolfPrepare:
    OnGolfPrepare(packet);
    break;

  case MessageID::ChangeGame:
    OnChangeGame(packet);
    break;

  case MessageID::GameStatus:
    OnGameStatus(packet);
    break;

  case MessageID::StartGame:
    OnStartGame(packet);
    break;

  case MessageID::StopGame:
  case MessageID::DisableGame:
    OnStopGame(packet);
    break;

  case MessageID::PowerButton:
    OnPowerButton();
    break;

  case MessageID::Ping:
    OnPing(packet);
    break;

  case MessageID::PlayerPingData:
    OnPlayerPingData(packet);
    break;

  case MessageID::DesyncDetected:
    OnDesyncDetected(packet);
    break;

  case MessageID::SyncSaveData:
    OnSyncSaveData(packet);
    break;

  case MessageID::SyncCodes:
    OnSyncCodes(packet);
    break;

  case MessageID::ComputeGameDigest:
    OnComputeGameDigest(packet);
    break;

  case MessageID::GameDigestProgress:
    OnGameDigestProgress(packet);
    break;

  case MessageID::GameDigestResult:
    OnGameDigestResult(packet);
    break;

  case MessageID::GameDigestError:
    OnGameDigestError(packet);
    break;

  case MessageID::GameDigestAbort:
    OnGameDigestAbort();
    break;

  default:
    PanicAlertFmtT("Unknown message received with id : {0}", static_cast<u8>(mid));
    break;
  }
}

void NetPlayClient::OnPlayerJoin(sf::Packet& packet)
{
  Player player{};
  packet >> player.pid;
  packet >> player.name;
  packet >> player.revision;

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) using {} joined", player.name, player.pid, player.revision);

  {
    std::lock_guard lkp(m_crit.players);
    m_players[player.pid] = player;
  }

  m_dialog->OnPlayerConnect(player.name);

  m_dialog->Update();
}

void NetPlayClient::OnPlayerLeave(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    const auto it = m_players.find(pid);
    if (it == m_players.end())
      return;

    const auto& player = it->second;
    INFO_LOG_FMT(NETPLAY, "Player {} ({}) left", player.name, pid);
    m_dialog->OnPlayerDisconnect(player.name);
    m_players.erase(it);
  }

  m_dialog->Update();
}

void NetPlayClient::OnPlayerReady(sf::Packet& packet, const bool ready)
{
  PlayerId pid = 0;
  packet >> pid;
  if (!packet || !packet.endOfPacket())
    return;

  {
    std::lock_guard lkp(m_crit.players);
    const auto it = m_players.find(pid);
    if (it == m_players.end())
      return;
    it->second.ready = ready;
  }
  m_dialog->Update();
}

void NetPlayClient::OnChatMessage(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;
  std::string msg;
  packet >> msg;

  // don't need lock to read in this thread
  const Player& player = m_players[pid];

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) wrote: {}", player.name, player.pid, msg);

  // add to gui
  m_dialog->AppendChat(fmt::format("{}[{}]: {}", player.name, pid, msg));
}

void NetPlayClient::OnChunkedDataStart(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;
  std::string title;
  packet >> title;
  const u64 data_size = Common::PacketReadU64(packet);

  INFO_LOG_FMT(NETPLAY, "Starting data chunk {}.", cid);

  m_chunked_data_receive_queue.emplace(cid, sf::Packet{});

  std::vector<int> players;
  players.push_back(m_local_player->pid);
  m_dialog->ShowChunkedProgressDialog(title, data_size, players);
}

void NetPlayClient::OnChunkedDataEnd(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto data_packet_iter = m_chunked_data_receive_queue.find(cid);
  if (data_packet_iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  INFO_LOG_FMT(NETPLAY, "Ending data chunk {}.", cid);

  auto& data_packet = data_packet_iter->second;
  OnData(data_packet, CHUNKED_DATA_CHANNEL);
  m_chunked_data_receive_queue.erase(data_packet_iter);
  m_dialog->HideChunkedProgressDialog();

  sf::Packet complete_packet;
  complete_packet << MessageID::ChunkedDataComplete;
  complete_packet << cid;
  Send(complete_packet, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::OnChunkedDataPayload(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto data_packet_iter = m_chunked_data_receive_queue.find(cid);
  if (data_packet_iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  auto& data_packet = data_packet_iter->second;
  while (!packet.endOfPacket())
  {
    u8 byte;
    packet >> byte;
    data_packet << byte;
  }

  INFO_LOG_FMT(NETPLAY, "Received {} bytes of data chunk {}.", data_packet.getDataSize(), cid);

  m_dialog->SetChunkedProgress(m_local_player->pid, data_packet.getDataSize());

  sf::Packet progress_packet;
  progress_packet << MessageID::ChunkedDataProgress;
  progress_packet << cid;
  progress_packet << u64{data_packet.getDataSize()};
  Send(progress_packet, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::OnChunkedDataAbort(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto iter = m_chunked_data_receive_queue.find(cid);
  if (iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  INFO_LOG_FMT(NETPLAY, "Aborting data chunk {}.", cid);

  m_chunked_data_receive_queue.erase(iter);
  m_dialog->HideChunkedProgressDialog();
}

void NetPlayClient::OnPadMapping(sf::Packet& packet)
{
  std::lock_guard game_guard(m_crit.game);
  for (PlayerId& mapping : m_pad_map)
    packet >> mapping;

  UpdateDevices();

  m_dialog->Update();
}

void NetPlayClient::OnWiimoteMapping(sf::Packet& packet)
{
  std::lock_guard game_guard(m_crit.game);
  for (PlayerId& mapping : m_wiimote_map)
    packet >> mapping;

  m_dialog->Update();
}

void NetPlayClient::OnGBAConfig(sf::Packet& packet)
{
  std::lock_guard game_guard(m_crit.game);
  for (size_t i = 0; i < m_gba_config.size(); ++i)
  {
    auto& config = m_gba_config[i];
    const auto old_config = config;

    packet >> config.enabled >> config.has_rom >> config.title;
    for (auto& data : config.hash)
      packet >> data;

    if (std::tie(config.has_rom, config.title, config.hash) !=
        std::tie(old_config.has_rom, old_config.title, old_config.hash))
    {
      m_dialog->OnMsgChangeGBARom(static_cast<int>(i), config);
      m_net_settings.gba_rom_paths[i] =
          config.has_rom ?
              m_dialog->FindGBARomPath(config.hash, config.title, static_cast<int>(i)) :
              "";
    }
  }

  SendGameStatus();
  UpdateDevices();

  m_dialog->Update();
}

void NetPlayClient::OnPadData(sf::Packet& packet)
{
  std::lock_guard game_guard(m_crit.game);
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    GCPadStatus pad;
    packet >> pad.button;
    if (static_cast<size_t>(map) < m_gba_config.size() && !m_gba_config.at(map).enabled)
    {
      packet >> pad.analogA >> pad.analogB >> pad.stickX >> pad.stickY >> pad.substickX >>
          pad.substickY >> pad.triggerLeft >> pad.triggerRight >> pad.isConnected;
    }

    if (static_cast<size_t>(map) < m_pad_buffer.size())
    {
      m_pad_buffer.at(map).Push(pad);
      m_gc_pad_event.Set();
    }
  }
}

void NetPlayClient::OnPadHostData(sf::Packet& packet)
{
  std::lock_guard game_guard(m_crit.game);
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    GCPadStatus pad;
    packet >> pad.button;
    if (static_cast<size_t>(map) < m_gba_config.size() && !m_gba_config.at(map).enabled)
    {
      packet >> pad.analogA >> pad.analogB >> pad.stickX >> pad.stickY >> pad.substickX >>
          pad.substickY >> pad.triggerLeft >> pad.triggerRight >> pad.isConnected;
    }

    if (static_cast<size_t>(map) < m_last_pad_status.size())
      m_last_pad_status[map] = pad;

    if (static_cast<size_t>(map) < m_first_pad_status_received.size())
    {
      if (!m_first_pad_status_received[map])
      {
        m_first_pad_status_received[map] = true;
        m_first_pad_status_received_event.Set();
      }
    }
  }
}

void NetPlayClient::OnWiimoteData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    WiimoteEmu::SerializedWiimoteState pad;
    packet >> pad.length;
    ASSERT(pad.length <= pad.data.size());
    if (pad.length <= pad.data.size())
    {
      for (size_t i = 0; i < pad.length; ++i)
        packet >> pad.data[i];
    }
    else
    {
      pad.length = 0;
    }

    if (static_cast<size_t>(map) < m_wiimote_buffer.size())
    {
      m_wiimote_buffer.at(map).Push(pad);
      m_wii_pad_event.Set();
    }
  }
}

void NetPlayClient::OnPadBuffer(sf::Packet& packet)
{
  u32 size = 0;
  packet >> size;

  m_target_buffer_size = size;
  m_dialog->OnPadBufferChanged(size);
}

void NetPlayClient::OnHostInputAuthority(sf::Packet& packet)
{
  packet >> m_host_input_authority;
  m_dialog->OnHostInputAuthorityChanged(m_host_input_authority);
}

void NetPlayClient::OnRollbackCapabilityQuery(sf::Packet& packet)
{
  u16 maximum_version = 0;
  packet >> maximum_version;
  if (!packet || !packet.endOfPacket())
    return;

  sf::Packet response;
  response << MessageID::RollbackCapability;
  if (m_advertise_rollback_capability && maximum_version >= ROLLBACK_NETPLAY_VERSION)
  {
    response << ROLLBACK_NETPLAY_VERSION << ROLLBACK_NETPLAY_MAX_HORIZON;
  }
  else
  {
    response << u16{0} << u16{0};
  }
  Send(response);
}

bool NetPlayClient::OnRollbackSession(sf::Packet& packet)
{
  u8 enabled = 0;
  RollbackNetplaySession session;
  packet >> enabled >> session.protocol_version >> session.generation >>
      session.base_delay_samples >> session.rollback_horizon_frames;
  session.enabled = enabled != 0;
  if (!packet || !packet.endOfPacket() || enabled > 1 || !IsValidRollbackNetplaySession(session) ||
      (session.enabled && !m_advertise_rollback_capability))
  {
    return false;
  }

  std::lock_guard guard(m_crit.rollback);
  m_rollback_session = session;
  m_rollback_protocol_fault.Clear();
  if (session.enabled)
  {
    std::fprintf(stderr,
                 "[rollback live] negotiated generation=%llu base_delay_samples=%u "
                 "horizon_frames=%u\n",
                 static_cast<unsigned long long>(session.generation), session.base_delay_samples,
                 session.rollback_horizon_frames);
    std::fflush(stderr);
  }
  return true;
}

bool NetPlayClient::OnRollbackSIInput(sf::Packet& packet)
{
  u16 payload_size = 0;
  packet >> payload_size;
  if (!packet || payload_size == 0 || payload_size > ROLLBACK_SI_MAX_PACKET_SIZE)
    return false;

  std::array<u8, ROLLBACK_SI_MAX_PACKET_SIZE> payload{};
  for (std::size_t i = 0; i < payload_size; ++i)
    packet >> payload[i];
  if (!packet || !packet.endOfPacket())
    return false;

  RollbackNetplaySession session;
  {
    std::lock_guard guard(m_crit.rollback);
    session = m_rollback_session;
  }
  RollbackSIInputDecodeResult decoded = DecodeRollbackSIInputPacket(
      std::span<const u8>(payload.data(), payload_size), 0);
  if (!decoded)
    return false;
  // The unreliable channel may deliver a valid datagram from the preceding
  // match after StopGame or after a new generation starts. Generation is the
  // session discriminator: retire stale input instead of faulting the current
  // match. Malformed datagrams still fail closed above.
  if (!session.enabled || decoded.packet.session_generation != session.generation)
    return true;
  if (m_rollback_input_queue.Size() >= 64)
    return false;

  // Packets can deliberately overlap: each sender may include up to eight
  // recent batches so an unreliable loss is repaired by the next datagram.
  // RollbackSIInputJournal performs per-batch duplicate/conflict detection.
  m_rollback_input_queue.Push(std::move(decoded.packet));
  m_rollback_input_event.Set();
  return true;
}

void NetPlayClient::FailRollbackProtocol()
{
  m_rollback_protocol_fault.Set();
  m_rollback_input_event.Set();
  std::lock_guard guard(m_crit.rollback);
  m_rollback_session = {};
}

void NetPlayClient::OnGolfSwitch(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  const PlayerId previous_golfer = m_current_golfer;
  m_current_golfer = pid;
  m_dialog->OnGolferChanged(m_local_player->pid == pid, pid != 0 ? m_players[pid].name : "");

  if (m_local_player->pid == previous_golfer)
  {
    sf::Packet spac;
    spac << MessageID::GolfRelease;
    Send(spac);
  }
  else if (m_local_player->pid == pid)
  {
    sf::Packet spac;
    spac << MessageID::GolfAcquire;
    Send(spac);

    // Pads are already calibrated so we can just ignore this
    m_first_pad_status_received.fill(true);

    m_wait_on_input = false;
    m_wait_on_input_event.Set();
  }
}

void NetPlayClient::OnGolfPrepare(sf::Packet& packet)
{
  m_wait_on_input_received = true;
  m_wait_on_input = true;
}

void NetPlayClient::OnChangeGame(sf::Packet& packet)
{
  {
    std::lock_guard guard(m_crit.rollback);
    m_rollback_session = {};
  }
  std::string netplay_name;
  {
    std::lock_guard lkg(m_crit.game);
    ReceiveSyncIdentifier(packet, m_selected_game);
    packet >> netplay_name;
  }

  INFO_LOG_FMT(NETPLAY, "Game changed to {}", netplay_name);

  // update gui
  m_dialog->OnMsgChangeGame(m_selected_game, netplay_name);

  SendGameStatus();

  sf::Packet client_capabilities_packet;
  client_capabilities_packet << MessageID::ClientCapabilities;
  client_capabilities_packet << ExpansionInterface::CEXIIPL::HasIPLDump();
  client_capabilities_packet << Config::Get(Config::SESSION_USE_FMA);
  Send(client_capabilities_packet);
}

void NetPlayClient::OnGameStatus(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    packet >> m_players[pid].game_status;
  }

  m_dialog->Update();
}

void NetPlayClient::OnStartGame(sf::Packet& packet)
{
  {
    std::lock_guard lkg(m_crit.game);

    INFO_LOG_FMT(NETPLAY, "Start of game {}", m_selected_game.game_id);

    packet >> m_current_game;
    packet >> m_net_settings.cpu_thread;
    packet >> m_net_settings.cpu_core;
    packet >> m_net_settings.enable_cheats;
    packet >> m_net_settings.enable_hardcore;
    packet >> m_net_settings.selected_language;
    packet >> m_net_settings.override_region_settings;
    packet >> m_net_settings.dsp_enable_jit;
    packet >> m_net_settings.dsp_hle;
    packet >> m_net_settings.ram_override_enable;
    packet >> m_net_settings.mem1_size;
    packet >> m_net_settings.mem2_size;
    packet >> m_net_settings.fallback_region;
    packet >> m_net_settings.allow_sd_writes;
    packet >> m_net_settings.oc_enable;
    packet >> m_net_settings.oc_factor;
    packet >> m_net_settings.vi_oc_enable;
    packet >> m_net_settings.vi_oc_factor;

    for (auto slot : ExpansionInterface::SLOTS)
      packet >> m_net_settings.exi_device[slot];

    packet >> m_net_settings.memcard_size_override;

    for (u32& value : m_net_settings.sysconf_settings)
      packet >> value;

    packet >> m_net_settings.efb_access_enable;
    packet >> m_net_settings.bbox_enable;
    packet >> m_net_settings.force_progressive;
    packet >> m_net_settings.efb_to_texture_enable;
    packet >> m_net_settings.xfb_to_texture_enable;
    packet >> m_net_settings.disable_copy_to_vram;
    packet >> m_net_settings.immediate_xfb_enable;
    packet >> m_net_settings.efb_emulate_format_changes;
    packet >> m_net_settings.safe_texture_cache_color_samples;
    packet >> m_net_settings.perf_queries_enable;
    packet >> m_net_settings.float_exceptions;
    packet >> m_net_settings.divide_by_zero_exceptions;
    packet >> m_net_settings.fprf;
    packet >> m_net_settings.accurate_nans;
    packet >> m_net_settings.disable_icache;
    packet >> m_net_settings.sync_on_skip_idle;
    packet >> m_net_settings.sync_gpu;
    packet >> m_net_settings.sync_gpu_max_distance;
    packet >> m_net_settings.sync_gpu_min_distance;
    packet >> m_net_settings.sync_gpu_overclock;
    packet >> m_net_settings.jit_follow_branch;
    packet >> m_net_settings.fast_disc_speed;
    packet >> m_net_settings.mmu;
    packet >> m_net_settings.fastmem;
    packet >> m_net_settings.skip_ipl;
    packet >> m_net_settings.load_ipl_dump;
    packet >> m_net_settings.vertex_rounding;
    packet >> m_net_settings.internal_resolution;
    packet >> m_net_settings.efb_scaled_copy;
    packet >> m_net_settings.fast_depth_calc;
    packet >> m_net_settings.enable_pixel_lighting;
    packet >> m_net_settings.widescreen_hack;
    packet >> m_net_settings.force_texture_filtering;
    packet >> m_net_settings.max_anisotropy;
    packet >> m_net_settings.force_true_color;
    packet >> m_net_settings.disable_copy_filter;
    packet >> m_net_settings.disable_fog;
    packet >> m_net_settings.arbitrary_mipmap_detection;
    packet >> m_net_settings.arbitrary_mipmap_detection_threshold;
    packet >> m_net_settings.enable_gpu_texture_decoding;
    packet >> m_net_settings.defer_efb_copies;
    packet >> m_net_settings.efb_access_tile_size;
    packet >> m_net_settings.efb_access_defer_invalidation;
    packet >> m_net_settings.savedata_load;
    packet >> m_net_settings.savedata_write;
    packet >> m_net_settings.savedata_sync_all_wii;
    if (!m_net_settings.savedata_load)
    {
      m_net_settings.savedata_write = false;
      m_net_settings.savedata_sync_all_wii = false;
    }
    if (GetRollbackNetplaySession().enabled)
    {
      // Rollback has no confirmed-frame persistence journal yet. Keep the
      // synchronized save visible to the guest, but never copy writes back to
      // the user's save or writable SD storage for this session.
      m_net_settings.savedata_write = false;
      m_net_settings.allow_sd_writes = false;
      m_net_settings.exi_device[ExpansionInterface::Slot::SP1] =
          ExpansionInterface::EXIDeviceType::None;
      m_net_settings.exi_device[ExpansionInterface::Slot::SP2] =
          ExpansionInterface::EXIDeviceType::None;
    }
    packet >> m_net_settings.strict_settings_sync;

    m_initial_rtc = Common::PacketReadU64(packet);

    packet >> m_net_settings.save_data_region;
    packet >> m_net_settings.sync_codes;

    packet >> m_net_settings.golf_mode;
    packet >> m_net_settings.use_fma;
    packet >> m_net_settings.hide_remote_gbas;

    for (size_t i = 0; i < sizeof(m_net_settings.sram); ++i)
      packet >> m_net_settings.sram[i];

    m_net_settings.is_hosting = m_local_player->IsHost();
  }

  m_dialog->OnMsgStartGame();
}

void NetPlayClient::OnStopGame(sf::Packet& packet)
{
  INFO_LOG_FMT(NETPLAY, "Game stopped");

  {
    std::lock_guard guard(m_crit.rollback);
    m_rollback_session = {};
  }

  StopGame();
  m_dialog->OnMsgStopGame();
}

void NetPlayClient::OnPowerButton()
{
  InvokeStop();
  m_dialog->OnMsgPowerButton();
}

void NetPlayClient::OnPing(sf::Packet& packet)
{
  u32 ping_key = 0;
  packet >> ping_key;

  sf::Packet response_packet;
  response_packet << MessageID::Pong;
  response_packet << ping_key;

  Send(response_packet);
}

void NetPlayClient::OnPlayerPingData(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    Player& player = m_players[pid];
    packet >> player.ping;
  }

  DisplayPlayersPing();
  m_dialog->Update();
}

void NetPlayClient::OnDesyncDetected(sf::Packet& packet)
{
  int pid_to_blame;
  u32 frame;
  packet >> pid_to_blame;
  packet >> frame;

  std::string player = "??";
  std::lock_guard lkp(m_crit.players);
  {
    const auto it = m_players.find(pid_to_blame);
    if (it != m_players.end())
      player = it->second.name;
  }

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) desynced!", player, pid_to_blame);

  m_dialog->OnDesync(frame, player);
}

void NetPlayClient::Send(const sf::Packet& packet, const u8 channel_id)
{
  const Common::ENet::PacketDelivery delivery =
      channel_id == ROLLBACK_INPUT_CHANNEL ? Common::ENet::PacketDelivery::UnreliableSequenced :
                                             Common::ENet::PacketDelivery::Reliable;
  Common::ENet::SendPacket(m_server, packet, channel_id, delivery);
}

u64 NetPlayClient::GetInitialRTCValue() const
{
  return m_initial_rtc;
}

void NetPlayClient::DisplayPlayersPing()
{
  if (!Config::Get(Config::GFX_SHOW_NETPLAY_PING))
    return;

  const RollbackPerformanceSnapshot rollback =
      m_rollback_performance_stats.Snapshot(Common::Timer::NowMs());
  const u32 depth = std::max(rollback.current_depth_frames, rollback.recent_peak_depth_frames);
  const u32 color = depth >= 4 ? OSD::Color::RED :
                    depth > 0  ? OSD::Color::YELLOW :
                                 OSD::Color::CYAN;
  OSD::AddTypedMessage(OSD::MessageType::NetPlayPing,
                       fmt::format("Ping: {} ms  |  Rollback: {}f", GetPlayersMaxPing(), depth),
                       OSD::Duration::SHORT, color);
}

u32 NetPlayClient::GetPlayersMaxPing()
{
  std::lock_guard lkp(m_crit.players);
  if (m_players.empty())
    return 0;
  return std::ranges::max_element(m_players, {}, [](const auto& kv) { return kv.second.ping; })
      ->second.ping;
}

void NetPlayClient::Disconnect()
{
  ENetEvent netEvent;
  m_connecting = false;
  m_connection_state = ConnectionState::Failure;
  if (m_server)
    enet_peer_disconnect(m_server, 0);
  else
    return;

  while (enet_host_service(m_client, &netEvent, 3000) > 0)
  {
    switch (netEvent.type)
    {
    case ENET_EVENT_TYPE_RECEIVE:
      enet_packet_destroy(netEvent.packet);
      break;
    case ENET_EVENT_TYPE_DISCONNECT:
      m_server = nullptr;
      return;
    default:
      break;
    }
  }
  // didn't disconnect gracefully force disconnect
  enet_peer_reset(m_server);
  m_server = nullptr;
}

void NetPlayClient::SendAsync(sf::Packet&& packet, const u8 channel_id)
{
  {
    std::lock_guard lkq(m_crit.async_queue_write);
    m_async_queue.Push(AsyncQueueEntry{std::move(packet), channel_id});
  }
  Common::ENet::WakeupThread(m_client);
}

// called from ---NETPLAY--- thread
void NetPlayClient::ThreadFunc()
{
  INFO_LOG_FMT(NETPLAY, "NetPlayClient starting.");

  Common::QoSSession qos_session;
  if (Config::Get(Config::NETPLAY_ENABLE_QOS))
  {
    qos_session = Common::QoSSession(m_server);

    if (qos_session.Successful())
    {
      m_dialog->AppendChat(
          Common::GetStringT("Quality of Service (QoS) was successfully enabled."));
    }
    else
    {
      m_dialog->AppendChat(Common::GetStringT("Quality of Service (QoS) couldn't be enabled."));
    }
  }

  while (m_do_loop.IsSet())
  {
    ENetEvent netEvent;
    int net;
    if (m_traversal_client)
      m_traversal_client->HandleResends();
    net = enet_host_service(m_client, &netEvent, 250);
    while (!m_async_queue.Empty())
    {
      INFO_LOG_FMT(NETPLAY, "Processing async queue event.");
      {
        auto& e = m_async_queue.Front();
        Send(e.packet, e.channel_id);
      }
      INFO_LOG_FMT(NETPLAY, "Processing async queue event done.");
      m_async_queue.Pop();
    }
    if (net > 0)
    {
      sf::Packet rpac;
      switch (netEvent.type)
      {
      case ENET_EVENT_TYPE_CONNECT:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: connect event");
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: receive event");

        rpac.append(netEvent.packet->data, netEvent.packet->dataLength);
        OnData(rpac, netEvent.channelID);

        enet_packet_destroy(netEvent.packet);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: disconnect event");

        m_dialog->OnConnectionLost();

        if (m_is_running.IsSet())
          StopGame();

        break;
      default:
        // not a valid switch case due to not technically being part of the enum
        if (static_cast<int>(netEvent.type) == Common::ENet::SKIPPABLE_EVENT)
          INFO_LOG_FMT(NETPLAY, "enet_host_service: skippable packet event");
        else
          ERROR_LOG_FMT(NETPLAY, "enet_host_service: unknown event type: {}", int(netEvent.type));
        break;
      }
    }
    else if (net == 0)
    {
      INFO_LOG_FMT(NETPLAY, "enet_host_service: no event occurred");
    }
    else
    {
      ERROR_LOG_FMT(NETPLAY, "enet_host_service error: {}", net);
    }
  }

  INFO_LOG_FMT(NETPLAY, "NetPlayClient shutting down.");

  Disconnect();
  return;
}

// called from ---GUI--- thread
std::vector<Player> NetPlayClient::GetPlayers()
{
  std::lock_guard lkp(m_crit.players);
  std::vector<Player> players;
  players.reserve(m_players.size());

  for (const auto& pair : m_players)
    players.push_back(pair.second);

  return players;
}

const NetSettings& NetPlayClient::GetNetSettings() const
{
  return m_net_settings;
}

RollbackNetplaySession NetPlayClient::GetRollbackNetplaySession() const
{
  std::lock_guard guard(m_crit.rollback);
  return m_rollback_session;
}

bool NetPlayClient::LoadRollbackFaultScript()
{
  const char* const test_ack = std::getenv("RINGOUT_ROLLBACK_TEST_ACK");
  const char* const production_ack = std::getenv("RINGOUT_ROLLBACK_FAULT_ACK");
  const char* const path = std::getenv("RINGOUT_ROLLBACK_FAULT_SCRIPT");
  if (path == nullptr)
    return true;
  const bool isolated_test =
      test_ack != nullptr && std::string_view(test_ack) == "HEADLESS_ISOLATED";
  const bool production_gate_test = production_ack != nullptr &&
                                    std::string_view(production_ack) ==
                                        "PRODUCTION_OUTPUT_GATE";
  if (!isolated_test && !production_gate_test)
    return false;

  if (const char* const arm_file = std::getenv("RINGOUT_ROLLBACK_FAULT_ARM_FILE");
      arm_file != nullptr)
  {
    if (!isolated_test || *arm_file == '\0')
      return false;
    m_rollback_fault_arm_file = arm_file;
  }

  std::ifstream script(path);
  if (!script)
    return false;

  std::string line;
  while (std::getline(script, line))
  {
    if (const std::size_t comment = line.find('#'); comment != std::string::npos)
      line.erase(comment);
    if (line.find_first_not_of(" \t\r") == std::string::npos)
      continue;
    if (m_rollback_fault_actions.size() >= 64)
      return false;

    std::istringstream fields(line);
    std::string operation;
    RollbackFaultAction action;
    fields >> operation >> action.send_ordinal;
    if (!fields || action.send_ordinal == 0)
      return false;
    if (operation == "drop")
    {
      action.drop = true;
    }
    else if (operation == "delay")
    {
      fields >> action.release_after_send_ordinal;
      if (!fields || action.release_after_send_ordinal <= action.send_ordinal)
        return false;
    }
    else
    {
      return false;
    }
    fields >> std::ws;
    if (!fields.eof())
      return false;
    m_rollback_fault_actions.push_back(action);
  }

  std::ranges::sort(m_rollback_fault_actions, {}, &RollbackFaultAction::send_ordinal);
  return std::adjacent_find(m_rollback_fault_actions.begin(), m_rollback_fault_actions.end(),
                            [](const auto& lhs, const auto& rhs) {
                              return lhs.send_ordinal == rhs.send_ordinal;
                            }) == m_rollback_fault_actions.end();
}

bool NetPlayClient::SendRollbackPacketWithFaults(sf::Packet&& packet,
                                                 const RollbackSIInputPacket& input)
{
  if (!m_rollback_fault_arm_file.empty() && !m_rollback_fault_armed)
  {
    if (!File::Exists(m_rollback_fault_arm_file))
    {
      SendAsync(std::move(packet), ROLLBACK_INPUT_CHANNEL);
      return true;
    }
    m_rollback_fault_armed = true;
    std::fprintf(stderr, "[rollback fault] armed file=%s\n",
                 m_rollback_fault_arm_file.c_str());
  }
  if (m_rollback_send_ordinal == std::numeric_limits<u64>::max())
    return false;
  const u64 ordinal = ++m_rollback_send_ordinal;
  const auto action = std::ranges::lower_bound(m_rollback_fault_actions, ordinal, {},
                                               &RollbackFaultAction::send_ordinal);

  bool send_current = true;
  if (action != m_rollback_fault_actions.end() && action->send_ordinal == ordinal)
  {
    const u64 first_batch = input.batch_count == 0 ? 0 : input.batches[0].batch_id;
    const u64 last_batch =
        input.batch_count == 0 ? 0 : input.batches[input.batch_count - 1].batch_id;
    u16 buttons = 0;
    for (std::size_t batch = 0; batch < input.batch_count; ++batch)
    {
      for (std::size_t pad = 0; pad < input.batches[batch].pads.size(); ++pad)
      {
        if ((input.batches[batch].pad_mask & (u8{1} << pad)) != 0)
          buttons |= input.batches[batch].pads[pad].button;
      }
    }
    std::fprintf(stderr,
                 "[rollback fault] action=%s send_ordinal=%llu release_after=%llu "
                 "batches=%zu first_batch=%llu last_batch=%llu buttons=0x%04x\n",
                 action->drop ? "drop" : "delay", static_cast<unsigned long long>(ordinal),
                 static_cast<unsigned long long>(action->release_after_send_ordinal),
                 input.batch_count, static_cast<unsigned long long>(first_batch),
                 static_cast<unsigned long long>(last_batch), buttons);
    for (std::size_t batch = 0; batch < input.batch_count; ++batch)
    {
      for (std::size_t pad = 0; pad < input.batches[batch].pads.size(); ++pad)
      {
        if ((input.batches[batch].pad_mask & (u8{1} << pad)) == 0)
          continue;
        std::fprintf(stderr,
                     "[rollback fault batch] send_ordinal=%llu batch=%llu pad=%zu "
                     "buttons=0x%04x stick=%u,%u connected=%s\n",
                     static_cast<unsigned long long>(ordinal),
                     static_cast<unsigned long long>(input.batches[batch].batch_id), pad,
                     input.batches[batch].pads[pad].button,
                     input.batches[batch].pads[pad].stickX,
                     input.batches[batch].pads[pad].stickY,
                     input.batches[batch].pads[pad].isConnected ? "yes" : "no");
      }
    }
    if (action->drop)
    {
      send_current = false;
    }
    else
    {
      if (m_delayed_rollback_packets.size() >= 64)
        return false;
      m_delayed_rollback_packets.push_back(
          {.release_after_send_ordinal = action->release_after_send_ordinal,
           .packet = std::move(packet)});
      send_current = false;
    }
  }

  if (send_current)
    SendAsync(std::move(packet), ROLLBACK_INPUT_CHANNEL);

  // Queue the delayed datagram after the current ordinal.  This deliberately
  // gives it a newer ENet sequence while retaining its older RSIB batch IDs,
  // modeling a late authoritative correction without sleeping a network
  // thread or perturbing control traffic.
  for (auto delayed = m_delayed_rollback_packets.begin();
       delayed != m_delayed_rollback_packets.end();)
  {
    if (delayed->release_after_send_ordinal == ordinal)
    {
      SendAsync(std::move(delayed->packet), ROLLBACK_INPUT_CHANNEL);
      delayed = m_delayed_rollback_packets.erase(delayed);
    }
    else
    {
      ++delayed;
    }
  }
  return true;
}

bool NetPlayClient::SendRollbackSIInput(const RollbackSIInputPacket& input)
{
  const RollbackNetplaySession session = GetRollbackNetplaySession();
  // Replay consumes already journaled batches. Any outbound RSIB from a
  // historical frame would duplicate input and can create a correction loop;
  // fail closed even if a future scheduler regression supplies a packet.
  if (!IsLiveRollbackReplayDerivedOutboundAllowed() || !m_is_connected || !session.enabled ||
      input.protocol_version != ROLLBACK_SI_INPUT_VERSION ||
      input.session_generation != session.generation)
  {
    return false;
  }

  std::array<u8, ROLLBACK_SI_MAX_PACKET_SIZE> payload{};
  const RollbackSIInputEncodeResult encoded = EncodeRollbackSIInputPacket(input, payload);
  if (!encoded)
    return false;

  sf::Packet packet;
  packet << MessageID::RollbackSIInput << static_cast<u16>(encoded.size);
  for (std::size_t i = 0; i < encoded.size; ++i)
    packet << payload[i];
  return SendRollbackPacketWithFaults(std::move(packet), input);
}

bool NetPlayClient::SendRollbackStateDigest(const RollbackStateDigest& digest)
{
  const RollbackNetplaySession session = GetRollbackNetplaySession();
  if (!IsLiveRollbackReplayDerivedOutboundAllowed() || !m_is_connected || !session.enabled ||
      digest.session_generation != session.generation)
    return false;

  std::array<u8, ROLLBACK_STATE_DIGEST_PACKET_SIZE> payload{};
  const std::optional<std::size_t> encoded = EncodeRollbackStateDigest(digest, payload);
  if (!encoded || *encoded != payload.size())
    return false;

  sf::Packet packet;
  packet << MessageID::RollbackStateDigest << static_cast<u16>(*encoded);
  for (const u8 byte : payload)
    packet << byte;
  // Periodic state evidence is required, not latency-sensitive. Reliable
  // default-channel ordering makes a missing checkpoint a meaningful fault.
  SendAsync(std::move(packet), DEFAULT_CHANNEL);
  return true;
}

bool NetPlayClient::TryPopRollbackSIInput(RollbackSIInputPacket* const packet)
{
  return packet != nullptr && m_rollback_input_queue.Pop(*packet);
}

bool NetPlayClient::WaitForRollbackSIInput(const std::chrono::milliseconds timeout)
{
  if (!m_is_running.IsSet())
    return false;
  if (!m_rollback_input_queue.Empty())
    return true;
  if (!m_rollback_input_event.WaitFor(timeout))
    return false;
  return m_is_running.IsSet() && !m_rollback_input_queue.Empty();
}

// called from ---GUI--- thread
void NetPlayClient::SendChatMessage(const std::string& msg)
{
  sf::Packet packet;
  packet << MessageID::ChatMessage;
  packet << msg;

  SendAsync(std::move(packet));
}

void NetPlayClient::SetReady(const bool ready)
{
  if (!m_is_connected || m_is_running.IsSet())
    return;

  sf::Packet packet;
  packet << (ready ? MessageID::Ready : MessageID::NotReady);
  SendAsync(std::move(packet));
}

// called from ---CPU--- thread
void NetPlayClient::AddPadStateToPacket(const int in_game_pad, const GCPadStatus& pad,
                                        sf::Packet& packet)
{
  std::lock_guard game_guard(m_crit.game);
  packet << static_cast<PadIndex>(in_game_pad);
  packet << pad.button;
  if (!m_gba_config[in_game_pad].enabled)
  {
    packet << pad.analogA << pad.analogB << pad.stickX << pad.stickY << pad.substickX
           << pad.substickY << pad.triggerLeft << pad.triggerRight << pad.isConnected;
  }
}

// called from ---CPU--- thread
void NetPlayClient::AddWiimoteStateToPacket(int in_game_pad,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  packet << static_cast<PadIndex>(in_game_pad);
  packet << state.length;
  for (size_t i = 0; i < state.length; ++i)
    packet << state.data[i];
}

// called from ---GUI--- thread
void NetPlayClient::SendStartGamePacket()
{
  sf::Packet packet;
  packet << MessageID::StartGame;
  packet << m_current_game;

  SendAsync(std::move(packet));
}

// called from ---GUI--- thread
void NetPlayClient::SendStopGamePacket()
{
  sf::Packet packet;
  packet << MessageID::StopGame;

  SendAsync(std::move(packet));
}

// called from ---GUI--- thread
bool NetPlayClient::StartGame(const std::string& path)
{
  std::lock_guard lkg(m_crit.game);
  if (m_is_running.IsSet())
  {
    PanicAlertFmtT("Game is already running!");
    return false;
  }

  // A NetPlayClient can host more than one match. Rollback objects carry a
  // generation, journal, snapshots, and poll cursor and therefore must never be
  // reused across StartGame boundaries.
  {
    std::lock_guard lk(crit_netplay_client);
    ResetLiveRollbackImpl();

    const RollbackNetplaySession rollback_session = GetRollbackNetplaySession();
    if (rollback_session.enabled)
    {
      const bool headless_isolated = [] {
        const char* const acknowledgement = std::getenv("RINGOUT_ROLLBACK_TEST_ACK");
        return acknowledgement != nullptr &&
               std::string_view(acknowledgement) == "HEADLESS_ISOLATED";
      }();

      if (headless_isolated)
      {
        m_pending_live_rollback_output_gate =
            LiveRollbackOutputGate::CreateHeadlessIsolatedTestGate();
      }
      else
      {
        const auto is_safe_memcard_slot = [this](const ExpansionInterface::Slot slot) {
          return IsLiveRollbackMemoryCardSlotSafe(m_net_settings.exi_device[slot]);
        };
        const bool gba_devices_disabled =
            std::ranges::none_of(m_gba_config, [](const GBAConfig& config) { return config.enabled; });
        const LiveRollbackProductionSessionPolicy policy{
            // RingOut boots an extracted main.dol, whose GameFile platform is
            // ELFOrDOL even though the runtime already inspected and accepted
            // its source disc identity. Use that explicit trusted preflight
            // rather than misclassifying every production session here.
            .is_gamecube_title = m_rollback_gamecube_title_verified,
            .save_data_writable = m_net_settings.savedata_write,
            .sd_writes_allowed = m_net_settings.allow_sd_writes,
            .memory_card_slots_safe =
                is_safe_memcard_slot(ExpansionInterface::Slot::A) &&
                is_safe_memcard_slot(ExpansionInterface::Slot::B),
            .serial_port_1_disabled =
                m_net_settings.exi_device[ExpansionInterface::Slot::SP1] ==
                ExpansionInterface::EXIDeviceType::None,
            .serial_port_2_disabled =
                m_net_settings.exi_device[ExpansionInterface::Slot::SP2] ==
                ExpansionInterface::EXIDeviceType::None,
            .gba_devices_disabled = gba_devices_disabled,
        };
        m_pending_live_rollback_output_gate =
            LiveRollbackOutputGate::CreateProductionGate(policy);
      }

      if (!m_pending_live_rollback_output_gate ||
          !m_pending_live_rollback_output_gate->BeginSessionQuarantine())
      {
        m_pending_live_rollback_output_gate.reset();
        std::fprintf(stderr,
                     "[rollback live] start refused: production capability/session policy unsafe\n");
        return false;
      }
    }
  }
  SendStartGamePacket();
  m_rollback_protocol_fault.Clear();
  RollbackSIInputPacket stale_rollback_input;
  while (m_rollback_input_queue.Pop(stale_rollback_input))
  {
  }
  m_rollback_input_event.Reset();
  m_delayed_rollback_packets.clear();
  m_rollback_fault_armed = false;
  m_rollback_send_ordinal = 0;

  m_timebase_frame = 0;
  m_current_golfer = 1;
  m_wait_on_input = false;

  m_is_running.Set();
  NetPlay_Enable(this);

  ClearBuffers();

  m_first_pad_status_received.fill(false);

  if (m_dialog->IsRecording() && !GetRollbackNetplaySession().enabled)
  {
    auto& movie = Core::System::GetInstance().GetMovie();
    if (movie.IsReadOnly())
      movie.SetReadOnly(false);

    Movie::ControllerTypeArray controllers{};
    Movie::WiimoteEnabledArray wiimotes{};
    for (unsigned int i = 0; i < 4; ++i)
    {
      if (m_pad_map[i] > 0 && m_gba_config[i].enabled)
        controllers[i] = Movie::ControllerType::GBA;
      else if (m_pad_map[i] > 0)
        controllers[i] = Movie::ControllerType::GC;
      else
        controllers[i] = Movie::ControllerType::None;
      wiimotes[i] = m_wiimote_map[i] > 0;
    }
    movie.BeginRecordingInput(controllers, wiimotes);
  }

  for (unsigned int i = 0; i < 4; ++i)
  {
    Config::SetCurrent(Config::GetInfoForWiimoteSource(i),
                       m_wiimote_map[i] > 0 ? WiimoteSource::Emulated : WiimoteSource::None);
  }

  // boot game
  auto boot_session_data = std::make_unique<BootSessionData>();

  INFO_LOG_FMT(NETPLAY,
               "Setting Wii sync data: has FS {}, sync_titles = {:016x}, redirect folder = {}",
               !!m_wii_sync_fs, fmt::join(m_wii_sync_titles, ", "), m_wii_sync_redirect_folder);

  boot_session_data->SetWiiSyncData(std::move(m_wii_sync_fs), std::move(m_wii_sync_titles),
                                    std::move(m_wii_sync_redirect_folder), [] {
                                      // on emulation end clean up the Wii save sync directory --
                                      // see OnSyncSaveDataWii()
                                      const std::string wii_path = File::GetUserPath(D_USER_IDX) +
                                                                   "Wii" GC_MEMCARD_NETPLAY DIR_SEP;
                                      if (File::Exists(wii_path))
                                        File::DeleteDirRecursively(wii_path);
                                      const std::string redirect_path =
                                          File::GetUserPath(D_USER_IDX) +
                                          "Redirect" GC_MEMCARD_NETPLAY DIR_SEP;
                                      if (File::Exists(redirect_path))
                                        File::DeleteDirRecursively(redirect_path);
                                    });
  boot_session_data->SetNetplaySettings(std::make_unique<NetPlay::NetSettings>(m_net_settings));

  m_dialog->BootGame(path, std::move(boot_session_data));

  UpdateDevices();

  return true;
}

// called from ---GUI--- thread
bool NetPlayClient::ChangeGame(const std::string&)
{
  return true;
}

// called from ---NETPLAY--- thread
void NetPlayClient::UpdateDevices()
{
  u8 local_pad = 0;
  u8 pad = 0;

  auto& si = Core::System::GetInstance().GetSerialInterface();
  for (auto player_id : m_pad_map)
  {
    const SerialInterface::SIDevices si_device = Config::Get(Config::GetInfoForSIDevice(local_pad));

    if (m_gba_config[pad].enabled && player_id > 0)
    {
      si.ChangeDevice(SerialInterface::SIDEVICE_GC_GBA_EMULATED, pad);
    }
    else if (player_id == m_local_player->pid)
    {
      // Use local controller types for local controllers if they are compatible
      if (SerialInterface::SIDevice_IsGCController(si_device))
      {
        si.ChangeDevice(si_device, pad);

        if (si_device == SerialInterface::SIDEVICE_WIIU_ADAPTER)
        {
          GCAdapter::ResetDeviceType(local_pad);
        }
      }
      else
      {
        si.ChangeDevice(SerialInterface::SIDEVICE_GC_CONTROLLER, pad);
      }
      local_pad++;
    }
    else if (player_id > 0)
    {
      if (si_device != SerialInterface::SIDEVICE_AM_BASEBOARD)
        si.ChangeDevice(SerialInterface::SIDEVICE_GC_CONTROLLER, pad);
    }
    else
    {
      si.ChangeDevice(SerialInterface::SIDEVICE_NONE, pad);
    }
    pad++;
  }
}

// called from ---NETPLAY--- thread
void NetPlayClient::ClearBuffers()
{
  // clear pad buffers, Clear method isn't thread safe
  for (unsigned int i = 0; i < 4; ++i)
  {
    while (m_pad_buffer[i].Size())
      m_pad_buffer[i].Pop();

    while (m_wiimote_buffer[i].Size())
      m_wiimote_buffer[i].Pop();
  }
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnTraversalStateChanged()
{
  const Common::TraversalClient::State state = m_traversal_client->GetState();

  if (m_connection_state == ConnectionState::WaitingForTraversalClientConnection &&
      state == Common::TraversalClient::State::Connected)
  {
    m_connection_state = ConnectionState::WaitingForTraversalClientConnectReady;
    m_traversal_client->ConnectToClient(m_host_spec);
  }
  else if (m_connection_state != ConnectionState::Connected &&
           m_connection_state != ConnectionState::Failure &&
           state == Common::TraversalClient::State::Failure)
  {
    m_connection_error = ConnectionError::TraversalServiceUnavailable;
    Disconnect();
    m_dialog->OnTraversalError(m_traversal_client->GetFailureReason());
  }
  m_dialog->OnTraversalStateChanged(state);
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnConnectReady(ENetAddress addr)
{
  if (m_connection_state == ConnectionState::WaitingForTraversalClientConnectReady)
  {
    INFO_LOG_FMT(NETPLAY, "Traversal supplied ENet peer {:08x}:{}.", addr.host, addr.port);
    m_connection_state = ConnectionState::Connecting;
    enet_host_connect(m_client, &addr, CHANNEL_COUNT, 0);
  }
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnConnectFailed(Common::TraversalConnectFailedReason reason)
{
  m_connecting = false;
  m_connection_state = ConnectionState::Failure;
  switch (reason)
  {
  case Common::TraversalConnectFailedReason::ClientDidntRespond:
    m_connection_error = ConnectionError::TraversalFailed;
    m_dialog->OnConnectionError(_trans("The room host did not respond to the traversal service."));
    break;
  case Common::TraversalConnectFailedReason::ClientFailure:
    m_connection_error = ConnectionError::TraversalFailed;
    m_dialog->OnConnectionError(_trans("The room host rejected the traversal attempt."));
    break;
  case Common::TraversalConnectFailedReason::NoSuchClient:
    m_connection_error = ConnectionError::RoomNotFound;
    m_dialog->OnConnectionError(_trans("That room code does not exist or has expired."));
    break;
  default:
    m_connection_error = ConnectionError::TraversalFailed;
    m_dialog->OnConnectionError(_trans("The traversal service could not connect this room."));
    break;
  }
}

// called from ---CPU--- thread
void NetPlayClient::InvokeStop()
{
  m_is_running.Clear();

  // stop waiting for input
  m_gc_pad_event.Set();
  m_wii_pad_event.Set();
  m_first_pad_status_received_event.Set();
  m_wait_on_input_event.Set();
  m_rollback_input_event.Set();
}

// called from ---GUI--- thread and ---NETPLAY--- thread (client side)
bool NetPlayClient::StopGame()
{
  InvokeStop();

  NetPlay_Disable();

  // stop game
  m_dialog->StopGame();

  return true;
}

// called from ---GUI--- thread
void NetPlayClient::Stop()
{
  if (!m_is_running.IsSet())
    return;

  InvokeStop();

  // Tell the server to stop if we have a pad mapped in game.
  if (LocalPlayerHasControllerMapped())
    SendStopGamePacket();
  else
    StopGame();
}

void NetPlayClient::RequestStopGame()
{
  // A local production preflight can reject StartGame after the server has
  // already broadcast it, before m_is_running is set. Notify every peer even
  // in that narrow lobby state; Stop() intentionally cannot do so.
  if (m_is_connected)
    SendStopGamePacket();
}

void NetPlayClient::SendPowerButtonEvent()
{
  sf::Packet packet;
  packet << MessageID::PowerButton;
  SendAsync(std::move(packet));
}

void NetPlayClient::RequestGolfControl(const PlayerId pid)
{
  if (!m_host_input_authority || !m_net_settings.golf_mode)
    return;

  sf::Packet packet;
  packet << MessageID::GolfRequest;
  packet << pid;
  SendAsync(std::move(packet));
}

void NetPlayClient::RequestGolfControl()
{
  RequestGolfControl(m_local_player->pid);
}

// called from ---GUI--- thread
std::string NetPlayClient::GetCurrentGolfer()
{
  std::lock_guard lkp(m_crit.players);
  if (const auto it = m_players.find(m_current_golfer); it != m_players.end())
    return it->second.name;
  return "";
}

// called from ---GUI--- thread
bool NetPlayClient::LocalPlayerHasControllerMapped() const
{
  std::lock_guard game_guard(m_crit.game);
  return PlayerHasControllerMapped(m_local_player->pid);
}

bool NetPlayClient::IsFirstInGamePad(int ingame_pad) const
{
  std::lock_guard game_guard(m_crit.game);
  return std::none_of(m_pad_map.begin(), m_pad_map.begin() + ingame_pad,
                      [](auto mapping) { return mapping > 0; });
}

int NetPlayClient::NumLocalPads() const
{
  std::lock_guard game_guard(m_crit.game);
  return std::ranges::count(m_pad_map, m_local_player->pid);
}

int NetPlayClient::NumLocalWiimotes() const
{
  std::lock_guard game_guard(m_crit.game);
  return std::ranges::count(m_wiimote_map, m_local_player->pid);
}

static int InGameToLocal(int ingame_pad, const PadMappingArray& pad_map, PlayerId local_player_pid)
{
  // not our pad
  if (pad_map[ingame_pad] != local_player_pid)
    return 4;

  int local_pad = 0;
  int pad = 0;

  for (; pad < ingame_pad; ++pad)
  {
    if (pad_map[pad] == local_player_pid)
      local_pad++;
  }

  return local_pad;
}

static int LocalToInGame(int local_pad, const PadMappingArray& pad_map, PlayerId local_player_pid)
{
  // Figure out which in-game pad maps to which local pad.
  // The logic we have here is that the local slots always
  // go in order.
  int local_pad_count = -1;
  int ingame_pad = 0;
  for (; ingame_pad < 4; ingame_pad++)
  {
    if (pad_map[ingame_pad] == local_player_pid)
      local_pad_count++;

    if (local_pad_count == local_pad)
      break;
  }

  return ingame_pad;
}

int NetPlayClient::InGamePadToLocalPad(int ingame_pad) const
{
  std::lock_guard game_guard(m_crit.game);
  return InGameToLocal(ingame_pad, m_pad_map, m_local_player->pid);
}

int NetPlayClient::LocalPadToInGamePad(int local_pad) const
{
  std::lock_guard game_guard(m_crit.game);
  return LocalToInGame(local_pad, m_pad_map, m_local_player->pid);
}

int NetPlayClient::InGameWiimoteToLocalWiimote(int ingame_wiimote) const
{
  std::lock_guard game_guard(m_crit.game);
  return InGameToLocal(ingame_wiimote, m_wiimote_map, m_local_player->pid);
}

int NetPlayClient::LocalWiimoteToInGameWiimote(int local_wiimote) const
{
  std::lock_guard game_guard(m_crit.game);
  return LocalToInGame(local_wiimote, m_wiimote_map, m_local_player->pid);
}

bool NetPlayClient::PlayerHasControllerMapped(const PlayerId pid) const
{
  std::lock_guard game_guard(m_crit.game);
  const auto mapping_matches_player_id = [pid](const PlayerId& mapping) { return mapping == pid; };

  return std::ranges::any_of(m_pad_map, mapping_matches_player_id) ||
         std::ranges::any_of(m_wiimote_map, mapping_matches_player_id);
}

bool NetPlayClient::IsLocalPlayer(const PlayerId pid) const
{
  return pid == m_local_player->pid;
}

const PlayerId& NetPlayClient::GetLocalPlayerId() const
{
  return m_local_player->pid;
}

void NetPlayClient::SendGameStatus()
{
  std::lock_guard game_guard(m_crit.game);
  sf::Packet packet;
  packet << MessageID::GameStatus;

  SyncIdentifierComparison result;
  m_dialog->FindGameFile(m_selected_game, &result);
  for (size_t i = 0; i < 4; ++i)
  {
    if (m_gba_config[i].enabled && m_gba_config[i].has_rom &&
        m_net_settings.gba_rom_paths[i].empty())
    {
      result = SyncIdentifierComparison::DifferentGame;
    }
  }

  packet << result;
  Send(packet);
}

void NetPlayClient::SendTimeBase()
{
  std::lock_guard lk(crit_netplay_client);

  if (netplay_client->m_timebase_frame % 60 == 0)
  {
    const u64 timebase = Core::System::GetInstance().GetSystemTimers().GetFakeTimeBase();

    sf::Packet packet;
    packet << MessageID::TimeBase;
    packet << timebase;
    packet << netplay_client->m_timebase_frame;

    netplay_client->SendAsync(std::move(packet));
  }

  netplay_client->m_timebase_frame++;
}

bool NetPlayClient::UpdateLiveRollbackFrameBoundary()
{
  std::lock_guard lk(crit_netplay_client);
  return netplay_client == nullptr || netplay_client->UpdateLiveRollbackFrameBoundaryImpl();
}

bool NetPlayClient::IsLiveRollbackSessionActive()
{
  std::lock_guard lk(crit_netplay_client);
  return netplay_client != nullptr && netplay_client->IsLiveRollbackSessionActiveImpl();
}

std::optional<Sc2LiveRollbackCorrection> NetPlayClient::ClaimSc2LiveRollbackCorrection()
{
  std::lock_guard lk(crit_netplay_client);
  return netplay_client ? netplay_client->ClaimSc2LiveRollbackCorrectionImpl() : std::nullopt;
}

bool NetPlayClient::ResolveSc2LiveRollbackPolls(
    const std::span<const Sc2EngineInputPoll> captured,
    std::vector<Sc2EngineInputPoll>* const authoritative)
{
  std::lock_guard lk(crit_netplay_client);
  return netplay_client &&
         netplay_client->ResolveSc2LiveRollbackPollsImpl(captured, authoritative);
}

bool NetPlayClient::CommitSc2LiveRollbackCorrection(
    const Sc2LiveRollbackCorrection& correction)
{
  std::lock_guard lk(crit_netplay_client);
  return netplay_client && netplay_client->CommitSc2LiveRollbackCorrectionImpl(correction);
}

void NetPlayClient::CancelSc2LiveRollbackCorrection()
{
  std::lock_guard lk(crit_netplay_client);
  if (netplay_client)
    netplay_client->CancelSc2LiveRollbackCorrectionImpl();
}

std::optional<Sc2LiveRollbackCorrection> ClaimSc2LiveRollbackCorrection()
{
  return NetPlayClient::ClaimSc2LiveRollbackCorrection();
}

bool ResolveSc2LiveRollbackPolls(const std::span<const Sc2EngineInputPoll> captured,
                                 std::vector<Sc2EngineInputPoll>* const authoritative)
{
  return NetPlayClient::ResolveSc2LiveRollbackPolls(captured, authoritative);
}

bool CommitSc2LiveRollbackCorrection(const Sc2LiveRollbackCorrection& correction)
{
  return NetPlayClient::CommitSc2LiveRollbackCorrection(correction);
}

void CancelSc2LiveRollbackCorrection()
{
  NetPlayClient::CancelSc2LiveRollbackCorrection();
}

bool NetPlayClient::DoAllPlayersHaveGame()
{
  std::lock_guard lkp(m_crit.players);

  return std::ranges::all_of(m_players, [](const auto& entry) {
    return entry.second.game_status == SyncIdentifierComparison::SameGame;
  });
}

PadMappingArray NetPlayClient::GetPadMapping() const
{
  std::lock_guard game_guard(m_crit.game);
  return m_pad_map;
}

GBAConfigArray NetPlayClient::GetGBAConfig() const
{
  std::lock_guard game_guard(m_crit.game);
  return m_gba_config;
}

PadMappingArray NetPlayClient::GetWiimoteMapping() const
{
  std::lock_guard game_guard(m_crit.game);
  return m_wiimote_map;
}

void NetPlayClient::AdjustPadBufferSize(const unsigned int size)
{
  m_target_buffer_size = size;
  m_dialog->OnPadBufferChanged(size);
}

void NetPlayClient::SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs,
                                   std::vector<u64> titles, std::string redirect_folder)
{
  m_wii_sync_fs = std::move(fs);
  m_wii_sync_titles = std::move(titles);
  m_wii_sync_redirect_folder = std::move(redirect_folder);
}

SyncIdentifier NetPlayClient::GetSDCardIdentifier()
{
  return SyncIdentifier{{}, "sd", {}, {}, {}, {}};
}

std::string GetPlayerMappingString(PlayerId pid, const PadMappingArray& pad_map,
                                   const GBAConfigArray& gba_config,
                                   const PadMappingArray& wiimote_map)
{
  std::vector<size_t> gc_slots, gba_slots, wiimote_slots;
  for (size_t i = 0; i < pad_map.size(); ++i)
  {
    if (pad_map[i] == pid && !gba_config[i].enabled)
      gc_slots.push_back(i + 1);
    if (pad_map[i] == pid && gba_config[i].enabled)
      gba_slots.push_back(i + 1);
    if (wiimote_map[i] == pid)
      wiimote_slots.push_back(i + 1);
  }
  std::vector<std::string> groups;
  std::array<std::pair<std::string, std::vector<size_t>*>, 3> slot_groups = {
      {{"GC", &gc_slots}, {"GBA", &gba_slots}, {"Wii", &wiimote_slots}}};

  for (const auto& [group_name, slots] : slot_groups)
  {
    if (!slots->empty())
      groups.emplace_back(fmt::format("{}{}", group_name, fmt::join(*slots, ",")));
  }
  std::string res = fmt::format("{}", fmt::join(groups, "|"));
  return res.empty() ? "None" : res;
}

bool IsNetPlayRunning()
{
  return netplay_client != nullptr;
}

void SetSIPollBatching(bool state)
{
  s_si_poll_batching = state;
}

void SendPowerButtonEvent()
{
  ASSERT(IsNetPlayRunning());
  netplay_client->SendPowerButtonEvent();
}

std::string GetGBASavePath(int pad_num)
{
  std::lock_guard lk(crit_netplay_client);

  if (!netplay_client || netplay_client->GetNetSettings().is_hosting)
  {
#ifdef HAS_LIBMGBA
    std::string rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[pad_num]);
    return HW::GBA::Core::GetSavePath(rom_path, pad_num);
#else
    return {};
#endif
  }

  if (!netplay_client->GetNetSettings().savedata_load)
    return {};

  return fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX), GBA_SAVE_NETPLAY, pad_num + 1);
}

PadDetails GetPadDetails(int pad_num)
{
  std::lock_guard lk(crit_netplay_client);

  PadDetails res{};
  res.local_pad = 4;
  if (!netplay_client)
    return res;

  auto pad_map = netplay_client->GetPadMapping();
  if (pad_map[pad_num] <= 0)
    return res;

  for (const auto& player : netplay_client->GetPlayers())
  {
    if (player.pid == pad_map[pad_num])
      res.player_name = player.name;
  }

  int local_pad = 0;
  int non_local_pad = 0;
  for (int i = 0; i < pad_num; ++i)
  {
    if (netplay_client->IsLocalPlayer(pad_map[i]))
      ++local_pad;
    else
      ++non_local_pad;
  }
  res.is_local = netplay_client->IsLocalPlayer(pad_map[pad_num]);
  res.local_pad = res.is_local ? local_pad : netplay_client->NumLocalPads() + non_local_pad;
  res.hide_gba = !res.is_local && netplay_client->GetNetSettings().hide_remote_gbas &&
                 netplay_client->LocalPlayerHasControllerMapped();
  return res;
}

int NumLocalWiimotes()
{
  std::lock_guard lk(crit_netplay_client);
  if (netplay_client)
    return netplay_client->NumLocalWiimotes();
  return 0;
}

void NetPlay_Enable(NetPlayClient* const np)
{
  std::lock_guard lk(crit_netplay_client);
  netplay_client = np;
}

void NetPlay_Disable()
{
  // This mutex is the lifecycle barrier used by NetPlay_GetInput and
  // UpdateLiveRollbackFrameBoundary. Once acquired, no CPU callback can retain
  // a reference while the owner thread cancels and destroys rollback state.
  std::lock_guard lk(crit_netplay_client);
  if (netplay_client)
    netplay_client->ResetLiveRollbackImpl();
  netplay_client = nullptr;
}
}  // namespace NetPlay

// stuff hacked into dolphin

// called from ---CPU--- thread
// Actual Core function which is called on every frame
bool SerialInterface::CSIDevice_GCController::NetPlay_GetInput(int pad_num, GCPadStatus* status)
{
  bool replayed_result = false;
  if (NetPlay::TryReplaySc2EngineInput(pad_num, NetPlay::s_si_poll_batching, status,
                                      &replayed_result))
  {
    return replayed_result;
  }

  std::lock_guard lk(NetPlay::crit_netplay_client);
  NetPlay::SetSc2EngineInputBatch(std::nullopt);

  bool result = false;
  if (NetPlay::netplay_client)
    result = NetPlay::netplay_client->GetNetPads(pad_num, NetPlay::s_si_poll_batching, status);

  NetPlay::RecordSc2EngineInput(pad_num, NetPlay::s_si_poll_batching, *status, result);
  return result;
}

bool NetPlay::NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries)
{
  std::lock_guard lk(crit_netplay_client);

  if (netplay_client)
    return netplay_client->WiimoteUpdate(entries);

  return false;
}

unsigned int NetPlay::NetPlay_GetLocalWiimoteForSlot(unsigned int slot)
{
  if (slot >= std::tuple_size_v<PadMappingArray>)
    return slot;

  std::lock_guard lk(crit_netplay_client);

  if (!netplay_client)
    return slot;

  const auto mapping = netplay_client->GetWiimoteMapping();
  const auto& local_player_id = netplay_client->GetLocalPlayerId();

  std::array<unsigned int, std::tuple_size_v<std::decay_t<decltype(mapping)>>> slot_map;
  size_t player_count = 0;
  for (size_t i = 0; i < mapping.size(); ++i)
  {
    if (mapping[i] == local_player_id)
    {
      slot_map[i] = static_cast<unsigned int>(player_count);
      ++player_count;
    }
  }
  for (size_t i = 0; i < mapping.size(); ++i)
  {
    if (mapping[i] != local_player_id)
    {
      slot_map[i] = static_cast<unsigned int>(player_count);
      ++player_count;
    }
  }

  INFO_LOG_FMT(NETPLAY, "Wiimote slot map: [{}]", fmt::join(slot_map, ", "));

  return slot_map[slot];
}

// called from ---CPU--- thread
// so all players' games get the same time
//
// also called from ---GUI--- thread when starting input recording
u64 ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime()
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->GetInitialRTCValue();

  return 0;
}

// called from ---CPU--- thread
// return the local pad num that should rumble given a ingame pad num
int SerialInterface::CSIDevice_GCController::NetPlay_InGamePadToLocalPad(int pad_num)
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->InGamePadToLocalPad(pad_num);

  return pad_num;
}
