// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/Sc2RollbackProfile.h"

#include <array>

namespace NetPlay
{
namespace
{
constexpr std::string_view GRSEAF_REV0_DOL_SHA256 =
    "0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5";
constexpr std::array<RollbackRegionSnapshotRing::Region, 0> DISCOVERY_REGIONS{};
}  // namespace

std::optional<Sc2RollbackProfile> FindSc2RollbackProfile(const std::string_view disc_id,
                                                         const std::string_view dol_sha256)
{
  if (disc_id != "GRSEAF" || dol_sha256 != GRSEAF_REV0_DOL_SHA256)
    return std::nullopt;
  return Sc2RollbackProfile{
      .id = "sc2-grseaf-rev0-discovery-v1",
      .disc_id = "GRSEAF",
      .dol_sha256 = GRSEAF_REV0_DOL_SHA256,
      .version = 1,
      .status = Sc2RollbackProfileStatus::DiscoveryOnly,
      .hooks = {},
      .state_regions = DISCOVERY_REGIONS,
  };
}

bool IsSc2RollbackProfileCertified(const Sc2RollbackProfile& profile)
{
  const Sc2RollbackHookPoints& hooks = profile.hooks;
  return profile.status == Sc2RollbackProfileStatus::Certified && profile.version != 0 &&
         hooks.update_begin != 0 && hooks.update_end != 0 && hooks.input_read != 0 &&
         hooks.render_begin != 0 && hooks.audio_dispatch != 0 && hooks.persistence_dispatch != 0 &&
         !profile.state_regions.empty();
}

}  // namespace NetPlay
