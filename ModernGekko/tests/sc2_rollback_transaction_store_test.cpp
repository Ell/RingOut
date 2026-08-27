#include "Core/NetPlay/Sc2RollbackTransactionStore.h"

#include <cstdlib>
#include <iostream>
#include <vector>

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

void Write(NetPlay::Sc2RollbackTransactionStore& store, std::vector<std::uint8_t>& memory,
           const std::size_t offset, const std::uint8_t value)
{
  Check(store.RecordWrite(offset, 1, memory), "preimage write records");
  memory[offset] = value;
}
}  // namespace

int main()
{
  using Store = NetPlay::Sc2RollbackTransactionStore;
  using PlanStatus = NetPlay::Sc2RollbackTransactionTimeline::PlanStatus;
  Store store(16, 4, 8, 2);
  Check(store.GetConfigurationStatus() == Store::ConfigurationStatus::Valid,
        "transaction store configures");
  std::vector<std::uint8_t> memory(16);
  std::vector<std::uint8_t> auxiliary{10, 11};

  Check(store.BeginTransaction(10, 20, memory, auxiliary), "transaction ten begins");
  Write(store, memory, 0, 1);
  Check(store.RecordConsumedBatch(40) && store.CompleteTransaction(21),
        "transaction ten completes");

  auxiliary = {12, 13};
  Check(store.BeginTransaction(11, 22, memory, auxiliary), "transaction eleven begins");
  Write(store, memory, 1, 2);
  Check(store.RecordConsumedBatch(44) && store.CompleteTransaction(23),
        "transaction eleven completes");
  auxiliary = {14, 15};
  Check(store.BeginTransaction(12, 24, memory, auxiliary), "transaction twelve begins");
  Write(store, memory, 2, 3);
  Check(store.RecordConsumedBatch(48) && store.CompleteTransaction(25),
        "transaction twelve completes");

  const Store::ReplayPlan plan = store.PlanCorrection(44, 48);
  Check(plan.status == PlanStatus::Ready && plan.restore_transaction == 11 &&
            plan.replay_through_transaction == 12,
        "correction selects exact transaction branch");
  std::fill(auxiliary.begin(), auxiliary.end(), 0xff);
  Check(store.Restore(plan, memory, auxiliary), "state and timeline rewind atomically");
  Check(memory[0] == 1 && memory[1] == 0 && memory[2] == 0,
        "multi-transaction undo restores target start");
  Check(auxiliary == std::vector<std::uint8_t>({12, 13}),
        "target CPU auxiliary state restores");

  Write(store, memory, 1, 9);
  Check(store.RecordConsumedBatch(44) && store.CompleteTransaction(23),
        "corrected target rebuilds in place");
  auxiliary = {14, 15};
  Check(store.BeginTransaction(12, 24, memory, auxiliary),
        "corrected descendant begins sequentially");
  Write(store, memory, 2, 8);
  Check(store.RecordConsumedBatch(48) && store.CompleteTransaction(25),
        "corrected descendant completes");

  const Store::ReplayPlan second_plan = store.PlanCorrection(44, 48);
  Check(store.Restore(second_plan, memory, auxiliary), "corrected branch remains rewindable");
  Check(memory[0] == 1 && memory[1] == 0 && memory[2] == 0,
        "corrected branch retains exact first preimages");

  Store invalid(0, 0, 0, 0);
  Check(invalid.GetConfigurationStatus() != Store::ConfigurationStatus::Valid,
        "invalid component config fails closed");

  std::cout << "SC2 rollback transaction store tests passed\n";
  return 0;
}
