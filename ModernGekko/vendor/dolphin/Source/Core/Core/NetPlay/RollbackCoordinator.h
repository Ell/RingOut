// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>

#include "Common/CommonTypes.h"
#include "Core/NetPlay/RollbackSIInputJournal.h"

namespace NetPlay
{

// Storage is deliberately abstract so the replay state machine can be tested
// without booting an emulator. The production implementation uses synchronous
// State::SaveToBuffer/LoadFromBuffer calls on the CPU thread.
class RollbackStateStore
{
public:
  virtual ~RollbackStateStore() = default;

  // Capture/restore the state immediately before |frame| is simulated.
  virtual bool CaptureFrameStart(u64 frame) = 0;
  virtual bool HasFrameStart(u64 frame) const = 0;
  virtual bool RestoreFrameStart(u64 frame) = 0;
};

// Hidden replay is more than skipping a swap. A production gate must suppress
// presentation and duplicate audio, rumble, achievements, movie writes, and
// other host-visible effects while still allowing guest-visible GPU/audio state
// to advance. BeginHiddenReplay must return false until all those guarantees are
// available; the coordinator will not restore state without them. During hidden
// replay the gate must retain the corrected frontier needed for presentation.
// EndHiddenReplay(true) atomically publishes/deduplicates that frontier, while
// EndHiddenReplay(false) discards it and leaves the session unable to resume.
class RollbackOutputGate
{
public:
  virtual ~RollbackOutputGate() = default;

  virtual bool BeginHiddenReplay(u64 first_frame, u64 replay_through_frame) = 0;
  virtual void EndHiddenReplay(bool corrected_frontier_is_publishable) = 0;
};

// Not thread-safe: session code must marshal journal triggers and normal state
// transitions onto the CPU thread. CancelReplay and destruction may run on the
// session owner thread only after an external lifecycle barrier has excluded
// every CPU entry point; they perform no state capture/restore or guest work.
class RollbackCoordinator final
{
public:
  using ReplayRequest = RollbackSIInputJournal::ReplayTrigger;

  struct Config
  {
    // Explicit opt-in. No production caller enables this yet, so fixed-delay
    // remains the only live netplay mode.
    bool enabled = false;
    std::size_t max_replay_frames = 0;
  };

  enum class State : u8
  {
    Disabled,
    Ready,
    Replaying,
    AwaitingCommit,
    Faulted,
  };

  enum class FrameStartStatus : u8
  {
    Captured,
    RestoredFrameAlreadyCaptured,
    Disabled,
    AwaitingCommit,
    UnexpectedFrame,
    CaptureFailed,
    Faulted,
  };

  enum class RequestStatus : u8
  {
    Started,
    Disabled,
    Busy,
    InvalidRange,
    InvalidMapping,
    BeyondReplayHorizon,
    SnapshotUnavailable,
    OutputSuppressionUnavailable,
    RestoreFailed,
    Faulted,
  };

  enum class FrameCompleteStatus : u8
  {
    NotReplaying,
    ReplayContinues,
    AwaitingCommit,
    UnexpectedFrame,
    Faulted,
  };

  RollbackCoordinator(Config config, RollbackStateStore& state_store,
                      RollbackOutputGate& output_gate);
  ~RollbackCoordinator();

  // Called on the CPU thread immediately before normal or replay simulation of
  // a frame. During replay this replaces later speculative checkpoints with
  // corrected checkpoints; the restored first frame is already captured.
  FrameStartStatus BeginFrame(u64 frame);

  // Arms output suppression before restoring the checkpoint immediately before
  // the first incorrect frame. The caller then lets the ordinary CPU run loop
  // advance, sourcing corrected SI batches from RollbackSIInputJournal.
  RequestStatus StartRollback(const ReplayRequest& request);

  // Called at the same CPU-thread frame boundary as Core::FrameUpdateOnCPUThread.
  FrameCompleteStatus CompleteFrame(u64 frame);

  // CompleteFrame intentionally leaves output hidden at the replay frontier.
  // CommitReplay holds the journal lock across pending-generation validation,
  // acknowledgement, and publication, so a network correction cannot arrive
  // between acknowledgement and EndHiddenReplay(true). A stale request is
  // rejected while output remains hidden. RestartRollbackWhileHidden can then
  // consume the journal's newer request without exposing an intermediate
  // corrected-but-stale frame.
  bool CommitReplay(RollbackSIInputJournal& journal);
  RequestStatus RestartRollbackWhileHidden(const ReplayRequest& replacement_request);
  bool CancelReplay();

  State GetState() const { return m_state; }
  std::optional<ReplayRequest> GetActiveRequest() const { return m_active_request; }
  std::optional<u64> GetExpectedReplayFrame() const { return m_expected_replay_frame; }

private:
  friend class RollbackSIInputJournal;

  RequestStatus ValidateRequest(const ReplayRequest& request) const;
  bool CanCommitReplay(const ReplayRequest& request) const;
  void CommitAcknowledgedReplay(const ReplayRequest& request);
  void EnterFaultedState();

  const Config m_config;
  RollbackStateStore& m_state_store;
  RollbackOutputGate& m_output_gate;
  State m_state;
  std::optional<ReplayRequest> m_active_request;
  std::optional<u64> m_expected_replay_frame;
  bool m_replay_frame_started = false;
  bool m_output_gate_active = false;
};

}  // namespace NetPlay
