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
                                            const u16 button,
                                            const u8 pad_mask = 0b0010) {
  NetPlay::RollbackSIInputPacket packet{.session_generation = 77,
                                        .batch_count = 1};
  packet.batches[0].batch_id = batch_id;
  packet.batches[0].pad_mask = pad_mask;
  for (std::size_t pad = 0; pad < packet.batches[0].pads.size(); ++pad) {
    if ((pad_mask & (u8{1} << pad)) != 0)
      packet.batches[0].pads[pad] = Pad(button);
  }
  return packet;
}

NetPlay::RollbackSIInputPacket AcknowledgingRemotePacket(const u64 batch_id,
                                                         const u8 pad_mask,
                                                         const u64 ack) {
  auto packet = RemotePacket(batch_id, 0, pad_mask);
  packet.has_contiguous_ack = true;
  packet.contiguous_ack = ack;
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
  CHECK(!replay_first.outgoing_packet);
  CHECK(replay_first.inputs.pads[0].button == PAD_BUTTON_A);
  CHECK(replay_first.inputs.pads[1].button == PAD_BUTTON_B);
  CHECK(replay_first.inputs.predicted_pad_mask == 0);

  const auto replay_later = scheduler.BeginReplayBatch(11, 1);
  CHECK(replay_later);
  CHECK(!replay_later.outgoing_packet);
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

bool TestAcknowledgedGapRepairIsBoundedAndAllPeerSafe() {
  auto config = Config();
  config.max_prediction_batches = 8;
  config.pad_authority[2] = Timeline::PadAuthority::Remote;
  Scheduler scheduler(config);
  CHECK(scheduler.GetConfigurationStatus() ==
        Scheduler::ConfigurationStatus::Valid);

  Scheduler::BatchResult outgoing;
  for (u32 ordinal = 0; ordinal < 4; ++ordinal) {
    outgoing = scheduler.BeginNormalBatch(30, ordinal, 0b0001,
                                          LocalPads(PAD_BUTTON_A));
    CHECK(outgoing);
  }
  CHECK(outgoing.outgoing_packet);
  CHECK(outgoing.outgoing_packet->batch_count == 3);
  // Batch 102 has fallen outside the ordinary recent-three tail, but remains
  // in the repair slot because no remote stream has acknowledged it.
  CHECK(outgoing.outgoing_packet->batches[0].batch_id == 102);
  CHECK(outgoing.outgoing_packet->batches[1].batch_id == 104);
  CHECK(outgoing.outgoing_packet->batches[2].batch_id == 105);

  CHECK(scheduler.SubmitRemotePacket(
            AcknowledgingRemotePacket(102, 0b0010, 102)) ==
        Scheduler::RemotePacketStatus::Accepted);
  outgoing = scheduler.BeginNormalBatch(31, 0, 0b0001,
                                        LocalPads(PAD_BUTTON_B));
  CHECK(outgoing);
  CHECK(outgoing.outgoing_packet);
  // Pad two has not acknowledged yet, so one peer cannot retire data on behalf
  // of another peer.
  CHECK(outgoing.outgoing_packet->batches[0].batch_id == 102);

  CHECK(scheduler.SubmitRemotePacket(
            AcknowledgingRemotePacket(102, 0b0100, 102)) ==
        Scheduler::RemotePacketStatus::Accepted);
  outgoing = scheduler.BeginNormalBatch(31, 1, 0b0001,
                                        LocalPads(PAD_BUTTON_X));
  CHECK(outgoing);
  CHECK(outgoing.outgoing_packet);
  CHECK(outgoing.outgoing_packet->batches[0].batch_id == 103);

  // A peer cannot acknowledge input that this scheduler has not produced. The
  // rejection happens before the packet's authoritative input is submitted.
  CHECK(scheduler.SubmitRemotePacket(
            AcknowledgingRemotePacket(103, 0b0010, 108)) ==
        Scheduler::RemotePacketStatus::InvalidAcknowledgement);
  CHECK(scheduler.SubmitRemotePacket(RemotePacket(103, 0, 0b0010)) ==
        Scheduler::RemotePacketStatus::Accepted);
  CHECK(scheduler.SubmitRemotePacket(
            AcknowledgingRemotePacket(103, 0b0010, 101)) ==
        Scheduler::RemotePacketStatus::DuplicateOrRetired);

  auto invalid_redundancy = config;
  invalid_redundancy.redundant_batch_count = 1;
  CHECK(Scheduler(invalid_redundancy).GetConfigurationStatus() ==
        Scheduler::ConfigurationStatus::InvalidRedundancy);
  return true;
}

bool TestDelayedAcknowledgementOutlivesRollbackHistory() {
  auto config = Config();
  config.history_capacity = 16;
  Scheduler scheduler(config);
  CHECK(scheduler.GetConfigurationStatus() ==
        Scheduler::ConfigurationStatus::Valid);

  Scheduler::BatchResult outgoing;
  for (u32 offset = 0; offset < 64; ++offset) {
    const u64 consumed_batch = 100 + offset;
    if (consumed_batch >= 102) {
      CHECK(scheduler.SubmitRemotePacket(RemotePacket(consumed_batch, 0)) ==
            Scheduler::RemotePacketStatus::Accepted);
    }
    outgoing = scheduler.BeginNormalBatch(40 + offset / 4, offset % 4,
                                          0b0001,
                                          LocalPads(PAD_BUTTON_A));
    CHECK(outgoing);
  }

  CHECK(outgoing.outgoing_packet);
  CHECK(outgoing.outgoing_packet->batches[0].batch_id == 102);
  CHECK(outgoing.outgoing_packet->batches[outgoing.outgoing_packet->batch_count - 1]
            .batch_id == 165);

  // Sixty-four unacknowledged batches exceed both the two-batch prediction
  // horizon and the 16-entry rollback journal budget. A late ACK can still
  // retire them because retransmit storage has its own protocol-bounded window.
  CHECK(scheduler.SubmitRemotePacket(
            AcknowledgingRemotePacket(164, 0b0010, 165)) ==
        Scheduler::RemotePacketStatus::Accepted);
  outgoing = scheduler.BeginNormalBatch(56, 0, 0b0001,
                                        LocalPads(PAD_BUTTON_B));
  CHECK(outgoing);
  CHECK(outgoing.outgoing_packet);
  CHECK(outgoing.outgoing_packet->batch_count == 1);
  CHECK(outgoing.outgoing_packet->batches[0].batch_id == 166);

  auto invalid_ack_history = config;
  invalid_ack_history.unacknowledged_history_capacity = 2;
  CHECK(Scheduler(invalid_ack_history).GetConfigurationStatus() ==
        Scheduler::ConfigurationStatus::InvalidAcknowledgementHistory);
  invalid_ack_history.unacknowledged_history_capacity =
      NetPlay::ROLLBACK_SI_MAX_UNACKNOWLEDGED_BATCHES + 1;
  CHECK(Scheduler(invalid_ack_history).GetConfigurationStatus() ==
        Scheduler::ConfigurationStatus::InvalidAcknowledgementHistory);
  return true;
}
} // namespace

int main() {
  if (!TestDelayedSamplingCorrectionAndReplay() ||
      !TestHardHorizonStallDoesNotResample() ||
      !TestAcknowledgedGapRepairIsBoundedAndAllPeerSafe() ||
      !TestDelayedAcknowledgementOutlivesRollbackHistory())
    return 1;
  return 0;
}
