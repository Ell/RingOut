// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/Sc2RollbackTransactionStore.h"

namespace NetPlay
{

Sc2RollbackTransactionStore::Sc2RollbackTransactionStore(
    const std::size_t memory_size, const std::size_t capacity,
    const std::size_t max_unique_bytes_per_transaction, const std::size_t auxiliary_state_size)
    : m_undo_ring(memory_size, capacity, max_unique_bytes_per_transaction, auxiliary_state_size),
      m_timeline(capacity),
      m_status(m_undo_ring.GetConfigurationStatus() !=
                       RollbackUndoSnapshotRing::ConfigurationStatus::Valid ?
                   ConfigurationStatus::InvalidUndoRing :
               !m_timeline.IsValid() ? ConfigurationStatus::InvalidTimeline :
                                       ConfigurationStatus::Valid)
{
}

bool Sc2RollbackTransactionStore::BeginTransaction(
    const std::uint64_t id, const std::uint64_t emulated_frame,
    const std::span<const std::uint8_t> memory,
    const std::span<const std::uint8_t> auxiliary_state)
{
  if (m_status != ConfigurationStatus::Valid ||
      !m_undo_ring.BeginFrame(id, memory, auxiliary_state))
  {
    return false;
  }
  return m_timeline.BeginTransaction(id, emulated_frame);
}

bool Sc2RollbackTransactionStore::RecordWrite(
    const std::size_t offset, const std::size_t size,
    const std::span<const std::uint8_t> memory)
{
  return m_status == ConfigurationStatus::Valid && m_undo_ring.RecordWrite(offset, size, memory);
}

bool Sc2RollbackTransactionStore::RecordConsumedBatch(const std::uint64_t batch_id)
{
  return m_status == ConfigurationStatus::Valid && m_timeline.RecordConsumedBatch(batch_id);
}

bool Sc2RollbackTransactionStore::CompleteTransaction(const std::uint64_t emulated_frame)
{
  return m_status == ConfigurationStatus::Valid && m_undo_ring.IsActiveFrameValid() &&
         m_timeline.CompleteTransaction(emulated_frame);
}

Sc2RollbackTransactionStore::ReplayPlan Sc2RollbackTransactionStore::PlanCorrection(
    const std::uint64_t first_incorrect_batch, const std::uint64_t replay_through_batch) const
{
  if (m_status != ConfigurationStatus::Valid)
  {
    return {.status = Sc2RollbackTransactionTimeline::PlanStatus::HistoryUnavailable};
  }
  return m_timeline.PlanCorrection(first_incorrect_batch, replay_through_batch);
}

bool Sc2RollbackTransactionStore::Restore(const ReplayPlan& plan,
                                          const std::span<std::uint8_t> memory,
                                          const std::span<std::uint8_t> auxiliary_state)
{
  if (m_status != ConfigurationStatus::Valid ||
      plan.status != Sc2RollbackTransactionTimeline::PlanStatus::Ready ||
      !m_timeline.Find(plan.restore_transaction) ||
      !m_undo_ring.HasFrame(plan.restore_transaction))
  {
    return false;
  }
  if (!m_undo_ring.RestoreFrame(plan.restore_transaction, memory, auxiliary_state))
    return false;
  return m_timeline.RewindToTransaction(plan.restore_transaction);
}

}  // namespace NetPlay
