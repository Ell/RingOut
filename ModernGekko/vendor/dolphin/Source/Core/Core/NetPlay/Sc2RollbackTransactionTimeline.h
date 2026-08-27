// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace NetPlay
{

// Maps SC2's 30 Hz game-owned update transactions to the variable-rate SI
// batches they actually consumed. This prevents a correction from being
// projected onto a video frame which never advanced game state.
class Sc2RollbackTransactionTimeline final
{
public:
  struct Transaction
  {
    std::uint64_t id = 0;
    std::uint64_t begin_emulated_frame = 0;
    std::uint64_t end_emulated_frame = 0;
    std::vector<std::uint64_t> consumed_batches;

    bool operator==(const Transaction&) const = default;
  };

  enum class PlanStatus : std::uint8_t
  {
    Ready,
    NotConsumedByGame,
    HistoryUnavailable,
    TransactionActive,
    InvalidCorrectionRange,
  };

  struct ReplayPlan
  {
    PlanStatus status = PlanStatus::HistoryUnavailable;
    std::uint64_t restore_transaction = 0;
    std::uint64_t replay_through_transaction = 0;
  };

  explicit Sc2RollbackTransactionTimeline(std::size_t capacity);

  bool IsValid() const { return !m_slots.empty(); }
  bool BeginTransaction(std::uint64_t id, std::uint64_t emulated_frame);
  bool RecordConsumedBatch(std::uint64_t batch_id);
  bool CompleteTransaction(std::uint64_t emulated_frame);
  ReplayPlan PlanCorrection(std::uint64_t first_incorrect_batch,
                            std::uint64_t replay_through_batch) const;
  const Transaction* Find(std::uint64_t id) const;
  std::optional<std::uint64_t> GetLatestCompletedTransaction() const
  {
    return m_latest_completed;
  }

private:
  struct Slot
  {
    Transaction transaction{};
    bool valid = false;
    bool complete = false;
  };

  Slot* FindSlot(std::uint64_t id);
  const Slot* FindSlot(std::uint64_t id) const;

  std::vector<Slot> m_slots;
  std::optional<std::uint64_t> m_active;
  std::optional<std::uint64_t> m_latest_completed;
  std::size_t m_completed_count = 0;
};

}  // namespace NetPlay
