// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/NetPlayClient.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/ranges.h>
#include <string>
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/SFMLHelper.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Host.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/InputConfig.h"

namespace NetPlay
{

bool NetPlayClient::GetNetPads(const int pad_nb, const bool batching, GCPadStatus* pad_status)
{
  while (m_wait_on_input)
  {
    if (!m_is_running.IsSet())
    {
      return false;
    }

    if (m_wait_on_input_received)
    {
      sf::Packet spac;
      spac << MessageID::GolfPrepare;
      Send(spac);

      m_wait_on_input_received = false;
    }

    m_wait_on_input_event.Wait();
  }

  bool rollback_handled = false;
  const bool rollback_result = GetLiveRollbackPads(pad_nb, batching, pad_status, &rollback_handled);
  if (rollback_handled)
    return rollback_result;

  if (IsFirstInGamePad(pad_nb) && batching)
  {
    sf::Packet packet;
    packet << MessageID::PadData;

    bool send_packet = false;
    const int num_local_pads = NumLocalPads();
    for (int local_pad = 0; local_pad < num_local_pads; local_pad++)
    {
      send_packet = PollLocalPad(local_pad, packet) || send_packet;
    }

    if (send_packet)
      SendAsync(std::move(packet));

    if (m_host_input_authority)
      SendPadHostPoll(-1);
  }

  if (!batching)
  {
    const int local_pad = InGamePadToLocalPad(pad_nb);
    if (local_pad < 4)
    {
      sf::Packet packet;
      packet << MessageID::PadData;
      if (PollLocalPad(local_pad, packet))
        SendAsync(std::move(packet));
    }

    if (m_host_input_authority)
      SendPadHostPoll(pad_nb);
  }

  if (m_host_input_authority)
  {
    if (m_local_player->pid != m_current_golfer)
    {
      const bool buffer_over_target = m_pad_buffer[pad_nb].Size() > m_target_buffer_size + 1;
      if (!buffer_over_target)
        m_buffer_under_target_last = std::chrono::steady_clock::now();

      std::chrono::duration<double> time_diff =
          std::chrono::steady_clock::now() - m_buffer_under_target_last;
      if (time_diff.count() >= 1.0 || !buffer_over_target)
      {
        Config::SetCurrent(Config::MAIN_EMULATION_SPEED, buffer_over_target ? 0.0f : 1.0f);
      }
    }
    else
    {
      Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
    }
  }

  while (m_pad_buffer[pad_nb].Size() == 0)
  {
    if (!m_is_running.IsSet())
    {
      return false;
    }

    m_gc_pad_event.Wait();
  }

  m_pad_buffer[pad_nb].Pop(*pad_status);

  // RINGOUT_NETPLAY_PADLOG=1: does each in-game port actually receive input?
  // Config, pad map and routing can all be provably correct and the game still
  // report an empty port 2 -- this is the layer below all of them, showing what
  // the game is handed per port. A port whose buttons never change is receiving
  // nothing, whoever it was assigned to.
  {
    static const bool s_padlog = std::getenv("RINGOUT_NETPLAY_PADLOG") != nullptr;
    if (s_padlog && pad_nb < 4)
    {
      static int s_count[4] = {};
      static u16 s_last[4] = {};
      if (s_count[pad_nb] < 12 || pad_status->button != s_last[pad_nb])
      {
        s_last[pad_nb] = pad_status->button;
        ++s_count[pad_nb];
        std::fprintf(stderr, "[padlog] in-game pad %d: buttons=0x%04x stick=(%u,%u)\n", pad_nb + 1,
                     pad_status->button, pad_status->stickX, pad_status->stickY);
        std::fflush(stderr);
      }
    }
  }

  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsRecordingInput())
  {
    movie.RecordInput(pad_status, pad_nb);
    movie.InputUpdate();
  }
  else
  {
    movie.CheckPadStatus(pad_status, pad_nb);
  }

  return true;
}

bool NetPlayClient::WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries)
{
  for (const WiimoteDataBatchEntry& entry : entries)
  {
    const int local_wiimote = InGameWiimoteToLocalWiimote(entry.wiimote);
    DEBUG_LOG_FMT(NETPLAY,
                  "Entering WiimoteUpdate() with wiimote {}, local_wiimote {}, state [{:02x}]",
                  entry.wiimote, local_wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));
    if (local_wiimote < 4)
    {
      sf::Packet packet;
      packet << MessageID::WiimoteData;
      if (AddLocalWiimoteToBuffer(local_wiimote, *entry.state, packet))
        SendAsync(std::move(packet));
    }

    while (m_wiimote_buffer[entry.wiimote].Size() == 0)
    {
      if (!m_is_running.IsSet())
      {
        return false;
      }

      m_wii_pad_event.Wait();
    }

    m_wiimote_buffer[entry.wiimote].Pop(*entry.state);

    DEBUG_LOG_FMT(NETPLAY, "Exiting WiimoteUpdate() with wiimote {}, state [{:02x}]", entry.wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));
  }

  return true;
}

bool NetPlayClient::PollLocalPad(const int local_pad, sf::Packet& packet)
{
  const int ingame_pad = LocalPadToInGamePad(local_pad);
  bool data_added = false;
  const GCPadStatus pad_status = SampleLocalPad(local_pad);

  // Send side. The receive-side padlog shows what each port is HANDED; this shows
  // what this peer READ from its own controller before sending. A peer whose
  // port stays neutral while its routing is correct is either reading nothing
  // locally (input acquisition) or failing to transmit -- only this tells them
  // apart.
  {
    static const bool s_padlog = std::getenv("RINGOUT_NETPLAY_PADLOG") != nullptr;
    if (s_padlog)
    {
      static int s_count = 0;
      static u16 s_last = 0;
      if (s_count < 12 || pad_status.button != s_last)
      {
        s_last = pad_status.button;
        ++s_count;
        std::fprintf(
            stderr, "[padlog] SEND local pad %d -> in-game pad %d: buttons=0x%04x stick=(%u,%u)\n",
            local_pad, ingame_pad + 1, pad_status.button, pad_status.stickX, pad_status.stickY);
        std::fflush(stderr);
      }
    }
  }

  if (m_host_input_authority)
  {
    if (m_local_player->pid != m_current_golfer)
    {
      AddPadStateToPacket(ingame_pad, pad_status, packet);
      data_added = true;
    }
    else
    {
      m_last_pad_status[ingame_pad] = pad_status;
      m_first_pad_status_received[ingame_pad] = true;
    }
  }
  else
  {
    while (m_pad_buffer[ingame_pad].Size() <= m_target_buffer_size)
    {
      m_pad_buffer[ingame_pad].Push(pad_status);
      AddPadStateToPacket(ingame_pad, pad_status, packet);
      data_added = true;
    }
  }

  return data_added;
}

GCPadStatus NetPlayClient::SampleLocalPad(const int local_pad) const
{
  const int ingame_pad = LocalPadToInGamePad(local_pad);
  GCPadStatus pad_status{};

  // The active netplay overlay cannot pause only one peer, so emulation keeps
  // running while its D-pad/A/B navigation is read directly by the host loop.
  // Do not send those same physical inputs into the game. The ordinary input
  // gate intentionally permits background input in netplay, making this
  // explicit neutral packet the only reliable UI capture point for every GC
  // pad source (configured pad, GBA and adapter).
  if (Host_UIBlocksControllerState())
  {
    pad_status = {};
  }
  else if (m_gba_config[ingame_pad].enabled)
  {
    pad_status = Pad::GetGBAStatus(local_pad);
  }
  else if (Config::Get(Config::GetInfoForSIDevice(local_pad)) ==
           SerialInterface::SIDEVICE_WIIU_ADAPTER)
  {
    pad_status = GCAdapter::Input(local_pad);
  }
  else
  {
    pad_status = Pad::GetStatus(local_pad);
  }

  // This one-time diagnostic belongs at the acquisition point so both fixed
  // delay and rollback report the same physical-device evidence.
  {
    static const bool s_padlog = std::getenv("RINGOUT_NETPLAY_PADLOG") != nullptr;
    if (s_padlog)
    {
      static bool s_reported = false;
      if (!s_reported)
      {
        s_reported = true;
        // Which device is this pad actually bound to, asked at the moment it is
        // read -- asking earlier reports "no pad config", because controllers
        // are initialised inside Runtime::Create. An empty device here is the
        // difference between "the pad is unbound" and "the pad is bound but
        // reports nothing" (focus / background input).
        std::string device = "<none>";
        if (InputConfig* const cfg = Pad::GetConfig())
        {
          if (cfg->GetControllerCount() > local_pad)
            device = cfg->GetController(local_pad)->GetDefaultDevice().ToString();
        }
        // Measured HERE, not in the lobby: Runtime::Create applies
        // background_input after the lobby logs, so a lobby-time reading shows
        // the stale ini value and means nothing. If the pad is bound and input
        // is still neutral, these two say why.
        std::fprintf(stderr,
                     "[padlog] SEND local pad %d bound to device: %s  "
                     "background_input=%s renderer_focus=%s\n",
                     local_pad, device.c_str(),
                     Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT) ? "true" : "false",
                     Host_RendererHasFocus() ? "true" : "false");
        // GetDefaultDevice() is the string from the ini -- it says what the
        // profile ASKS for, not that such a device exists now. A profile naming
        // a device the interface never enumerated resolves to nothing and reads
        // neutral, with no error anywhere. So: is it actually there, and what IS
        // there?
        {
          bool found = false;
          if (InputConfig* const cfg = Pad::GetConfig())
          {
            if (cfg->GetControllerCount() > local_pad)
              found = g_controller_interface.FindDevice(
                          cfg->GetController(local_pad)->GetDefaultDevice()) != nullptr;
          }
          std::string available;
          for (const std::string& d : g_controller_interface.GetAllDeviceStrings())
            available += (available.empty() ? "" : " | ") + d;
          std::fprintf(stderr, "[padlog] device present in interface: %s; available now: [%s]\n",
                       found ? "YES" : "NO", available.c_str());
        }
      }
      // Per-sample values are logged by the caller with its transport context.
    }
  }
  return pad_status;
}

bool NetPlayClient::AddLocalWiimoteToBuffer(const int local_wiimote,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  const int ingame_pad = LocalWiimoteToInGameWiimote(local_wiimote);
  bool data_added = false;

  while (m_wiimote_buffer[ingame_pad].Size() <= m_target_buffer_size)
  {
    m_wiimote_buffer[ingame_pad].Push(state);
    AddWiimoteStateToPacket(ingame_pad, state, packet);
    data_added = true;
  }

  return data_added;
}

void NetPlayClient::SendPadHostPoll(const PadIndex pad_num)
{
  if (m_local_player->pid != m_current_golfer)
    return;

  sf::Packet packet;
  packet << MessageID::PadHostData;

  if (pad_num < 0)
  {
    for (size_t i = 0; i < m_pad_map.size(); i++)
    {
      if (m_pad_map[i] <= 0)
        continue;

      while (!m_first_pad_status_received[i])
      {
        if (!m_is_running.IsSet())
          return;

        m_first_pad_status_received_event.Wait();
      }
    }

    for (size_t i = 0; i < m_pad_map.size(); i++)
    {
      if (m_pad_map[i] == 0 || m_pad_buffer[i].Size() > 0)
        continue;

      const GCPadStatus& pad_status = m_last_pad_status[i];
      m_pad_buffer[i].Push(pad_status);
      AddPadStateToPacket(static_cast<int>(i), pad_status, packet);
    }
  }
  else if (m_pad_map[pad_num] != 0)
  {
    while (!m_first_pad_status_received[pad_num])
    {
      if (!m_is_running.IsSet())
        return;

      m_first_pad_status_received_event.Wait();
    }

    if (m_pad_buffer[pad_num].Size() == 0)
    {
      const GCPadStatus& pad_status = m_last_pad_status[pad_num];
      m_pad_buffer[pad_num].Push(pad_status);
      AddPadStateToPacket(pad_num, pad_status, packet);
    }
  }

  SendAsync(std::move(packet));
}

}  // namespace NetPlay
