#include "Core/NetPlay/Sc2RollbackTransactionTimeline.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Check(const bool value, const char* const message)
{
  if (!value)
  {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
}  // namespace

int main()
{
  using Timeline = NetPlay::Sc2RollbackTransactionTimeline;
  Timeline timeline(3);
  Check(timeline.IsValid(), "nonzero history is valid");

  Check(timeline.BeginTransaction(10, 20), "first 30 Hz transaction begins on video frame 20");
  Check(timeline.RecordConsumedBatch(40), "first transaction consumes batch 40");
  Check(timeline.RecordConsumedBatch(40), "grouped second pad keeps the same batch");
  Check(timeline.CompleteTransaction(21), "transaction can span two video frames");

  Check(timeline.BeginTransaction(11, 22), "second transaction begins");
  Check(timeline.RecordConsumedBatch(44), "unconsumed hardware batches may form gaps");
  Check(timeline.CompleteTransaction(23), "second transaction completes");
  Check(timeline.BeginTransaction(12, 24), "third transaction begins");
  Check(timeline.RecordConsumedBatch(48), "third transaction consumes batch 48");
  Check(timeline.CompleteTransaction(25), "third transaction completes");

  const auto corrected = timeline.PlanCorrection(44, 49);
  Check(corrected.status == Timeline::PlanStatus::Ready &&
            corrected.restore_transaction == 11 && corrected.replay_through_transaction == 12,
        "correction maps to the exact consuming transaction");
  Check(timeline.PlanCorrection(45, 49).status == Timeline::PlanStatus::NotConsumedByGame,
        "hardware-only poll correction does not rewind game state");
  Check(timeline.PlanCorrection(49, 48).status == Timeline::PlanStatus::InvalidCorrectionRange,
        "invalid correction interval fails closed");

  Check(timeline.BeginTransaction(13, 26), "ring reuse begins next transaction");
  Check(timeline.RecordConsumedBatch(52), "ring reuse records input");
  Check(timeline.PlanCorrection(52, 52).status == Timeline::PlanStatus::TransactionActive,
        "planning cannot race an active transaction");
  Check(timeline.CompleteTransaction(27), "ring reuse completes");
  Check(timeline.Find(10) == nullptr && timeline.Find(13) != nullptr,
        "modulo reuse preserves exact transaction identity");
  Check(timeline.PlanCorrection(40, 52).status == Timeline::PlanStatus::HistoryUnavailable,
        "evicted consumed input fails closed");
  Check(!timeline.BeginTransaction(15, 28), "nonsequential transaction identity is rejected");

  Timeline invalid(0);
  Check(!invalid.IsValid() && !invalid.BeginTransaction(1, 1),
        "zero-capacity timeline is inert");

  std::cout << "SC2 rollback transaction timeline tests passed\n";
  return 0;
}
