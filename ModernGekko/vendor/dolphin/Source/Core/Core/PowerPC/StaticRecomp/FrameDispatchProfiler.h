// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace PowerPC
{

// Research-only profiler used to locate a game's actual update-loop boundary.
// Recording is wired behind RINGOUT_SC2_HOOK_PROFILE, so the production
// dispatcher pays no hash-table cost.  A useful hook candidate executes once
// in every observed video frame after warmup; total-hit and min/max counts are
// retained to reject periodic and bursty false positives.
class FrameDispatchProfiler final
{
public:
  static constexpr std::size_t MAX_TRACKED_PCS = 262144;
  struct Candidate
  {
    std::uint32_t pc = 0;
    std::uint64_t frames_with_hits = 0;
    // Coverage split by zero-based profiled-video-frame parity. SC2 advances
    // its logical engine at 30 Hz while VI/video boundaries occur at 60 Hz;
    // retaining the split makes that alternating cadence directly testable.
    std::uint64_t even_frames_with_hits = 0;
    std::uint64_t odd_frames_with_hits = 0;
    std::uint64_t exactly_once_frames = 0;
    std::uint64_t total_hits = 0;
    std::uint32_t min_hits = 0;
    std::uint32_t max_hits = 0;
    std::uint64_t first_ordinal_min = 0;
    std::uint64_t first_ordinal_max = 0;
    std::uint64_t last_ordinal_min = 0;
    std::uint64_t last_ordinal_max = 0;
    // Context at the first occurrence of this PC in each video frame.  For an
    // exactly-once candidate these identify the guest callsite and the native
    // dispatch immediately preceding it, which is considerably more useful
    // than frequency alone when runtime/VI callbacks dominate the result.
    std::uint32_t caller_lr = 0;
    std::uint32_t predecessor_pc = 0;
    bool caller_lr_stable = true;
    bool predecessor_pc_stable = true;
  };

  explicit FrameDispatchProfiler(std::uint64_t warmup_frames = 120,
                                 std::uint64_t maximum_frames = 600);

  void RecordDispatch(std::uint32_t pc, std::uint32_t caller_lr);
  void EndVideoFrame();

  std::uint64_t GetObservedFrames() const { return m_observed_frames; }
  std::uint64_t GetProfiledFrames() const { return m_profiled_frames; }
  bool IsComplete() const { return m_profiled_frames >= m_maximum_frames; }

  // Candidates are sorted by strongest once-per-frame coverage, then by PC.
  // require_every_frame is the safe default for generating actual hook input.
  std::vector<Candidate> GetCandidates(bool require_every_frame = true) const;

private:
  std::uint64_t m_warmup_frames;
  std::uint64_t m_maximum_frames;
  std::uint64_t m_observed_frames = 0;
  std::uint64_t m_profiled_frames = 0;
  struct FrameHit
  {
    std::uint32_t count = 0;
    std::uint64_t first_ordinal = 0;
    std::uint64_t last_ordinal = 0;
    std::uint32_t first_caller_lr = 0;
    std::uint32_t first_predecessor_pc = 0;
  };
  std::uint64_t m_current_dispatch_ordinal = 0;
  std::uint32_t m_previous_dispatch_pc = 0;
  std::unordered_map<std::uint32_t, FrameHit> m_current_hits;
  std::unordered_map<std::uint32_t, Candidate> m_candidates;
};

}  // namespace PowerPC
