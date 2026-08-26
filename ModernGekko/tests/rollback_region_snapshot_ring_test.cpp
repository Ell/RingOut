#include "Core/NetPlay/RollbackRegionSnapshotRing.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
void Check(bool value, const char* message)
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
  using Ring = NetPlay::RollbackRegionSnapshotRing;
  Ring ring(32, {{16, 4}, {0, 8}}, 3, 4);
  Check(ring.GetConfigurationStatus() == Ring::ConfigurationStatus::Valid,
        "valid regions accepted");
  Check(ring.GetSnapshotSize() == 16, "only selected bytes plus auxiliary state are stored");

  std::vector<std::uint8_t> memory(32);
  std::vector<std::uint8_t> auxiliary = {1, 2, 3, 4};
  for (std::size_t i = 0; i < memory.size(); ++i)
    memory[i] = static_cast<std::uint8_t>(i);
  Check(ring.Capture(7, memory, auxiliary), "capture succeeds");

  std::fill(memory.begin(), memory.end(), 0xee);
  std::fill(auxiliary.begin(), auxiliary.end(), 0xff);
  Check(ring.Restore(7, memory, auxiliary), "restore succeeds");
  for (std::size_t i = 0; i < 8; ++i)
    Check(memory[i] == i, "first selected range restored");
  for (std::size_t i = 8; i < 16; ++i)
    Check(memory[i] == 0xee, "omitted host-owned gap preserved");
  for (std::size_t i = 16; i < 20; ++i)
    Check(memory[i] == i, "second selected range restored");
  Check(auxiliary == std::vector<std::uint8_t>({1, 2, 3, 4}), "CPU/game auxiliary state restored");

  for (std::uint64_t frame = 8; frame <= 10; ++frame)
    Check(ring.Capture(frame, memory, auxiliary), "ring capture succeeds");
  Check(!ring.HasFrame(7), "modulo reuse evicts old frame identity");
  Check(ring.HasFrame(10), "new frame identity retained");

  Ring overlap(32, {{0, 8}, {4, 8}}, 2);
  Check(overlap.GetConfigurationStatus() == Ring::ConfigurationStatus::OverlappingRegions,
        "overlap fails closed");
  Ring out_of_bounds(32, {{31, 2}}, 2);
  Check(out_of_bounds.GetConfigurationStatus() == Ring::ConfigurationStatus::InvalidRegion,
        "out-of-bounds region fails closed");

  std::cout << "rollback region snapshot ring tests passed\n";
  return 0;
}
