// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/NetPlay/RollbackUndoSnapshotRing.h"
#include "Core/NetPlay/Sc2RollbackTransactionTimeline.h"

namespace NetPlay
{

// One ownership boundary for the branchable SC2 transaction timeline and its
// sparse pre-write RAM/CPU checkpoints. Any disagreement between the two
// histories is a terminal false result; callers must fall back or stop.
class Sc2RollbackTransactionStore final
{
public:
  using ReplayPlan = Sc2RollbackTransactionTimeline::ReplayPlan;

  enum class ConfigurationStatus : std::uint8_t
  {
    Valid,
    InvalidUndoRing,
    InvalidTimeline,
  };

  Sc2RollbackTransactionStore(std::size_t memory_size, std::size_t capacity,
                              std::size_t max_unique_bytes_per_transaction,
                              std::size_t auxiliary_state_size);

  ConfigurationStatus GetConfigurationStatus() const { return m_status; }
  bool BeginTransaction(std::uint64_t id, std::uint64_t emulated_frame,
                        std::span<const std::uint8_t> memory,
                        std::span<const std::uint8_t> auxiliary_state);
  bool RecordWrite(std::size_t offset, std::size_t size,
                   std::span<const std::uint8_t> memory);
  bool RecordConsumedBatch(std::uint64_t batch_id);
  bool CompleteTransaction(std::uint64_t emulated_frame);
  ReplayPlan PlanCorrection(std::uint64_t first_incorrect_batch,
                            std::uint64_t replay_through_batch) const;
  bool Restore(const ReplayPlan& plan, std::span<std::uint8_t> memory,
               std::span<std::uint8_t> auxiliary_state);

  const Sc2RollbackTransactionTimeline& GetTimeline() const { return m_timeline; }
  const RollbackUndoSnapshotRing& GetUndoRing() const { return m_undo_ring; }

private:
  RollbackUndoSnapshotRing m_undo_ring;
  Sc2RollbackTransactionTimeline m_timeline;
  const ConfigurationStatus m_status;
};

}  // namespace NetPlay
