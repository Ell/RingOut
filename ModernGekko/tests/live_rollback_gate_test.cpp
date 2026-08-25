// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/LiveRollbackFrameBoundary.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/NetPlay/LiveRollbackOutputGate.h"
#include "Core/NetPlay/RollbackCoordinator.h"
#include "Core/NetPlay/RollbackSIInputJournal.h"
#include "Core/State.h"

#include <array>
#include <cstdio>
#include <set>

namespace {
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "live_rollback_gate_test:%d: %s\n", __LINE__,       \
                   #condition);                                                \
      return false;                                                            \
    }                                                                          \
  } while (false)

using Boundary = NetPlay::LiveRollbackFrameBoundary;
using Coordinator = NetPlay::RollbackCoordinator;
using Gate = NetPlay::LiveRollbackOutputGate;
using Journal = NetPlay::RollbackSIInputJournal;

int s_frontier_barrier_phase = 0;

bool BeginObservedCorrectedFrontierBarrier(bool *gpu_was_running) {
  if (s_frontier_barrier_phase != 0 ||
      !NetPlay::IsLiveRollbackHiddenReplayActive())
    return false;
  s_frontier_barrier_phase = 1;
  *gpu_was_running = true;
  return true;
}

void EndObservedCorrectedFrontierBarrier(bool gpu_was_running) {
  if (s_frontier_barrier_phase == 1 && gpu_was_running &&
      !NetPlay::IsLiveRollbackHiddenReplayActive())
    s_frontier_barrier_phase = 2;
}

bool RejectCorrectedFrontierBarrier(bool *) { return false; }

class FakeStateStore final : public NetPlay::RollbackStateStore {
public:
  bool CaptureFrameStart(const u64 frame) override {
    frames.insert(frame);
    return true;
  }
  bool HasFrameStart(const u64 frame) const override {
    return frames.contains(frame);
  }
  bool RestoreFrameStart(const u64 frame) override {
    restored = frame;
    return frames.contains(frame);
  }

  std::set<u64> frames;
  std::optional<u64> restored;
};

Journal::Config MakeJournalConfig() {
  return {.session_generation = 9,
          .first_batch_id = 20,
          .history_capacity = 8,
          .max_prediction_batches = 2,
          .max_polls_per_frame = 2,
          .pad_authority = {Journal::Timeline::PadAuthority::Local,
                            Journal::Timeline::PadAuthority::Remote,
                            Journal::Timeline::PadAuthority::Inactive,
                            Journal::Timeline::PadAuthority::Inactive}};
}

GCPadStatus Pad(const u16 buttons) {
  GCPadStatus pad{};
  pad.button = buttons;
  return pad;
}

bool TestProductionCoverageAndPolicyFailClosed() {
  CHECK(NetPlay::IsLiveRollbackMemoryCardSlotSafe(
      ExpansionInterface::EXIDeviceType::None));
  CHECK(NetPlay::IsLiveRollbackMemoryCardSlotSafe(
      ExpansionInterface::EXIDeviceType::MemoryCardFolder));
  CHECK(!NetPlay::IsLiveRollbackMemoryCardSlotSafe(
      ExpansionInterface::EXIDeviceType::MemoryCard));
  const NetPlay::LiveRollbackProductionSessionPolicy safe_policy{
      .is_gamecube_title = true,
      .save_data_writable = false,
      .sd_writes_allowed = false,
      .memory_card_slots_safe = true,
      .serial_port_1_disabled = true,
      .serial_port_2_disabled = true,
      .gba_devices_disabled = true,
  };
  CHECK(NetPlay::IsLiveRollbackProductionSessionPolicySafe(safe_policy));
  auto unsafe_policy = safe_policy;
  unsafe_policy.serial_port_2_disabled = false;
  CHECK(!NetPlay::IsLiveRollbackProductionSessionPolicySafe(unsafe_policy));
  CHECK(NetPlay::IsLiveRollbackProductionReady());
  CHECK(Gate::CreateProductionGate(safe_policy));
  CHECK(!Gate::CreateProductionGate(unsafe_policy));
  Gate gate;
  CHECK(gate.HasCompleteCoverage());

  const auto &coverage = gate.GetCoverage();
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::VideoPresentation)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::FrameDumping)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::AudioPresentation)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::BufferedAudioReconciliation)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::ControllerRumble)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::Achievements)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::MovieRecording)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::PersistentStorage)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::ReplayDerivedNetPlayOutbound)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::GuestNetwork)]);
  CHECK(coverage[static_cast<std::size_t>(
      NetPlay::RollbackHostEffect::CorrectedFrontierPublication)]);

  FakeStateStore store;
  Coordinator coordinator({.enabled = true, .max_replay_frames = 2}, store,
                          gate);
  Journal journal(MakeJournalConfig());
  Boundary boundary(coordinator, journal, gate);
  CHECK(boundary.Activate(100) == Boundary::ActivationStatus::Active);
  CHECK(store.frames.contains(100));
  return true;
}

bool TestGlobalGateOwnershipAndFaultLatch() {
  Gate first(Gate::CompleteCoverageForTesting());
  Gate second(Gate::CompleteCoverageForTesting());
  first.SetCorrectedFrontierBarrierForTesting(
      &BeginObservedCorrectedFrontierBarrier,
      &EndObservedCorrectedFrontierBarrier);
  const u64 first_generation = NetPlay::GetLiveRollbackAudioResetGeneration();
  CHECK(!first.BeginHiddenReplay(4, 3));
  CHECK(NetPlay::GetLiveRollbackAudioResetGeneration() == first_generation);
  CHECK(first.BeginHiddenReplay(3, 4));
  CHECK(NetPlay::GetLiveRollbackAudioResetGeneration() == first_generation + 1);
  CHECK(NetPlay::IsLiveRollbackHiddenReplayActive());
  CHECK(!NetPlay::IsLiveRollbackReplayDerivedOutboundAllowed());
  CHECK(!second.BeginHiddenReplay(3, 4));
  CHECK(first.EndHiddenReplay(true));
  CHECK(s_frontier_barrier_phase == 2);
  CHECK(!NetPlay::IsLiveRollbackHiddenReplayActive());
  CHECK(NetPlay::IsLiveRollbackReplayDerivedOutboundAllowed());

  CHECK(second.BeginHiddenReplay(5, 5));
  CHECK(NetPlay::GetLiveRollbackAudioResetGeneration() == first_generation + 2);
  CHECK(!second.EndHiddenReplay(false));
  CHECK(second.IsFaulted());
  CHECK(NetPlay::IsLiveRollbackHiddenReplayActive());
  CHECK(!NetPlay::IsLiveRollbackReplayDerivedOutboundAllowed());
  CHECK(!second.BeginHiddenReplay(6, 6));
  NetPlay::FinalizeLiveRollbackOutputSuppressionAfterCoreTeardown();
  CHECK(!NetPlay::IsLiveRollbackHiddenReplayActive());
  CHECK(NetPlay::IsLiveRollbackReplayDerivedOutboundAllowed());

  {
    Gate rejected(Gate::CompleteCoverageForTesting());
    rejected.SetCorrectedFrontierBarrierForTesting(
        &RejectCorrectedFrontierBarrier, &EndObservedCorrectedFrontierBarrier);
    CHECK(rejected.BeginHiddenReplay(7, 7));
    CHECK(!rejected.EndHiddenReplay(true));
    CHECK(rejected.IsFaulted());
    CHECK(NetPlay::IsLiveRollbackHiddenReplayActive());
  }
  CHECK(NetPlay::IsLiveRollbackHiddenReplayActive());
  NetPlay::FinalizeLiveRollbackOutputSuppressionAfterCoreTeardown();
  CHECK(!NetPlay::IsLiveRollbackHiddenReplayActive());

  {
    Gate abandoned(Gate::CompleteCoverageForTesting());
    CHECK(abandoned.BeginSessionQuarantine());
    CHECK(abandoned.BeginHiddenReplay(8, 8));
  }
  CHECK(NetPlay::IsLiveRollbackHiddenReplayActive());
  CHECK(NetPlay::IsLiveRollbackSessionQuarantineActive());
  NetPlay::FinalizeLiveRollbackOutputSuppressionAfterCoreTeardown();
  CHECK(!NetPlay::IsLiveRollbackHiddenReplayActive());
  CHECK(!NetPlay::IsLiveRollbackSessionQuarantineActive());
  return true;
}

bool TestSessionQuarantineOwnershipAndDestruction() {
  CHECK(!NetPlay::IsLiveRollbackSessionQuarantineActive());
  Gate second(Gate::CompleteCoverageForTesting());
  {
    Gate first(Gate::CompleteCoverageForTesting());
    CHECK(first.BeginSessionQuarantine());
    CHECK(first.IsSessionQuarantineActive());
    CHECK(NetPlay::IsLiveRollbackSessionQuarantineActive());
    CHECK(!second.BeginSessionQuarantine());
  }
  CHECK(!NetPlay::IsLiveRollbackSessionQuarantineActive());
  CHECK(second.BeginSessionQuarantine());
  second.EndSessionQuarantine();
  CHECK(!second.IsSessionQuarantineActive());
  CHECK(!NetPlay::IsLiveRollbackSessionQuarantineActive());
  return true;
}

bool TestRollbackSnapshotScopeNestsAndReleases() {
  CHECK(!State::IsRollbackSnapshotActive());
  CHECK(State::ShouldSerializeHostMixerState());
  {
    const State::ScopedRollbackSnapshot outer;
    CHECK(State::IsRollbackSnapshotActive());
    CHECK(!State::ShouldSerializeHostMixerState());
    CHECK(State::ShouldSerializeMemoryCardState(false));
    {
      const State::ScopedRollbackSnapshot inner;
      CHECK(State::IsRollbackSnapshotActive());
    }
    CHECK(State::IsRollbackSnapshotActive());
  }
  CHECK(!State::IsRollbackSnapshotActive());
  CHECK(State::ShouldSerializeHostMixerState());
  CHECK(!State::ShouldSerializeMemoryCardState(false));
  CHECK(State::ShouldSerializeMemoryCardState(true));
  return true;
}

bool TestFrameBoundaryCorrectionAndCommit() {
  FakeStateStore store;
  Gate gate(Gate::CompleteCoverageForTesting());
  Coordinator coordinator({.enabled = true, .max_replay_frames = 2}, store,
                          gate);
  Journal journal(MakeJournalConfig());
  Boundary boundary(coordinator, journal, gate);

  CHECK(journal.ObserveAppliedBatch(1, 9,
                                    {.batch_id = 20,
                                     .emulated_frame = 100,
                                     .poll_ordinal = 0,
                                     .requested_pad_mask = 0b0011}) ==
        Journal::ObserveStatus::Accepted);
  std::array<GCPadStatus, Journal::Timeline::PAD_COUNT> local{};
  std::array<GCPadStatus, Journal::Timeline::PAD_COUNT> remote{};
  local[0] = Pad(PAD_BUTTON_A);
  remote[1] = Pad(PAD_BUTTON_B);
  CHECK(journal.SubmitInputBatch(
            1, 9, Journal::InputSource::Local,
            {.batch_id = 20, .pad_mask = 0b0001, .pads = local}) ==
        Journal::SubmitStatus::Accepted);
  CHECK(journal.ResolveBatch(20));
  CHECK(journal.SubmitInputBatch(
            1, 9, Journal::InputSource::Remote,
            {.batch_id = 20, .pad_mask = 0b0010, .pads = remote}) ==
        Journal::SubmitStatus::CorrectedPrediction);

  CHECK(boundary.Activate(100) == Boundary::ActivationStatus::Active);
  CHECK(boundary.CompleteCurrentFrame() ==
        Boundary::BoundaryStatus::RollbackStarted);
  CHECK(store.restored == 100);
  CHECK(gate.IsActive());
  CHECK(boundary.GetCurrentFrame() == 100);

  CHECK(boundary.CompleteCurrentFrame() ==
        Boundary::BoundaryStatus::ReplayCommitted);
  CHECK(!gate.IsActive());
  CHECK(!NetPlay::IsLiveRollbackHiddenReplayActive());
  CHECK(boundary.GetCurrentFrame() == 101);
  CHECK(coordinator.GetState() == Coordinator::State::Ready);
  return true;
}

bool TestFrameBoundaryDeactivateLatchesHiddenReplayUntilTeardown() {
  FakeStateStore store;
  Gate gate(Gate::CompleteCoverageForTesting());
  Coordinator coordinator({.enabled = true, .max_replay_frames = 2}, store,
                          gate);
  Journal journal(MakeJournalConfig());
  Boundary boundary(coordinator, journal, gate);

  CHECK(journal.ObserveAppliedBatch(1, 9,
                                    {.batch_id = 20,
                                     .emulated_frame = 100,
                                     .poll_ordinal = 0,
                                     .requested_pad_mask = 0b0011}) ==
        Journal::ObserveStatus::Accepted);
  std::array<GCPadStatus, Journal::Timeline::PAD_COUNT> local{};
  std::array<GCPadStatus, Journal::Timeline::PAD_COUNT> remote{};
  local[0] = Pad(PAD_BUTTON_A);
  remote[1] = Pad(PAD_BUTTON_B);
  CHECK(journal.SubmitInputBatch(
            1, 9, Journal::InputSource::Local,
            {.batch_id = 20, .pad_mask = 0b0001, .pads = local}) ==
        Journal::SubmitStatus::Accepted);
  CHECK(journal.ResolveBatch(20));
  CHECK(journal.SubmitInputBatch(
            1, 9, Journal::InputSource::Remote,
            {.batch_id = 20, .pad_mask = 0b0010, .pads = remote}) ==
        Journal::SubmitStatus::CorrectedPrediction);

  CHECK(boundary.Activate(100) == Boundary::ActivationStatus::Active);
  CHECK(boundary.CompleteCurrentFrame() ==
        Boundary::BoundaryStatus::RollbackStarted);
  CHECK(gate.IsActive());
  CHECK(NetPlay::IsLiveRollbackHiddenReplayActive());

  boundary.Deactivate();
  CHECK(!boundary.GetCurrentFrame());
  CHECK(!gate.IsActive());
  CHECK(gate.IsFaulted());
  CHECK(NetPlay::IsLiveRollbackHiddenReplayActive());
  CHECK(coordinator.GetState() == Coordinator::State::Faulted);
  NetPlay::FinalizeLiveRollbackOutputSuppressionAfterCoreTeardown();
  CHECK(!NetPlay::IsLiveRollbackHiddenReplayActive());
  return true;
}
} // namespace

int main() {
  return TestProductionCoverageAndPolicyFailClosed() &&
                 TestGlobalGateOwnershipAndFaultLatch() &&
                 TestSessionQuarantineOwnershipAndDestruction() &&
                 TestRollbackSnapshotScopeNestsAndReleases() &&
                 TestFrameBoundaryCorrectionAndCommit() &&
                 TestFrameBoundaryDeactivateLatchesHiddenReplayUntilTeardown()
             ? 0
             : 1;
}
