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
    profiler.RecordDispatch(0x80001000, 0x8000f004);
    profiler.RecordDispatch(0x80002000, 0x8000f104);
    if ((frame & 1) == 0)
      profiler.RecordDispatch(0x80002000, 0x8000f204);
    if (frame != 4)
      profiler.RecordDispatch(0x80003000, 0x8000f304 + frame * 4);
    profiler.RecordDispatch(0x80004000, 0x8000f404);
    profiler.RecordDispatch(0x80004000, 0x8000f408);
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
  Check(strict[0].even_frames_with_hits == 2 && strict[0].odd_frames_with_hits == 2,
        "stable PC covers both profiled video-frame parities");
  Check(strict[0].first_ordinal_min == 0 && strict[0].first_ordinal_max == 0,
        "stable PC retains its frame-relative dispatch position");
  Check(strict[0].caller_lr == 0x8000f004 && strict[0].caller_lr_stable,
        "stable PC retains its guest return address");
  Check(strict[0].predecessor_pc == 0 && strict[0].predecessor_pc_stable,
        "first dispatch in a frame retains its empty predecessor");

  const auto diagnostic = profiler.GetCandidates(false);
  Check(diagnostic.size() == 4,
        "diagnostic candidates retain partial, bursty, and always-multiple PCs");
  Check(diagnostic[0].pc == 0x80001000, "strongest candidate remains first");
  Check(diagnostic[2].pc == 0x80004000 && diagnostic[2].exactly_once_frames == 0,
        "coverage diagnostics do not discard a PC that always executes twice");
  Check(diagnostic[1].pc == 0x80002000 &&
            diagnostic[1].even_frames_with_hits == 2 &&
            diagnostic[1].odd_frames_with_hits == 2,
        "bursty PC parity remains explicit");
  Check(diagnostic[3].pc == 0x80003000 &&
            diagnostic[3].even_frames_with_hits == 1 &&
            diagnostic[3].odd_frames_with_hits == 2,
        "partial PC parity exposes the missing profiled frame");
  Check(!diagnostic[3].caller_lr_stable,
        "diagnostic candidates report a changing guest return address");

  std::cout << "frame dispatch profiler tests passed\n";
  return 0;
}
