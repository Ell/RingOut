// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Common/CommonTypes.h"

namespace NetPlay
{
class LiveRollbackOutputGate;
class RollbackCoordinator;
class RollbackSIInputJournal;

// CPU-thread-only adapter between the once-per-emulated-frame callback and
// RollbackCoordinator. It deliberately cannot activate while the production
// output capability matrix is incomplete.
class LiveRollbackFrameBoundary final
{
public:
  enum class ActivationStatus : u8
  {
    Active,
    AlreadyActive,
    OutputCoverageIncomplete,
    CoordinatorNotReady,
    InitialCaptureFailed,
  };

  enum class BoundaryStatus : u8
  {
    Inactive,
    Advanced,
    RollbackStarted,
    ReplayAdvanced,
    ReplayCommitted,
    ChainedRollbackStarted,
    Faulted,
  };

  LiveRollbackFrameBoundary(RollbackCoordinator& coordinator, RollbackSIInputJournal& journal,
                            LiveRollbackOutputGate& output_gate);

  ActivationStatus Activate(u64 initial_frame);
  BoundaryStatus CompleteCurrentFrame();
  void Deactivate();

  bool IsActive() const { return m_active; }
  std::optional<u64> GetCurrentFrame() const;

private:
  bool BeginTrackedFrame(u64 frame);
  BoundaryStatus StartPendingRollback(bool chained);
  BoundaryStatus Fault();

  RollbackCoordinator& m_coordinator;
  RollbackSIInputJournal& m_journal;
  LiveRollbackOutputGate& m_output_gate;
  u64 m_current_frame = 0;
  bool m_active = false;
};

// Installation is CPU-thread-only and non-owning. Session teardown must remove
// the driver before destroying it. The ordinary fixed-delay path observes a
// cheap no-op when no driver is installed.
bool InstallLiveRollbackFrameBoundary(LiveRollbackFrameBoundary* driver, u64 initial_frame);
void UninstallLiveRollbackFrameBoundary(LiveRollbackFrameBoundary* driver);
LiveRollbackFrameBoundary::BoundaryStatus CompleteLiveRollbackFrameBoundary();

}  // namespace NetPlay
