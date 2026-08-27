// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackUndoSnapshotRing.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace NetPlay
{

RollbackUndoSnapshotRing::RollbackUndoSnapshotRing(
    const std::size_t memory_size, const std::size_t capacity,
    const std::size_t max_unique_bytes_per_frame, const std::size_t auxiliary_state_size)
    : m_memory_size(memory_size), m_max_unique_bytes_per_frame(max_unique_bytes_per_frame),
      m_auxiliary_state_size(auxiliary_state_size),
      m_status(memory_size == 0 ? ConfigurationStatus::ZeroMemory :
               capacity == 0 ? ConfigurationStatus::ZeroCapacity :
               max_unique_bytes_per_frame == 0 ? ConfigurationStatus::ZeroWriteCapacity :
               memory_size > std::numeric_limits<std::uint32_t>::max() ?
                   ConfigurationStatus::MemoryTooLarge :
                                                 ConfigurationStatus::Valid),
      m_slots(capacity), m_seen_bits((memory_size + 63) / 64)
{
  if (m_status != ConfigurationStatus::Valid)
    return;
  for (Slot& slot : m_slots)
  {
    slot.offsets.reserve(max_unique_bytes_per_frame);
    slot.preimages.reserve(max_unique_bytes_per_frame);
    slot.auxiliary_state.resize(auxiliary_state_size);
  }
  m_touched_seen_words.reserve(std::min(m_seen_bits.size(), max_unique_bytes_per_frame));
}

bool RollbackUndoSnapshotRing::BeginFrame(const std::uint64_t frame,
                                          const std::span<const std::uint8_t> memory,
                                          const std::span<const std::uint8_t> auxiliary_state)
{
  if (m_status != ConfigurationStatus::Valid || memory.size() != m_memory_size ||
      auxiliary_state.size() != m_auxiliary_state_size)
  {
    return false;
  }
  if (m_active_frame)
  {
    if (m_active_overflow || *m_active_frame == std::numeric_limits<std::uint64_t>::max() ||
        frame != *m_active_frame + 1)
    {
      return false;
    }
  }

  ClearSeenBits();
  Slot& slot = m_slots[frame % m_slots.size()];
  slot.frame.reset();
  slot.valid = false;
  slot.offsets.clear();
  slot.preimages.clear();
  if (!auxiliary_state.empty())
    std::memcpy(slot.auxiliary_state.data(), auxiliary_state.data(), auxiliary_state.size());
  slot.frame = frame;
  slot.valid = true;
  m_active_frame = frame;
  m_active_overflow = false;
  return true;
}

bool RollbackUndoSnapshotRing::RecordWrite(const std::size_t offset, const std::size_t size,
                                           const std::span<const std::uint8_t> memory)
{
  if (m_status != ConfigurationStatus::Valid || !m_active_frame || m_active_overflow || size == 0 ||
      memory.size() != m_memory_size || offset > m_memory_size || size > m_memory_size - offset)
  {
    return false;
  }

  Slot& slot = m_slots[*m_active_frame % m_slots.size()];
  if (!slot.valid || slot.frame != m_active_frame)
    return false;
  for (std::size_t byte = offset; byte < offset + size; ++byte)
  {
    const std::size_t word = byte / 64;
    const std::uint64_t mask = std::uint64_t{1} << (byte % 64);
    if ((m_seen_bits[word] & mask) != 0)
      continue;
    if (slot.offsets.size() == m_max_unique_bytes_per_frame)
    {
      slot.valid = false;
      m_active_overflow = true;
      return false;
    }
    if (m_seen_bits[word] == 0)
      m_touched_seen_words.push_back(static_cast<std::uint32_t>(word));
    m_seen_bits[word] |= mask;
    slot.offsets.push_back(static_cast<std::uint32_t>(byte));
    slot.preimages.push_back(memory[byte]);
  }
  return true;
}

bool RollbackUndoSnapshotRing::HasFrame(const std::uint64_t frame) const
{
  return Find(frame) != nullptr;
}

bool RollbackUndoSnapshotRing::RestoreFrame(const std::uint64_t frame,
                                            const std::span<std::uint8_t> memory,
                                            const std::span<std::uint8_t> auxiliary_state)
{
  if (m_status != ConfigurationStatus::Valid || !m_active_frame || frame > *m_active_frame ||
      memory.size() != m_memory_size || auxiliary_state.size() != m_auxiliary_state_size)
  {
    return false;
  }
  Slot* const target = Find(frame);
  if (!target)
    return false;

  // Preflight the complete chain before mutating memory. A missing or corrupt
  // intermediate checkpoint must never leave the caller partly restored.
  for (std::uint64_t current = *m_active_frame;; --current)
  {
    const Slot* const slot = Find(current);
    if (!slot || slot->offsets.size() != slot->preimages.size())
      return false;
    if (current == frame)
      break;
    if (current == 0)
      return false;
  }
  for (std::uint64_t current = *m_active_frame;; --current)
  {
    const Slot& slot = *Find(current);
    for (std::size_t i = 0; i < slot.offsets.size(); ++i)
      memory[slot.offsets[i]] = slot.preimages[i];
    if (current == frame)
      break;
  }
  if (!auxiliary_state.empty())
    std::memcpy(auxiliary_state.data(), target->auxiliary_state.data(), auxiliary_state.size());

  ClearSeenBits();
  target->offsets.clear();
  target->preimages.clear();
  target->valid = true;
  m_active_frame = frame;
  m_active_overflow = false;
  return true;
}

std::optional<std::size_t>
RollbackUndoSnapshotRing::GetUniqueBytes(const std::uint64_t frame) const
{
  const Slot* const slot = Find(frame);
  return slot ? std::optional{slot->offsets.size()} : std::nullopt;
}

RollbackUndoSnapshotRing::Slot* RollbackUndoSnapshotRing::Find(const std::uint64_t frame)
{
  if (m_status != ConfigurationStatus::Valid)
    return nullptr;
  Slot& slot = m_slots[frame % m_slots.size()];
  return slot.valid && slot.frame == frame ? &slot : nullptr;
}

const RollbackUndoSnapshotRing::Slot*
RollbackUndoSnapshotRing::Find(const std::uint64_t frame) const
{
  if (m_status != ConfigurationStatus::Valid)
    return nullptr;
  const Slot& slot = m_slots[frame % m_slots.size()];
  return slot.valid && slot.frame == frame ? &slot : nullptr;
}

void RollbackUndoSnapshotRing::ClearSeenBits()
{
  for (const std::uint32_t word : m_touched_seen_words)
    m_seen_bits[word] = 0;
  m_touched_seen_words.clear();
}

}  // namespace NetPlay
