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

// Sparse preimage checkpoints for a deterministic game-state transaction.
// RecordWrite must be called before each covered write. Restoring frame F
// applies preimages from the current frame back through F, leaving omitted
// host/hardware state at the canonical frontier.
class RollbackUndoSnapshotRing final
{
public:
  enum class ConfigurationStatus : std::uint8_t
  {
    Valid,
    ZeroMemory,
    ZeroCapacity,
    ZeroWriteCapacity,
    MemoryTooLarge,
  };

  RollbackUndoSnapshotRing(std::size_t memory_size, std::size_t capacity,
                           std::size_t max_unique_bytes_per_frame,
                           std::size_t auxiliary_state_size = 0);

  ConfigurationStatus GetConfigurationStatus() const { return m_status; }
  std::size_t GetCapacity() const { return m_slots.size(); }
  std::size_t GetMaxUniqueBytesPerFrame() const { return m_max_unique_bytes_per_frame; }

  bool BeginFrame(std::uint64_t frame, std::span<const std::uint8_t> memory,
                  std::span<const std::uint8_t> auxiliary_state = {});
  bool RecordWrite(std::size_t offset, std::size_t size,
                   std::span<const std::uint8_t> memory);
  bool HasFrame(std::uint64_t frame) const;
  bool RestoreFrame(std::uint64_t frame, std::span<std::uint8_t> memory,
                    std::span<std::uint8_t> auxiliary_state = {});

  std::optional<std::uint64_t> GetActiveFrame() const { return m_active_frame; }
  std::optional<std::size_t> GetUniqueBytes(std::uint64_t frame) const;
  std::span<const std::uint32_t> GetWriteOffsets(std::uint64_t frame) const;
  bool IsActiveFrameValid() const { return m_active_frame.has_value() && !m_active_overflow; }

private:
  struct Slot
  {
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint8_t> preimages;
    std::vector<std::uint8_t> auxiliary_state;
    std::optional<std::uint64_t> frame;
    bool valid = false;
  };

  Slot* Find(std::uint64_t frame);
  const Slot* Find(std::uint64_t frame) const;
  void ClearSeenBits();

  const std::size_t m_memory_size;
  const std::size_t m_max_unique_bytes_per_frame;
  const std::size_t m_auxiliary_state_size;
  const ConfigurationStatus m_status;
  std::vector<Slot> m_slots;
  std::vector<std::uint64_t> m_seen_bits;
  std::vector<std::uint32_t> m_touched_seen_words;
  std::optional<std::uint64_t> m_active_frame;
  bool m_active_overflow = false;
};

}  // namespace NetPlay
