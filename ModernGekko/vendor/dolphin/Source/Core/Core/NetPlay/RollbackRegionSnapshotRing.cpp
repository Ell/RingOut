// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackRegionSnapshotRing.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace NetPlay
{

RollbackRegionSnapshotRing::RollbackRegionSnapshotRing(const std::size_t memory_size,
                                                       std::vector<Region> regions,
                                                       const std::size_t capacity,
                                                       const std::size_t auxiliary_state_size)
    : m_memory_size(memory_size), m_regions(std::move(regions)),
      m_auxiliary_state_size(auxiliary_state_size),
      m_status(capacity == 0 ? ConfigurationStatus::ZeroCapacity : ConfigurationStatus::Valid),
      m_slots(capacity)
{
  if (m_status != ConfigurationStatus::Valid)
    return;
  if (m_regions.empty())
  {
    m_status = ConfigurationStatus::EmptyRegions;
    return;
  }

  std::sort(m_regions.begin(), m_regions.end(),
            [](const Region& lhs, const Region& rhs) { return lhs.offset < rhs.offset; });
  std::size_t previous_end = 0;
  bool first = true;
  for (const Region& region : m_regions)
  {
    if (region.size == 0 || region.offset > memory_size ||
        region.size > memory_size - region.offset ||
        region.size > std::numeric_limits<std::size_t>::max() - m_snapshot_size)
    {
      m_status = ConfigurationStatus::InvalidRegion;
      return;
    }
    if (!first && region.offset < previous_end)
    {
      m_status = ConfigurationStatus::OverlappingRegions;
      return;
    }
    first = false;
    previous_end = region.offset + region.size;
    m_snapshot_size += region.size;
  }
  if (m_auxiliary_state_size > std::numeric_limits<std::size_t>::max() - m_snapshot_size)
  {
    m_status = ConfigurationStatus::InvalidRegion;
    return;
  }
  m_snapshot_size += m_auxiliary_state_size;
  for (Slot& slot : m_slots)
    slot.bytes.resize(m_snapshot_size);
}

bool RollbackRegionSnapshotRing::Capture(const std::uint64_t frame,
                                         const std::span<const std::uint8_t> memory,
                                         const std::span<const std::uint8_t> auxiliary_state)
{
  if (m_status != ConfigurationStatus::Valid || memory.size() != m_memory_size ||
      auxiliary_state.size() != m_auxiliary_state_size)
  {
    return false;
  }
  Slot& slot = m_slots[frame % m_slots.size()];
  slot.frame.reset();
  std::size_t cursor = 0;
  for (const Region& region : m_regions)
  {
    std::memcpy(slot.bytes.data() + cursor, memory.data() + region.offset, region.size);
    cursor += region.size;
  }
  if (!auxiliary_state.empty())
    std::memcpy(slot.bytes.data() + cursor, auxiliary_state.data(), auxiliary_state.size());
  slot.frame = frame;
  return true;
}

bool RollbackRegionSnapshotRing::HasFrame(const std::uint64_t frame) const
{
  return Find(frame) != nullptr;
}

bool RollbackRegionSnapshotRing::Restore(const std::uint64_t frame,
                                         const std::span<std::uint8_t> memory,
                                         const std::span<std::uint8_t> auxiliary_state) const
{
  const Slot* const slot = Find(frame);
  if (!slot || memory.size() != m_memory_size || auxiliary_state.size() != m_auxiliary_state_size)
  {
    return false;
  }
  std::size_t cursor = 0;
  for (const Region& region : m_regions)
  {
    std::memcpy(memory.data() + region.offset, slot->bytes.data() + cursor, region.size);
    cursor += region.size;
  }
  if (!auxiliary_state.empty())
    std::memcpy(auxiliary_state.data(), slot->bytes.data() + cursor, auxiliary_state.size());
  return true;
}

const RollbackRegionSnapshotRing::Slot*
RollbackRegionSnapshotRing::Find(const std::uint64_t frame) const
{
  if (m_status != ConfigurationStatus::Valid)
    return nullptr;
  const Slot& slot = m_slots[frame % m_slots.size()];
  return slot.frame == frame ? &slot : nullptr;
}

}  // namespace NetPlay
