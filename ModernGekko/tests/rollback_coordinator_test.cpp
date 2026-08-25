// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackCoordinator.h"

#include <array>
#include <cstdio>
#include <optional>
#include <set>
#include <vector>

namespace {
using Coordinator = NetPlay::RollbackCoordinator;
using Request = Coordinator::ReplayRequest;
using Journal = NetPlay::RollbackSIInputJournal;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "rollback_coordinator_test:%d: %s\n", __LINE__,     \
                   #condition);                                                \
      return false;                                                            \
    }                                                                          \
  } while (false)

class FakeStateStore final : public NetPlay::RollbackStateStore {
public:
  bool CaptureFrameStart(const u64 frame) override {
    captures.push_back(frame);
    if (fail_capture == frame)
      return false;
    frames.insert(frame);
    return true;
  }

  bool HasFrameStart(const u64 frame) const override {
    return frames.contains(frame);
  }

  bool RestoreFrameStart(const u64 frame) override {
    restores.push_back(frame);
    return fail_restore != frame && frames.contains(frame);
  }

  std::set<u64> frames;
  std::vector<u64> captures;
  std::vector<u64> restores;
  std::optional<u64> fail_capture;
  std::optional<u64> fail_restore;
};

class FakeOutputGate final : public NetPlay::RollbackOutputGate {
public:
  bool BeginHiddenReplay(const u64 first_frame,
                         const u64 replay_through_frame) override {
    begins.emplace_back(first_frame, replay_through_frame);
    active = allow_begin;
    return allow_begin;
  }

  bool EndHiddenReplay(const bool publishable) override {
    ends.push_back(publishable);
    active = false;
    return publishable && allow_end;
  }

  bool allow_begin = true;
  bool allow_end = true;
  bool active = false;
  std::vector<std::pair<u64, u64>> begins;
  std::vector<bool> ends;
};

Request Replay(const u64 first_frame, const u64 through_frame,
               const u64 generation) {
  return {.timeline_request = {.first_incorrect_frame = 1000 + first_frame,
                               .replay_through_frame = 1000 + through_frame,
                               .generation = generation},
          .emulated_frame_request = {.first_incorrect_frame = first_frame,
                                     .replay_through_frame = through_frame,
                                     .generation = generation},
          .restore_before_emulated_frame = first_frame,
          .first_replay_batch_id = 1000 + first_frame,
          .first_incorrect_batch_id = 1000 + first_frame,
          .replay_through_batch_id = 1000 + through_frame,
          .replay_through_emulated_frame = through_frame};
}

GCPadStatus Pad(const u16 buttons) {
  GCPadStatus pad{};
  pad.button = buttons;
  return pad;
}

std::array<GCPadStatus, Journal::Timeline::PAD_COUNT>
Pads(const GCPadStatus &pad0 = {}, const GCPadStatus &pad1 = {}) {
  return {pad0, pad1, {}, {}};
}

NetPlay::RollbackSIInputBatch
Batch(const u64 id, const u8 mask,
      const std::array<GCPadStatus, Journal::Timeline::PAD_COUNT> &pads) {
  return {.batch_id = id, .pad_mask = mask, .pads = pads};
}

Journal::AppliedBatch Applied(const u64 id, const u64 frame,
                              const u32 ordinal = 0) {
  return {.batch_id = id,
          .emulated_frame = frame,
          .poll_ordinal = ordinal,
          .requested_pad_mask = 0b0011};
}

Journal::Config JournalConfig() {
  return {.session_generation = 42,
          .first_batch_id = 10,
          .history_capacity = 16,
          .max_prediction_batches = 3,
          .max_polls_per_frame = 4,
          .pad_authority = {Journal::Timeline::PadAuthority::Local,
                            Journal::Timeline::PadAuthority::Remote,
                            Journal::Timeline::PadAuthority::Inactive,
                            Journal::Timeline::PadAuthority::Inactive}};
}

bool TestDisabledByDefault() {
  FakeStateStore store;
  FakeOutputGate gate;
  Coordinator coordinator({}, store, gate);
  CHECK(coordinator.GetState() == Coordinator::State::Disabled);
  CHECK(coordinator.BeginFrame(10) == Coordinator::FrameStartStatus::Disabled);
  CHECK(coordinator.StartRollback(Replay(10, 11, 1)) ==
        Coordinator::RequestStatus::Disabled);
  CHECK(store.captures.empty());
  CHECK(gate.begins.empty());
  return true;
}

bool TestCaptureRestoreHiddenReplayAndCancel() {
  FakeStateStore store;
  FakeOutputGate gate;
  Coordinator coordinator({.enabled = true, .max_replay_frames = 3}, store,
                          gate);
  const Request request = Replay(11, 12, 7);

  CHECK(coordinator.BeginFrame(10) == Coordinator::FrameStartStatus::Captured);
  CHECK(coordinator.BeginFrame(11) == Coordinator::FrameStartStatus::Captured);
  CHECK(coordinator.BeginFrame(12) == Coordinator::FrameStartStatus::Captured);
  CHECK(coordinator.StartRollback(request) ==
        Coordinator::RequestStatus::Started);
  CHECK(gate.active);
  CHECK(store.restores == std::vector<u64>{11});
  CHECK(coordinator.GetExpectedReplayFrame() == 11);

  CHECK(coordinator.BeginFrame(11) ==
        Coordinator::FrameStartStatus::RestoredFrameAlreadyCaptured);
  CHECK(coordinator.CompleteFrame(11) ==
        Coordinator::FrameCompleteStatus::ReplayContinues);
  CHECK(coordinator.GetExpectedReplayFrame() == 12);
  CHECK(coordinator.BeginFrame(12) == Coordinator::FrameStartStatus::Captured);
  CHECK(store.captures == std::vector<u64>({10, 11, 12, 12}));
  CHECK(coordinator.CompleteFrame(12) ==
        Coordinator::FrameCompleteStatus::AwaitingCommit);
  CHECK(gate.active);
  CHECK(coordinator.BeginFrame(13) ==
        Coordinator::FrameStartStatus::AwaitingCommit);

  CHECK(coordinator.CancelReplay());
  CHECK(!gate.active);
  CHECK(gate.ends == std::vector<bool>{false});
  CHECK(coordinator.GetState() == Coordinator::State::Faulted);
  return true;
}

bool TestRequestFailsClosedBeforeRestore() {
  FakeStateStore store;
  FakeOutputGate gate;
  Coordinator coordinator({.enabled = true, .max_replay_frames = 2}, store,
                          gate);

  CHECK(coordinator.StartRollback(Replay(5, 5, 1)) ==
        Coordinator::RequestStatus::SnapshotUnavailable);
  CHECK(gate.begins.empty());
  CHECK(store.restores.empty());

  CHECK(coordinator.BeginFrame(5) == Coordinator::FrameStartStatus::Captured);
  CHECK(coordinator.StartRollback(Replay(5, 4, 1)) ==
        Coordinator::RequestStatus::InvalidRange);
  Request invalid_mapping = Replay(5, 6, 1);
  ++invalid_mapping.first_incorrect_batch_id;
  CHECK(coordinator.StartRollback(invalid_mapping) ==
        Coordinator::RequestStatus::InvalidMapping);
  CHECK(coordinator.StartRollback(Replay(5, 7, 1)) ==
        Coordinator::RequestStatus::BeyondReplayHorizon);

  gate.allow_begin = false;
  CHECK(coordinator.StartRollback(Replay(5, 6, 1)) ==
        Coordinator::RequestStatus::OutputSuppressionUnavailable);
  CHECK(store.restores.empty());
  CHECK(coordinator.GetState() == Coordinator::State::Ready);
  return true;
}

bool TestJournalAcknowledgementAndPublicationAreAtomic() {
  Journal journal(JournalConfig());
  CHECK(journal.GetConfigurationStatus() ==
        Journal::ConfigurationStatus::Valid);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(10, 100)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(11, 100, 1)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(12, 102)) ==
        Journal::ObserveStatus::Accepted);

  for (u64 id = 10; id <= 12; ++id) {
    CHECK(
        journal.SubmitInputBatch(1, 42, Journal::InputSource::Local,
                                 Batch(id, 0b0001, Pads(Pad(PAD_BUTTON_A)))) ==
        Journal::SubmitStatus::Accepted);
  }
  CHECK(journal.SubmitInputBatch(
            1, 42, Journal::InputSource::Remote,
            Batch(10, 0b0010, Pads({}, Pad(PAD_BUTTON_B)))) ==
        Journal::SubmitStatus::Accepted);
  CHECK(journal.ResolveBatch(10));
  CHECK(journal.ResolveBatch(11));
  CHECK(journal.ResolveBatch(12));
  CHECK(journal.SubmitInputBatch(
            1, 42, Journal::InputSource::Remote,
            Batch(11, 0b0010, Pads({}, Pad(PAD_BUTTON_Y)))) ==
        Journal::SubmitStatus::CorrectedPrediction);

  const auto first_trigger = journal.GetReplayTrigger();
  CHECK(first_trigger);
  CHECK(first_trigger->first_incorrect_batch_id == 11);
  CHECK(first_trigger->restore_before_emulated_frame == 100);
  CHECK(first_trigger->replay_through_emulated_frame == 102);

  FakeStateStore store;
  FakeOutputGate gate;
  Coordinator coordinator({.enabled = true, .max_replay_frames = 3}, store,
                          gate);
  CHECK(coordinator.BeginFrame(100) == Coordinator::FrameStartStatus::Captured);
  CHECK(coordinator.StartRollback(*first_trigger) ==
        Coordinator::RequestStatus::Started);
  for (u64 frame = 100; frame <= 102; ++frame) {
    const auto start = coordinator.BeginFrame(frame);
    CHECK(start ==
          (frame == 100
               ? Coordinator::FrameStartStatus::RestoredFrameAlreadyCaptured
               : Coordinator::FrameStartStatus::Captured));
    const auto complete = coordinator.CompleteFrame(frame);
    CHECK(complete ==
          (frame == 102 ? Coordinator::FrameCompleteStatus::AwaitingCommit
                        : Coordinator::FrameCompleteStatus::ReplayContinues));
  }

  // Model a packet received after replay reached its frontier but before the
  // CPU thread attempts to publish. Commit must observe the new generation
  // under the same journal lock used by SubmitInputBatch and leave output
  // hidden.
  CHECK(journal.SubmitInputBatch(
            1, 42, Journal::InputSource::Remote,
            Batch(12, 0b0010, Pads({}, Pad(PAD_BUTTON_X)))) ==
        Journal::SubmitStatus::CorrectedPrediction);
  CHECK(!coordinator.CommitReplay(journal));
  CHECK(gate.active);
  CHECK(gate.ends.empty());

  const auto replacement = journal.GetReplayTrigger();
  CHECK(replacement);
  CHECK(replacement->timeline_request.generation >
        first_trigger->timeline_request.generation);
  CHECK(coordinator.RestartRollbackWhileHidden(*replacement) ==
        Coordinator::RequestStatus::Started);
  for (u64 frame = 100; frame <= 102; ++frame) {
    CHECK(coordinator.BeginFrame(frame) ==
          (frame == 100
               ? Coordinator::FrameStartStatus::RestoredFrameAlreadyCaptured
               : Coordinator::FrameStartStatus::Captured));
    CHECK(coordinator.CompleteFrame(frame) ==
          (frame == 102 ? Coordinator::FrameCompleteStatus::AwaitingCommit
                        : Coordinator::FrameCompleteStatus::ReplayContinues));
  }
  CHECK(coordinator.CommitReplay(journal));
  CHECK(!gate.active);
  CHECK(gate.ends == std::vector<bool>{true});
  CHECK(!journal.GetReplayTrigger());
  CHECK(coordinator.GetState() == Coordinator::State::Ready);
  return true;
}

bool TestRestoreAndScheduleFailuresFault() {
  {
    FakeStateStore store;
    FakeOutputGate gate;
    Coordinator coordinator({.enabled = true, .max_replay_frames = 2}, store,
                            gate);
    CHECK(coordinator.BeginFrame(20) ==
          Coordinator::FrameStartStatus::Captured);
    store.fail_restore = 20;
    CHECK(coordinator.StartRollback(Replay(20, 20, 1)) ==
          Coordinator::RequestStatus::RestoreFailed);
    CHECK(coordinator.GetState() == Coordinator::State::Faulted);
    CHECK(!gate.active);
    CHECK(gate.ends == std::vector<bool>{false});
  }

  {
    FakeStateStore store;
    FakeOutputGate gate;
    Coordinator coordinator({.enabled = true, .max_replay_frames = 2}, store,
                            gate);
    CHECK(coordinator.BeginFrame(30) ==
          Coordinator::FrameStartStatus::Captured);
    CHECK(coordinator.StartRollback(Replay(30, 31, 1)) ==
          Coordinator::RequestStatus::Started);
    CHECK(coordinator.CompleteFrame(30) ==
          Coordinator::FrameCompleteStatus::UnexpectedFrame);
    CHECK(coordinator.GetState() == Coordinator::State::Faulted);
    CHECK(gate.ends == std::vector<bool>{false});
  }

  {
    FakeStateStore store;
    FakeOutputGate gate;
    Coordinator coordinator({.enabled = true, .max_replay_frames = 2}, store,
                            gate);
    CHECK(coordinator.BeginFrame(30) ==
          Coordinator::FrameStartStatus::Captured);
    CHECK(coordinator.StartRollback(Replay(30, 31, 1)) ==
          Coordinator::RequestStatus::Started);
    CHECK(coordinator.BeginFrame(31) ==
          Coordinator::FrameStartStatus::UnexpectedFrame);
    CHECK(coordinator.GetState() == Coordinator::State::Faulted);
    CHECK(gate.ends == std::vector<bool>{false});
  }
  return true;
}

bool TestPublicationFailureFaultsCommit() {
  Journal journal(JournalConfig());
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(10, 100)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.SubmitInputBatch(1, 42, Journal::InputSource::Local,
                                 Batch(10, 0b0001, Pads(Pad(PAD_BUTTON_A)))) ==
        Journal::SubmitStatus::Accepted);
  CHECK(journal.ResolveBatch(10));
  CHECK(journal.SubmitInputBatch(
            1, 42, Journal::InputSource::Remote,
            Batch(10, 0b0010, Pads({}, Pad(PAD_BUTTON_B)))) ==
        Journal::SubmitStatus::CorrectedPrediction);

  const auto trigger = journal.GetReplayTrigger();
  CHECK(trigger);
  FakeStateStore store;
  FakeOutputGate gate;
  gate.allow_end = false;
  Coordinator coordinator({.enabled = true, .max_replay_frames = 2}, store,
                          gate);
  CHECK(coordinator.BeginFrame(100) == Coordinator::FrameStartStatus::Captured);
  CHECK(coordinator.StartRollback(*trigger) ==
        Coordinator::RequestStatus::Started);
  CHECK(coordinator.BeginFrame(100) ==
        Coordinator::FrameStartStatus::RestoredFrameAlreadyCaptured);
  CHECK(coordinator.CompleteFrame(100) ==
        Coordinator::FrameCompleteStatus::AwaitingCommit);

  CHECK(!coordinator.CommitReplay(journal));
  CHECK(coordinator.GetState() == Coordinator::State::Faulted);
  CHECK(!gate.active);
  CHECK(gate.ends == std::vector<bool>{true});
  CHECK(!journal.GetReplayTrigger());
  return true;
}

bool TestCorrectedCaptureFailureAndCancellation() {
  FakeStateStore store;
  FakeOutputGate gate;
  Coordinator coordinator({.enabled = true, .max_replay_frames = 3}, store,
                          gate);
  CHECK(coordinator.BeginFrame(40) == Coordinator::FrameStartStatus::Captured);
  CHECK(coordinator.StartRollback(Replay(40, 41, 1)) ==
        Coordinator::RequestStatus::Started);
  CHECK(coordinator.BeginFrame(40) ==
        Coordinator::FrameStartStatus::RestoredFrameAlreadyCaptured);
  CHECK(coordinator.CompleteFrame(40) ==
        Coordinator::FrameCompleteStatus::ReplayContinues);
  store.fail_capture = 41;
  CHECK(coordinator.BeginFrame(41) ==
        Coordinator::FrameStartStatus::CaptureFailed);
  CHECK(coordinator.GetState() == Coordinator::State::Faulted);
  CHECK(gate.ends == std::vector<bool>{false});

  FakeStateStore cancel_store;
  FakeOutputGate cancel_gate;
  Coordinator cancel({.enabled = true, .max_replay_frames = 2}, cancel_store,
                     cancel_gate);
  CHECK(cancel.BeginFrame(50) == Coordinator::FrameStartStatus::Captured);
  CHECK(cancel.StartRollback(Replay(50, 50, 2)) ==
        Coordinator::RequestStatus::Started);
  CHECK(cancel.CancelReplay());
  CHECK(cancel.GetState() == Coordinator::State::Faulted);
  CHECK(cancel_gate.ends == std::vector<bool>{false});
  return true;
}
} // namespace

int main() {
  if (!TestDisabledByDefault() || !TestCaptureRestoreHiddenReplayAndCancel() ||
      !TestRequestFailsClosedBeforeRestore() ||
      !TestJournalAcknowledgementAndPublicationAreAtomic() ||
      !TestRestoreAndScheduleFailuresFault() ||
      !TestPublicationFailureFaultsCommit() ||
      !TestCorrectedCaptureFailureAndCancellation()) {
    return 1;
  }
  return 0;
}
