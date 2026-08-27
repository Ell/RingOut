// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/LiveRollbackFrameBoundary.h"

#include <limits>

#include "Core/NetPlay/LiveRollbackOutputGate.h"
#include "Core/NetPlay/RollbackCoordinator.h"
#include "Core/NetPlay/RollbackSIInputJournal.h"

namespace NetPlay
{
LiveRollbackFrameBoundary::LiveRollbackFrameBoundary(RollbackCoordinator& coordinator,
                                                     RollbackSIInputJournal& journal,
                                                     LiveRollbackOutputGate& output_gate)
    : m_coordinator(coordinator), m_journal(journal), m_output_gate(output_gate)
{
}

LiveRollbackFrameBoundary::ActivationStatus
LiveRollbackFrameBoundary::Activate(const u64 initial_frame)
{
  if (m_active)
    return ActivationStatus::AlreadyActive;
  if (!m_output_gate.HasCompleteCoverage())
    return ActivationStatus::OutputCoverageIncomplete;
  if (m_coordinator.GetState() != RollbackCoordinator::State::Ready)
    return ActivationStatus::CoordinatorNotReady;

  m_current_frame = initial_frame;
  if (!BeginTrackedFrame(initial_frame))
    return ActivationStatus::InitialCaptureFailed;
  m_active = true;
  return ActivationStatus::Active;
}

LiveRollbackFrameBoundary::BoundaryStatus
LiveRollbackFrameBoundary::CompleteCurrentFrame(const bool start_pending_rollback)
{
  if (!m_active)
    return BoundaryStatus::Inactive;

  const RollbackCoordinator::State state = m_coordinator.GetState();
  if (state == RollbackCoordinator::State::Faulted || state == RollbackCoordinator::State::Disabled)
  {
    return Fault();
  }

  if (state == RollbackCoordinator::State::Ready)
  {
    if (m_coordinator.CompleteFrame(m_current_frame) !=
        RollbackCoordinator::FrameCompleteStatus::NotReplaying)
    {
      return Fault();
    }
    if (start_pending_rollback && m_journal.GetReplayTrigger())
      return StartPendingRollback(false);
    if (m_current_frame == std::numeric_limits<u64>::max())
      return Fault();
    ++m_current_frame;
    return BeginTrackedFrame(m_current_frame) ? BoundaryStatus::Advanced : Fault();
  }

  if (state != RollbackCoordinator::State::Replaying)
    return Fault();

  const RollbackCoordinator::FrameCompleteStatus complete =
      m_coordinator.CompleteFrame(m_current_frame);
  if (complete == RollbackCoordinator::FrameCompleteStatus::ReplayContinues)
  {
    const std::optional<u64> expected = m_coordinator.GetExpectedReplayFrame();
    if (!expected)
      return Fault();
    m_current_frame = *expected;
    return BeginTrackedFrame(m_current_frame) ? BoundaryStatus::ReplayAdvanced : Fault();
  }
  if (complete != RollbackCoordinator::FrameCompleteStatus::AwaitingCommit)
    return Fault();

  if (m_coordinator.CommitReplay(m_journal))
  {
    if (m_current_frame == std::numeric_limits<u64>::max())
      return Fault();
    ++m_current_frame;
    return BeginTrackedFrame(m_current_frame) ? BoundaryStatus::ReplayCommitted : Fault();
  }

  if (m_coordinator.GetState() == RollbackCoordinator::State::Faulted)
    return Fault();

  // A newer correction can arrive between reaching the frontier and commit.
  // Keep output hidden and immediately restart from the replacement trigger.
  return StartPendingRollback(true);
}

void LiveRollbackFrameBoundary::Deactivate()
{
  if (!m_active)
    return;
  if (m_coordinator.GetState() == RollbackCoordinator::State::Replaying ||
      m_coordinator.GetState() == RollbackCoordinator::State::AwaitingCommit)
  {
    m_coordinator.CancelReplay();
  }
  m_active = false;
}

std::optional<u64> LiveRollbackFrameBoundary::GetCurrentFrame() const
{
  return m_active ? std::optional{m_current_frame} : std::nullopt;
}

bool LiveRollbackFrameBoundary::BeginTrackedFrame(const u64 frame)
{
  const RollbackCoordinator::FrameStartStatus status = m_coordinator.BeginFrame(frame);
  return status == RollbackCoordinator::FrameStartStatus::Captured ||
         status == RollbackCoordinator::FrameStartStatus::RestoredFrameAlreadyCaptured;
}

LiveRollbackFrameBoundary::BoundaryStatus
LiveRollbackFrameBoundary::StartPendingRollback(const bool chained)
{
  const std::optional<RollbackCoordinator::ReplayRequest> request = m_journal.GetReplayTrigger();
  if (!request)
    return Fault();

  const RollbackCoordinator::RequestStatus started =
      chained ? m_coordinator.RestartRollbackWhileHidden(*request) :
                m_coordinator.StartRollback(*request);
  if (started != RollbackCoordinator::RequestStatus::Started)
    return Fault();

  m_current_frame = request->restore_before_emulated_frame;
  if (!BeginTrackedFrame(m_current_frame))
    return Fault();
  return chained ? BoundaryStatus::ChainedRollbackStarted : BoundaryStatus::RollbackStarted;
}

LiveRollbackFrameBoundary::BoundaryStatus LiveRollbackFrameBoundary::Fault()
{
  if (m_coordinator.GetState() == RollbackCoordinator::State::Replaying ||
      m_coordinator.GetState() == RollbackCoordinator::State::AwaitingCommit)
  {
    m_coordinator.CancelReplay();
  }
  m_active = false;
  return BoundaryStatus::Faulted;
}

}  // namespace NetPlay
