// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/Sc2RollbackTransactionTimeline.h"

#include <algorithm>
#include <limits>

namespace NetPlay
{

Sc2RollbackTransactionTimeline::Sc2RollbackTransactionTimeline(const std::size_t capacity)
    : m_slots(capacity)
{
}

bool Sc2RollbackTransactionTimeline::BeginTransaction(const std::uint64_t id,
                                                      const std::uint64_t emulated_frame)
{
  if (!IsValid() || m_active ||
      (m_latest_completed &&
       (*m_latest_completed == std::numeric_limits<std::uint64_t>::max() ||
        id != *m_latest_completed + 1)))
  {
    return false;
  }

  Slot& slot = m_slots[id % m_slots.size()];
  slot.valid = false;
  slot.complete = false;
  slot.transaction = {.id = id, .begin_emulated_frame = emulated_frame};
  slot.valid = true;
  m_active = id;
  return true;
}

bool Sc2RollbackTransactionTimeline::RecordConsumedBatch(const std::uint64_t batch_id)
{
  if (!m_active)
    return false;
  Slot* const slot = FindSlot(*m_active);
  if (!slot || slot->complete)
    return false;
  auto& batches = slot->transaction.consumed_batches;
  if (!batches.empty())
  {
    if (batch_id == batches.back())
      return true;
    if (batch_id < batches.back())
      return false;
  }
  batches.push_back(batch_id);
  return true;
}

bool Sc2RollbackTransactionTimeline::CompleteTransaction(const std::uint64_t emulated_frame)
{
  if (!m_active)
    return false;
  Slot* const slot = FindSlot(*m_active);
  if (!slot || slot->complete || emulated_frame < slot->transaction.begin_emulated_frame)
    return false;
  slot->transaction.end_emulated_frame = emulated_frame;
  slot->complete = true;
  m_latest_completed = *m_active;
  m_retained_completed_count = std::min(m_retained_completed_count + 1, m_slots.size());
  m_active.reset();
  return true;
}

bool Sc2RollbackTransactionTimeline::RewindToTransaction(const std::uint64_t id)
{
  if (m_active || !m_latest_completed || id > *m_latest_completed)
    return false;
  Slot* target = FindSlot(id);
  if (!target || !target->complete)
    return false;

  const std::uint64_t latest = *m_latest_completed;
  const std::uint64_t removed = latest - id + 1;
  if (removed > m_retained_completed_count)
    return false;
  for (std::uint64_t current = id;; ++current)
  {
    const Slot* const slot = FindSlot(current);
    if (!slot || !slot->complete)
      return false;
    if (current == latest)
      break;
  }

  const std::size_t retained_after =
      m_retained_completed_count - static_cast<std::size_t>(removed);
  const Slot* const previous = retained_after != 0 && id != 0 ? FindSlot(id - 1) : nullptr;
  if (retained_after != 0 && (!previous || !previous->complete))
    return false;

  for (std::uint64_t current = id; current < latest;)
  {
    ++current;
    Slot& slot = m_slots[current % m_slots.size()];
    slot.valid = false;
    slot.complete = false;
  }
  target = FindSlot(id);
  target->complete = false;
  target->transaction.end_emulated_frame = 0;
  target->transaction.consumed_batches.clear();
  m_retained_completed_count -= static_cast<std::size_t>(removed);
  if (m_retained_completed_count != 0)
  {
    m_latest_completed = id - 1;
  }
  else
  {
    m_latest_completed.reset();
  }
  m_active = id;
  return true;
}

Sc2RollbackTransactionTimeline::ReplayPlan Sc2RollbackTransactionTimeline::PlanCorrection(
    const std::uint64_t first_incorrect_batch, const std::uint64_t replay_through_batch) const
{
  if (first_incorrect_batch > replay_through_batch)
    return {.status = PlanStatus::InvalidCorrectionRange};
  if (m_active)
    return {.status = PlanStatus::TransactionActive};
  if (!m_latest_completed)
    return {.status = PlanStatus::HistoryUnavailable};

  const std::uint64_t latest = *m_latest_completed;
  const std::uint64_t retained = m_retained_completed_count;
  const std::uint64_t oldest = latest + 1 - retained;
  std::optional<std::uint64_t> oldest_retained_batch;
  for (std::uint64_t id = oldest; id <= latest; ++id)
  {
    const Slot* const slot = FindSlot(id);
    if (!slot || !slot->complete)
      return {.status = PlanStatus::HistoryUnavailable};
    const auto& batches = slot->transaction.consumed_batches;
    if (!batches.empty() && (!oldest_retained_batch || batches.front() < *oldest_retained_batch))
      oldest_retained_batch = batches.front();
  }
  if (oldest_retained_batch && first_incorrect_batch < *oldest_retained_batch)
    return {.status = PlanStatus::HistoryUnavailable};
  for (std::uint64_t id = oldest; id <= latest; ++id)
  {
    const Slot* const slot = FindSlot(id);
    if (!slot || !slot->complete)
      return {.status = PlanStatus::HistoryUnavailable};
    const auto& batches = slot->transaction.consumed_batches;
    if (std::ranges::any_of(batches, [&](const std::uint64_t batch) {
          return batch >= first_incorrect_batch && batch <= replay_through_batch;
        }))
    {
      return {.status = PlanStatus::Ready,
              .restore_transaction = id,
              .replay_through_transaction = latest};
    }
  }
  return {.status = PlanStatus::NotConsumedByGame};
}

const Sc2RollbackTransactionTimeline::Transaction*
Sc2RollbackTransactionTimeline::Find(const std::uint64_t id) const
{
  const Slot* const slot = FindSlot(id);
  return slot && slot->complete ? &slot->transaction : nullptr;
}

Sc2RollbackTransactionTimeline::Slot*
Sc2RollbackTransactionTimeline::FindSlot(const std::uint64_t id)
{
  if (!IsValid())
    return nullptr;
  Slot& slot = m_slots[id % m_slots.size()];
  return slot.valid && slot.transaction.id == id ? &slot : nullptr;
}

const Sc2RollbackTransactionTimeline::Slot*
Sc2RollbackTransactionTimeline::FindSlot(const std::uint64_t id) const
{
  if (!IsValid())
    return nullptr;
  const Slot& slot = m_slots[id % m_slots.size()];
  return slot.valid && slot.transaction.id == id ? &slot : nullptr;
}

}  // namespace NetPlay
