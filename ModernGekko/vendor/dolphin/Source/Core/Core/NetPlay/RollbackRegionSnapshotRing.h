// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace NetPlay
{

// Preallocated, memcpy-only checkpoint ring for a game-specific rollback
// profile. Regions omitted from the profile are deliberately preserved on
// restore (network transport, renderer, audio, and persistence ownership stay
// with the host). This is not selected by production until a DOL-specific
// profile has passed continuous resimulation tests.
class RollbackRegionSnapshotRing final
{
public:
  struct Region
  {
    std::size_t offset = 0;
    std::size_t size = 0;
  };

  enum class ConfigurationStatus : std::uint8_t
  {
    Valid,
    ZeroCapacity,
    EmptyRegions,
    InvalidRegion,
    OverlappingRegions,
  };

  RollbackRegionSnapshotRing(std::size_t memory_size, std::vector<Region> regions,
                             std::size_t capacity, std::size_t auxiliary_state_size = 0);

  ConfigurationStatus GetConfigurationStatus() const { return m_status; }
  std::size_t GetSnapshotSize() const { return m_snapshot_size; }
  std::size_t GetCapacity() const { return m_slots.size(); }

  bool Capture(std::uint64_t frame, std::span<const std::uint8_t> memory,
               std::span<const std::uint8_t> auxiliary_state = {});
  bool HasFrame(std::uint64_t frame) const;
  bool Restore(std::uint64_t frame, std::span<std::uint8_t> memory,
               std::span<std::uint8_t> auxiliary_state = {}) const;

private:
  struct Slot
  {
    std::vector<std::uint8_t> bytes;
    std::optional<std::uint64_t> frame;
  };

  const Slot* Find(std::uint64_t frame) const;

  std::size_t m_memory_size;
  std::vector<Region> m_regions;
  std::size_t m_auxiliary_state_size;
  std::size_t m_snapshot_size = 0;
  ConfigurationStatus m_status;
  std::vector<Slot> m_slots;
};

}  // namespace NetPlay
