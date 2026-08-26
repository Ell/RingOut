// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "Core/NetPlay/RollbackRegionSnapshotRing.h"

namespace NetPlay
{

enum class Sc2RollbackProfileStatus : std::uint8_t
{
  DiscoveryOnly,
  Certified,
};

struct Sc2RollbackHookPoints
{
  std::uint32_t update_begin = 0;
  std::uint32_t update_end = 0;
  std::uint32_t input_read = 0;
  std::uint32_t render_begin = 0;
  std::uint32_t audio_dispatch = 0;
  std::uint32_t persistence_dispatch = 0;
};

struct Sc2RollbackProfile
{
  std::string_view id;
  std::string_view disc_id;
  std::string_view dol_sha256;
  std::uint32_t version = 0;
  Sc2RollbackProfileStatus status = Sc2RollbackProfileStatus::DiscoveryOnly;
  Sc2RollbackHookPoints hooks;
  std::span<const RollbackRegionSnapshotRing::Region> state_regions;
};

// Returns the exact-revision research identity when known. Production callers
// must additionally call IsSc2RollbackProfileCertified; discovery identity is
// deliberately not activation authority.
std::optional<Sc2RollbackProfile> FindSc2RollbackProfile(std::string_view disc_id,
                                                         std::string_view dol_sha256);
bool IsSc2RollbackProfileCertified(const Sc2RollbackProfile& profile);

}  // namespace NetPlay
