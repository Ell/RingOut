// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/NetPlayServer.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/SFMLHelper.h"
#include "Core/Config/MainSettings.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif

namespace NetPlay
{

PadMappingArray NetPlayServer::GetPadMapping() const
{
  return m_pad_map;
}

GBAConfigArray NetPlayServer::GetGBAConfig() const
{
  return m_gba_config;
}

PadMappingArray NetPlayServer::GetWiimoteMapping() const
{
  return m_wiimote_map;
}

void NetPlayServer::SetPadMapping(const PadMappingArray& mappings)
{
  m_pad_map = mappings;
  UpdatePadMapping();
}

void NetPlayServer::SetGBAConfig(const GBAConfigArray& configs, bool update_rom)
{
#ifdef HAS_LIBMGBA
  m_gba_config = configs;
  if (update_rom)
  {
    for (size_t i = 0; i < m_gba_config.size(); ++i)
    {
      auto& config = m_gba_config[i];
      if (!config.enabled)
        continue;
      std::string rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[i]);
      config.has_rom = HW::GBA::Core::GetRomInfo(rom_path.c_str(), config.hash, config.title);
    }
  }
#endif
  UpdateGBAConfig();
}

void NetPlayServer::SetWiimoteMapping(const PadMappingArray& mappings)
{
  m_wiimote_map = mappings;
  UpdateWiimoteMapping();
}

void NetPlayServer::UpdatePadMapping()
{
  sf::Packet spac;
  spac << MessageID::PadMapping;
  for (PlayerId mapping : m_pad_map)
  {
    spac << mapping;
  }
  SendToClients(spac);
}

void NetPlayServer::UpdateGBAConfig()
{
  sf::Packet spac;
  spac << MessageID::GBAConfig;
  for (const auto& config : m_gba_config)
  {
    spac << config.enabled << config.has_rom << config.title;
    for (auto& data : config.hash)
      spac << data;
  }
  SendToClients(spac);
}

void NetPlayServer::UpdateWiimoteMapping()
{
  sf::Packet spac;
  spac << MessageID::WiimoteMapping;
  for (PlayerId mapping : m_wiimote_map)
  {
    spac << mapping;
  }
  SendToClients(spac);
}

void NetPlayServer::AdjustPadBufferSize(unsigned int size)
{
  std::lock_guard lkg(m_crit.game);

  m_target_buffer_size = size;

  if (!m_host_input_authority)
  {
    sf::Packet spac;
    spac << MessageID::PadBuffer;
    spac << m_target_buffer_size;

    SendAsyncToClients(std::move(spac));
  }
}

void NetPlayServer::SetHostInputAuthority(const bool enable)
{
  std::lock_guard lkg(m_crit.game);

  m_host_input_authority = enable;

  sf::Packet spac;
  spac << MessageID::HostInputAuthority;
  spac << m_host_input_authority;

  SendAsyncToClients(std::move(spac));

  if (!m_host_input_authority)
    AdjustPadBufferSize(m_target_buffer_size);
}

}  // namespace NetPlay
