// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackCoordinator.h"

#include <limits>

namespace NetPlay
{

RollbackCoordinator::RollbackCoordinator(const Config config, RollbackStateStore& state_store,
                                         RollbackOutputGate& output_gate)
    : m_config(config), m_state_store(state_store), m_output_gate(output_gate),
      m_state(config.enabled && config.max_replay_frames != 0 ? State::Ready : State::Disabled)
{
}

RollbackCoordinator::~RollbackCoordinator()
{
  if (m_output_gate_active)
    m_output_gate.EndHiddenReplay(false);
}

RollbackCoordinator::FrameStartStatus RollbackCoordinator::BeginFrame(const u64 frame)
{
  if (m_state == State::Disabled)
    return FrameStartStatus::Disabled;
  if (m_state == State::Faulted)
    return FrameStartStatus::Faulted;
  if (m_state == State::AwaitingCommit)
    return FrameStartStatus::AwaitingCommit;

  if (m_state == State::Replaying)
  {
    if (!m_expected_replay_frame || frame != *m_expected_replay_frame || m_replay_frame_started)
    {
      EnterFaultedState();
      return FrameStartStatus::UnexpectedFrame;
    }

    // StartRollback restored this exact checkpoint. Capturing it again would
    // only waste a full-state copy and could overwrite the sole recovery point.
    if (m_active_request && frame == m_active_request->restore_before_emulated_frame)
    {
      m_replay_frame_started = true;
      return FrameStartStatus::RestoredFrameAlreadyCaptured;
    }
  }

  if (!m_state_store.CaptureFrameStart(frame))
  {
    EnterFaultedState();
    return FrameStartStatus::CaptureFailed;
  }
  if (m_state == State::Replaying)
    m_replay_frame_started = true;
  return FrameStartStatus::Captured;
}

RollbackCoordinator::RequestStatus RollbackCoordinator::StartRollback(const ReplayRequest& request)
{
  if (m_state == State::Disabled)
    return RequestStatus::Disabled;
  if (m_state == State::Faulted)
    return RequestStatus::Faulted;
  if (m_state != State::Ready)
    return RequestStatus::Busy;
  if (const RequestStatus validation = ValidateRequest(request);
      validation != RequestStatus::Started)
    return validation;

  // Output must be hidden before state moves backwards. If the gate cannot
  // guarantee that, leave the current fixed-delay-compatible state untouched.
  if (!m_output_gate.BeginHiddenReplay(request.restore_before_emulated_frame,
                                       request.replay_through_emulated_frame))
  {
    return RequestStatus::OutputSuppressionUnavailable;
  }
  m_output_gate_active = true;

  if (!m_state_store.RestoreFrameStart(request.restore_before_emulated_frame))
  {
    EnterFaultedState();
    return RequestStatus::RestoreFailed;
  }

  m_active_request = request;
  m_expected_replay_frame = request.restore_before_emulated_frame;
  m_replay_frame_started = false;
  m_state = State::Replaying;
  return RequestStatus::Started;
}

RollbackCoordinator::FrameCompleteStatus RollbackCoordinator::CompleteFrame(const u64 frame)
{
  if (m_state == State::Faulted)
    return FrameCompleteStatus::Faulted;
  if (m_state != State::Replaying)
    return FrameCompleteStatus::NotReplaying;
  if (!m_active_request || !m_expected_replay_frame || frame != *m_expected_replay_frame ||
      !m_replay_frame_started)
  {
    EnterFaultedState();
    return FrameCompleteStatus::UnexpectedFrame;
  }

  if (frame == m_active_request->replay_through_emulated_frame)
  {
    m_replay_frame_started = false;
    m_state = State::AwaitingCommit;
    return FrameCompleteStatus::AwaitingCommit;
  }

  if (frame == std::numeric_limits<u64>::max())
  {
    EnterFaultedState();
    return FrameCompleteStatus::UnexpectedFrame;
  }
  m_expected_replay_frame = frame + 1;
  m_replay_frame_started = false;
  return FrameCompleteStatus::ReplayContinues;
}

bool RollbackCoordinator::CommitReplay(RollbackSIInputJournal& journal)
{
  if (!m_active_request)
    return false;
  return journal.AcknowledgeAndCommitReplay(*m_active_request, *this);
}

bool RollbackCoordinator::CanCommitReplay(const ReplayRequest& request) const
{
  return m_state == State::AwaitingCommit && m_active_request && request == *m_active_request &&
         m_output_gate_active;
}

bool RollbackCoordinator::CommitAcknowledgedReplay(const ReplayRequest& request)
{
  // AcknowledgeAndCommitReplay checked this under the journal lock immediately
  // before calling us. Coordinator ownership remains on the CPU thread.
  if (!CanCommitReplay(request))
    return false;
  const bool published = m_output_gate.EndHiddenReplay(true);
  m_output_gate_active = false;
  m_active_request.reset();
  m_expected_replay_frame.reset();
  m_replay_frame_started = false;
  m_state = published ? State::Ready : State::Faulted;
  return published;
}

RollbackCoordinator::RequestStatus
RollbackCoordinator::RestartRollbackWhileHidden(const ReplayRequest& replacement_request)
{
  if (m_state == State::Disabled)
    return RequestStatus::Disabled;
  if (m_state == State::Faulted)
    return RequestStatus::Faulted;
  if (m_state != State::AwaitingCommit || !m_output_gate_active)
    return RequestStatus::Busy;
  if (const RequestStatus validation = ValidateRequest(replacement_request);
      validation != RequestStatus::Started)
  {
    return validation;
  }

  // The original gate remains armed across chained corrections. Restoring
  // while hidden prevents a stale corrected frontier from reaching the host.
  if (!m_state_store.RestoreFrameStart(replacement_request.restore_before_emulated_frame))
  {
    EnterFaultedState();
    return RequestStatus::RestoreFailed;
  }

  m_active_request = replacement_request;
  m_expected_replay_frame = replacement_request.restore_before_emulated_frame;
  m_replay_frame_started = false;
  m_state = State::Replaying;
  return RequestStatus::Started;
}

bool RollbackCoordinator::CancelReplay()
{
  if ((m_state != State::Replaying && m_state != State::AwaitingCommit) || !m_output_gate_active)
    return false;

  m_output_gate.EndHiddenReplay(false);
  m_output_gate_active = false;
  m_active_request.reset();
  m_expected_replay_frame.reset();
  m_replay_frame_started = false;
  // A cancelled replay leaves the emulator at an intermediate historical
  // state. It is not safe to resume or publish from there without another
  // verified restore, so require the session owner to stop/fallback explicitly.
  m_state = State::Faulted;
  return true;
}

RollbackCoordinator::RequestStatus
RollbackCoordinator::ValidateRequest(const ReplayRequest& request) const
{
  if (request.replay_through_emulated_frame < request.restore_before_emulated_frame)
    return RequestStatus::InvalidRange;
  if (request.timeline_request.first_incorrect_frame != request.first_incorrect_batch_id ||
      request.timeline_request.replay_through_frame != request.replay_through_batch_id ||
      request.emulated_frame_request.first_incorrect_frame !=
          request.restore_before_emulated_frame ||
      request.emulated_frame_request.replay_through_frame !=
          request.replay_through_emulated_frame ||
      request.timeline_request.generation != request.emulated_frame_request.generation ||
      request.first_replay_batch_id > request.first_incorrect_batch_id ||
      request.first_incorrect_batch_id > request.replay_through_batch_id)
  {
    return RequestStatus::InvalidMapping;
  }
  const u64 replay_distance =
      request.replay_through_emulated_frame - request.restore_before_emulated_frame;
  if (replay_distance >= m_config.max_replay_frames)
    return RequestStatus::BeyondReplayHorizon;
  if (!m_state_store.HasFrameStart(request.restore_before_emulated_frame))
    return RequestStatus::SnapshotUnavailable;
  return RequestStatus::Started;
}

void RollbackCoordinator::EnterFaultedState()
{
  if (m_output_gate_active)
  {
    m_output_gate.EndHiddenReplay(false);
    m_output_gate_active = false;
  }
  m_active_request.reset();
  m_expected_replay_frame.reset();
  m_replay_frame_started = false;
  m_state = State::Faulted;
}

}  // namespace NetPlay
