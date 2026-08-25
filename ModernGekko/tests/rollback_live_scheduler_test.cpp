// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/LiveRollbackInputScheduler.h"

#include <array>
#include <cstdio>

namespace {
using Scheduler = NetPlay::LiveRollbackInputScheduler;
using Timeline = NetPlay::RollbackInputTimeline;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "rollback_live_scheduler_test:%d: %s\n", __LINE__,  \
                   #condition);                                                \
      return false;                                                            \
    }                                                                          \
  } while (false)

GCPadStatus Pad(const u16 button) {
  GCPadStatus pad{};
  pad.button = button;
  pad.isConnected = true;
  return pad;
}

std::array<GCPadStatus, 4> LocalPads(const u16 button) {
  return {Pad(button), {}, {}, {}};
}

Scheduler::Config Config() {
  return {.session_generation = 77,
          .first_batch_id = 100,
          .history_capacity = 32,
          .base_delay_batches = 2,
          .max_prediction_batches = 2,
          .max_polls_per_frame = 4,
          .redundant_batch_count = 3,
          .pad_authority = {Timeline::PadAuthority::Local,
                            Timeline::PadAuthority::Remote,
                            Timeline::PadAuthority::Inactive,
                            Timeline::PadAuthority::Inactive}};
}

NetPlay::RollbackSIInputPacket RemotePacket(const u64 batch_id,
                                            const u16 button) {
  NetPlay::RollbackSIInputPacket packet{.session_generation = 77,
                                        .batch_count = 1};
  packet.batches[0].batch_id = batch_id;
  packet.batches[0].pad_mask = 0b0010;
  packet.batches[0].pads[1] = Pad(button);
  return packet;
}

bool TestDelayedSamplingCorrectionAndReplay() {
  Scheduler scheduler(Config());
  CHECK(scheduler.GetConfigurationStatus() ==
        Scheduler::ConfigurationStatus::Valid);

  const auto first =
      scheduler.BeginNormalBatch(10, 0, 0b0001, LocalPads(PAD_BUTTON_A));
  CHECK(first);
  CHECK(first.inputs.pads[0].button == 0);
  CHECK(first.inputs.pads[1].button == 0);
  CHECK(first.outgoing_packet &&
        first.outgoing_packet->batches[0].batch_id == 102);

  const auto second =
      scheduler.BeginNormalBatch(10, 1, 0b0001, LocalPads(PAD_BUTTON_X));
  CHECK(second);
  CHECK(second.inputs.pads[0].button == 0);

  const auto predicted =
      scheduler.BeginNormalBatch(11, 0, 0b0001, LocalPads(PAD_BUTTON_Y));
  CHECK(predicted);
  CHECK(predicted.inputs.pads[0].button == PAD_BUTTON_A);
  CHECK(predicted.inputs.pads[1].button == 0);
  CHECK(predicted.inputs.predicted_pad_mask == 0b0010);

  CHECK(scheduler.SubmitRemotePacket(RemotePacket(102, PAD_BUTTON_B)) ==
        Scheduler::RemotePacketStatus::CorrectedPrediction);

  // The correction arrived between two polls in the same emulated frame.
  // Normal execution consumes poll one before the frame-boundary hook can
  // restore, so the pending replay must grow to include that later batch.
  const auto later_poll =
      scheduler.BeginNormalBatch(11, 1, 0b0001, LocalPads(PAD_TRIGGER_Z));
  CHECK(later_poll);
  CHECK(later_poll.applied.batch_id == 103);

  const auto trigger = scheduler.GetReplayTrigger();
  CHECK(trigger);
  CHECK(trigger->restore_before_emulated_frame == 11);
  CHECK(trigger->first_incorrect_batch_id == 102);
  CHECK(trigger->replay_through_batch_id == 103);
  CHECK(scheduler.StartReplay(*trigger));

  const auto replay_first = scheduler.BeginReplayBatch(11, 0);
  CHECK(replay_first);
  CHECK(replay_first.inputs.pads[0].button == PAD_BUTTON_A);
  CHECK(replay_first.inputs.pads[1].button == PAD_BUTTON_B);
  CHECK(replay_first.inputs.predicted_pad_mask == 0);

  const auto replay_later = scheduler.BeginReplayBatch(11, 1);
  CHECK(replay_later);
  CHECK(replay_later.applied.batch_id == 103);
  CHECK(scheduler.BeginReplayBatch(11, 2).status ==
        Scheduler::BatchStatus::ReplayComplete);
  scheduler.CancelReplay();
  return true;
}

bool TestHardHorizonStallDoesNotResample() {
  Scheduler scheduler(Config());
  CHECK(scheduler.BeginNormalBatch(20, 0, 0b0001, LocalPads(PAD_BUTTON_A)));
  CHECK(scheduler.BeginNormalBatch(20, 1, 0b0001, LocalPads(PAD_BUTTON_B)));
  CHECK(scheduler.BeginNormalBatch(21, 0, 0b0001, LocalPads(PAD_BUTTON_X)));
  CHECK(scheduler.BeginNormalBatch(22, 0, 0b0001, LocalPads(PAD_BUTTON_Y)));

  const auto stalled =
      scheduler.BeginNormalBatch(23, 0, 0b0001, LocalPads(PAD_BUTTON_START));
  CHECK(stalled.status == Scheduler::BatchStatus::AwaitingRemoteInput);
  CHECK(stalled.outgoing_packet);
  const auto retained_packet = *stalled.outgoing_packet;

  CHECK(scheduler.SubmitRemotePacket(RemotePacket(104, PAD_BUTTON_B)) ==
        Scheduler::RemotePacketStatus::Accepted);
  const auto resumed =
      scheduler.BeginNormalBatch(23, 0, 0b0001, LocalPads(PAD_TRIGGER_Z));
  CHECK(resumed);
  CHECK(resumed.outgoing_packet);
  CHECK(
      resumed.outgoing_packet->batches[resumed.outgoing_packet->batch_count - 1]
          .pads[0]
          .button ==
      retained_packet.batches[retained_packet.batch_count - 1].pads[0].button);
  CHECK(
      resumed.outgoing_packet->batches[resumed.outgoing_packet->batch_count - 1]
          .pads[0]
          .button == PAD_BUTTON_START);
  return true;
}
} // namespace

int main() {
  if (!TestDelayedSamplingCorrectionAndReplay() ||
      !TestHardHorizonStallDoesNotResample())
    return 1;
  return 0;
}
