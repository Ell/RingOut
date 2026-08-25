// Copyright 2026 RingOut Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <SFML/Network/Packet.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "Common/CommonTypes.h"

namespace NetPlay
{
// This extension follows Dolphin's three legacy hello fields.  A legacy hello
// ends immediately after the nickname, so fixed-delay-only peers remain wire
// compatible.  Rollback hosts require this extension and fail closed.
constexpr u32 NETPLAY_CONNECT_EXTENSION_MAGIC = 0x524F4345;  // "ROCE"
constexpr u16 NETPLAY_CONNECT_EXTENSION_VERSION = 2;
constexpr std::size_t NETPLAY_COMPATIBILITY_FINGERPRINT_MAX_SIZE = 512;

struct NetPlayConnectExtension
{
  bool rollback_capable = false;
  bool rollback_requested = false;
  std::string compatibility_fingerprint;
};

enum class NetPlayConnectExtensionStatus
{
  Legacy,
  Valid,
  Malformed,
};

inline bool AppendNetPlayConnectExtension(sf::Packet& packet, const bool rollback_capable,
                                          const bool rollback_requested,
                                          const std::string_view compatibility_fingerprint)
{
  if (compatibility_fingerprint.empty() ||
      compatibility_fingerprint.size() > NETPLAY_COMPATIBILITY_FINGERPRINT_MAX_SIZE)
  {
    return false;
  }

  packet << NETPLAY_CONNECT_EXTENSION_MAGIC << NETPLAY_CONNECT_EXTENSION_VERSION
         << static_cast<u8>(rollback_capable ? 1 : 0)
         << static_cast<u8>(rollback_requested ? 1 : 0)
         << static_cast<u16>(compatibility_fingerprint.size());
  for (const char byte : compatibility_fingerprint)
    packet << static_cast<u8>(byte);
  return true;
}

inline NetPlayConnectExtensionStatus
ParseNetPlayConnectExtension(sf::Packet& packet, NetPlayConnectExtension* const extension)
{
  if (packet.endOfPacket())
    return NetPlayConnectExtensionStatus::Legacy;
  if (!extension)
    return NetPlayConnectExtensionStatus::Malformed;

  u32 magic = 0;
  u16 version = 0;
  u8 rollback_capable = 0;
  u8 rollback_requested = 0;
  u16 fingerprint_size = 0;
  packet >> magic >> version >> rollback_capable >> rollback_requested >> fingerprint_size;
  if (!packet || magic != NETPLAY_CONNECT_EXTENSION_MAGIC ||
      version != NETPLAY_CONNECT_EXTENSION_VERSION || rollback_capable > 1 ||
      rollback_requested > 1 || (rollback_requested != 0 && rollback_capable == 0) ||
      fingerprint_size == 0 || fingerprint_size > NETPLAY_COMPATIBILITY_FINGERPRINT_MAX_SIZE)
  {
    return NetPlayConnectExtensionStatus::Malformed;
  }

  std::string fingerprint;
  fingerprint.reserve(fingerprint_size);
  for (u16 i = 0; i < fingerprint_size; ++i)
  {
    u8 byte = 0;
    packet >> byte;
    if (!packet)
      return NetPlayConnectExtensionStatus::Malformed;
    fingerprint.push_back(static_cast<char>(byte));
  }
  if (!packet.endOfPacket())
    return NetPlayConnectExtensionStatus::Malformed;

  extension->rollback_capable = rollback_capable != 0;
  extension->rollback_requested = rollback_requested != 0;
  extension->compatibility_fingerprint = std::move(fingerprint);
  return NetPlayConnectExtensionStatus::Valid;
}
}  // namespace NetPlay
