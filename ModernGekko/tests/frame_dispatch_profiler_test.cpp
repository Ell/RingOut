#include "Core/PowerPC/StaticRecomp/FrameDispatchProfiler.h"

#include <cstdlib>
#include <iostream>

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
  PowerPC::FrameDispatchProfiler profiler(2, 4);
  for (unsigned frame = 0; frame < 6; ++frame)
  {
    profiler.RecordDispatch(0x80001000);
    profiler.RecordDispatch(0x80002000);
    if ((frame & 1) == 0)
      profiler.RecordDispatch(0x80002000);
    if (frame != 4)
      profiler.RecordDispatch(0x80003000);
    profiler.EndVideoFrame();
  }

  Check(profiler.GetObservedFrames() == 6, "all frame boundaries are counted");
  Check(profiler.GetProfiledFrames() == 4, "warmup is excluded and maximum is bounded");
  Check(profiler.IsComplete(), "profile completes at its configured bound");

  const auto strict = profiler.GetCandidates();
  Check(strict.size() == 1, "only an exactly-once/every-frame PC is strict");
  Check(strict[0].pc == 0x80001000, "stable PC is selected");
  Check(strict[0].total_hits == 4 && strict[0].min_hits == 1 && strict[0].max_hits == 1,
        "stable PC statistics are exact");
  Check(strict[0].first_ordinal_min == 0 && strict[0].first_ordinal_max == 0,
        "stable PC retains its frame-relative dispatch position");

  const auto diagnostic = profiler.GetCandidates(false);
  Check(diagnostic.size() == 3, "diagnostic candidates retain partial and bursty PCs");
  Check(diagnostic[0].pc == 0x80001000, "strongest candidate remains first");

  std::cout << "frame dispatch profiler tests passed\n";
  return 0;
}
