// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <limits>

#include "Common/CommonTypes.h"

namespace NetPlay
{
struct RollbackPerformanceSnapshot
{
  u32 current_depth_frames = 0;
  u32 recent_peak_depth_frames = 0;
};

// Thread-safe telemetry shared by the CPU-thread correction path and the
// network-thread ping display. Timestamps are supplied by the caller so the
// retention behavior remains deterministic and directly testable.
class RollbackPerformanceStats
{
public:
  static constexpr u64 RECENT_PEAK_HOLD_MS = 1000;

  void Reset()
  {
    m_current_depth_frames.store(0, std::memory_order_relaxed);
    m_recent_peak_depth_frames.store(0, std::memory_order_relaxed);
    m_recent_peak_until_ms.store(0, std::memory_order_release);
  }

  void BeginCorrection(const u64 restore_before_frame, const u64 replay_through_frame,
                       const u64 now_ms)
  {
    if (replay_through_frame < restore_before_frame)
      return;

    // Both endpoints are emulated frames that will be replayed. A correction
    // from frame 137 through frame 137 therefore resimulates one frame.
    const u64 distance = replay_through_frame - restore_before_frame;
    const u64 inclusive_depth =
        distance == std::numeric_limits<u64>::max() ? distance : distance + 1;
    const u32 depth =
        static_cast<u32>(std::min<u64>(inclusive_depth, std::numeric_limits<u32>::max()));
    m_current_depth_frames.store(depth, std::memory_order_relaxed);

    const u64 previous_until = m_recent_peak_until_ms.load(std::memory_order_acquire);
    const u32 previous_peak = m_recent_peak_depth_frames.load(std::memory_order_relaxed);
    if (now_ms > previous_until || depth >= previous_peak)
    {
      m_recent_peak_depth_frames.store(depth, std::memory_order_relaxed);
      const u64 hold_until = now_ms > std::numeric_limits<u64>::max() - RECENT_PEAK_HOLD_MS ?
                                 std::numeric_limits<u64>::max() :
                                 now_ms + RECENT_PEAK_HOLD_MS;
      m_recent_peak_until_ms.store(hold_until, std::memory_order_release);
    }
  }

  void EndCorrection() { m_current_depth_frames.store(0, std::memory_order_relaxed); }

  RollbackPerformanceSnapshot Snapshot(const u64 now_ms) const
  {
    const u32 current = m_current_depth_frames.load(std::memory_order_relaxed);
    const u64 peak_until = m_recent_peak_until_ms.load(std::memory_order_acquire);
    const u32 recent =
        now_ms <= peak_until ? m_recent_peak_depth_frames.load(std::memory_order_relaxed) : 0;
    return {.current_depth_frames = current, .recent_peak_depth_frames = recent};
  }

private:
  std::atomic<u32> m_current_depth_frames{0};
  std::atomic<u32> m_recent_peak_depth_frames{0};
  std::atomic<u64> m_recent_peak_until_ms{0};
};
}  // namespace NetPlay
