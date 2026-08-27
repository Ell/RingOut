#include "Core/NetPlay/RollbackUndoSnapshotRing.h"

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

void JournalWrite(NetPlay::RollbackUndoSnapshotRing& ring, std::vector<std::uint8_t>& memory,
                  const std::size_t offset, const std::vector<std::uint8_t>& values)
{
  Check(ring.RecordWrite(offset, values.size(), memory), "write preimage recorded");
  for (std::size_t i = 0; i < values.size(); ++i)
    memory[offset + i] = values[i];
}
}  // namespace

int main()
{
  using Ring = NetPlay::RollbackUndoSnapshotRing;
  Ring ring(32, 4, 12, 2);
  Check(ring.GetConfigurationStatus() == Ring::ConfigurationStatus::Valid,
        "valid undo ring accepted");
  std::vector<std::uint8_t> memory(32);
  for (std::size_t i = 0; i < memory.size(); ++i)
    memory[i] = static_cast<std::uint8_t>(i);
  const std::vector<std::uint8_t> original = memory;
  std::vector<std::uint8_t> auxiliary{1, 2};

  Check(ring.BeginFrame(10, memory, auxiliary), "first frame begins");
  JournalWrite(ring, memory, 4, {90, 91, 92, 93});
  JournalWrite(ring, memory, 6, {80, 81, 82, 83});
  Check(ring.GetUniqueBytes(10) == 6, "overlapping writes retain first preimage once");
  Check(std::vector<std::uint32_t>(ring.GetWriteOffsets(10).begin(),
                                   ring.GetWriteOffsets(10).end()) ==
            std::vector<std::uint32_t>({4, 5, 6, 7, 8, 9}),
        "exact transaction-owned offsets are exposed in first-write order");

  auxiliary = {3, 4};
  Check(ring.BeginFrame(11, memory, auxiliary), "second frame begins");
  JournalWrite(ring, memory, 8, {70, 71});
  auxiliary = {5, 6};
  Check(ring.BeginFrame(12, memory, auxiliary), "third frame begins");
  JournalWrite(ring, memory, 4, {60, 61});

  std::fill(auxiliary.begin(), auxiliary.end(), 0xff);
  Check(ring.RestoreFrame(10, memory, auxiliary), "multi-frame undo succeeds");
  Check(memory == original, "multi-frame undo restores exact first preimages");
  Check(auxiliary == std::vector<std::uint8_t>({1, 2}), "target auxiliary state restored");
  Check(ring.GetActiveFrame() == 10, "target becomes active replay frame");

  JournalWrite(ring, memory, 1, {0xaa});
  Check(ring.BeginFrame(11, memory, std::span<const std::uint8_t>{auxiliary}),
        "corrected replay advances normally");
  JournalWrite(ring, memory, 2, {0xbb});
  Check(ring.RestoreFrame(10, memory, auxiliary), "corrected history remains undoable");
  Check(memory == original, "corrected replay preimages restore exact target");

  Ring overflow(16, 2, 2);
  std::vector<std::uint8_t> small(16);
  Check(overflow.BeginFrame(1, small), "overflow case begins");
  Check(!overflow.RecordWrite(0, 3, small), "unique-byte capacity fails closed");
  Check(!overflow.IsActiveFrameValid(), "overflow invalidates active checkpoint");
  Check(!overflow.BeginFrame(2, small), "overflow cannot be advanced past silently");

  Ring wrap(8, 2, 2);
  std::vector<std::uint8_t> wrap_memory(8);
  Check(wrap.BeginFrame(1, wrap_memory), "wrap frame one");
  Check(wrap.BeginFrame(2, wrap_memory), "wrap frame two");
  Check(wrap.BeginFrame(3, wrap_memory), "wrap frame three");
  Check(!wrap.HasFrame(1) && wrap.HasFrame(3), "modulo reuse evicts old identity");

  Ring missing(8, 2, 2);
  std::vector<std::uint8_t> missing_memory(8);
  Check(missing.BeginFrame(1, missing_memory), "missing-chain frame one");
  JournalWrite(missing, missing_memory, 0, {1});
  Check(missing.BeginFrame(2, missing_memory), "missing-chain frame two");
  JournalWrite(missing, missing_memory, 1, {2});
  Check(missing.BeginFrame(3, missing_memory), "missing-chain frame three evicts one");
  JournalWrite(missing, missing_memory, 2, {3});
  const std::vector<std::uint8_t> before_failed_restore = missing_memory;
  Check(!missing.RestoreFrame(1, missing_memory), "evicted restore chain fails before mutation");
  Check(missing_memory == before_failed_restore, "failed restore never partially mutates memory");

  Ring max_frame(8, 2, 2);
  Check(max_frame.BeginFrame(UINT64_MAX, missing_memory), "maximum frame can begin");
  Check(!max_frame.BeginFrame(0, missing_memory), "frame identity cannot wrap around");

  std::cout << "rollback undo snapshot ring tests passed\n";
  return 0;
}
