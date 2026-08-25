// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackSIInputJournal.h"
#include "Core/NetPlay/RollbackSIInputProtocol.h"

#include <array>
#include <cstdio>
#include <span>

namespace {
using Journal = NetPlay::RollbackSIInputJournal;
using CodecStatus = NetPlay::RollbackSIInputCodecStatus;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "rollback_si_input_journal_test:%d: %s\n",          \
                   __LINE__, #condition);                                      \
      return false;                                                            \
    }                                                                          \
  } while (false)

GCPadStatus Pad(const u16 buttons,
                const u8 stick_x = GCPadStatus::MAIN_STICK_CENTER_X) {
  GCPadStatus pad{};
  pad.button = buttons;
  pad.stickX = stick_x;
  return pad;
}

std::array<GCPadStatus, 4> Pads(const GCPadStatus &pad0 = {},
                                const GCPadStatus &pad1 = {},
                                const GCPadStatus &pad2 = {},
                                const GCPadStatus &pad3 = {}) {
  return {pad0, pad1, pad2, pad3};
}

NetPlay::RollbackSIInputBatch Batch(const u64 id, const u8 mask,
                                    const std::array<GCPadStatus, 4> &pads) {
  return {.batch_id = id, .pad_mask = mask, .pads = pads};
}

Journal::AppliedBatch Applied(const u64 id, const u64 frame,
                              const u32 ordinal = 0,
                              const u8 requested_pad_mask = 0b0011) {
  return {.batch_id = id,
          .emulated_frame = frame,
          .poll_ordinal = ordinal,
          .requested_pad_mask = requested_pad_mask};
}

Journal::Config Config() {
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

bool TestCodecRoundTripAndFailClosedParsing() {
  NetPlay::RollbackSIInputPacket packet;
  packet.session_generation = 0x1122334455667788ULL;
  packet.has_contiguous_ack = true;
  packet.contiguous_ack = 98;
  packet.batch_count = 2;
  packet.batches[0] =
      Batch(100, 0b0011, Pads(Pad(PAD_BUTTON_A, 141), Pad(PAD_BUTTON_B, 111)));
  packet.batches[0].pads[1].switches = SWITCH_COIN;
  packet.batches[1] = Batch(101, 0b0010, Pads({}, Pad(PAD_BUTTON_X, 109)));
  packet.batches[1].pads[1].isConnected = false;

  std::array<u8, NetPlay::ROLLBACK_SI_MAX_PACKET_SIZE> bytes{};
  const auto encoded = NetPlay::EncodeRollbackSIInputPacket(packet, bytes);
  CHECK(encoded);
  CHECK(encoded.size < bytes.size());

  const auto decoded = NetPlay::DecodeRollbackSIInputPacket(
      std::span<const u8>(bytes.data(), encoded.size),
      packet.session_generation);
  CHECK(decoded);
  CHECK(decoded.packet.protocol_version == NetPlay::ROLLBACK_SI_INPUT_VERSION);
  CHECK(decoded.packet.session_generation == packet.session_generation);
  CHECK(decoded.packet.has_contiguous_ack);
  CHECK(decoded.packet.contiguous_ack == 98);
  CHECK(decoded.packet.batch_count == 2);
  CHECK(decoded.packet.batches[0].batch_id == 100);
  CHECK(decoded.packet.batches[0].pad_mask == 0b0011);
  CHECK(decoded.packet.batches[0].pads[0].button == PAD_BUTTON_A);
  CHECK(decoded.packet.batches[0].pads[0].stickX == 141);
  CHECK(decoded.packet.batches[0].pads[1].switches == SWITCH_COIN);
  CHECK(decoded.packet.batches[1].pads[1].button == PAD_BUTTON_X);
  CHECK(!decoded.packet.batches[1].pads[1].isConnected);

  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(bytes.data(), encoded.size - 1),
            packet.session_generation)
            .status == CodecStatus::Truncated);
  bytes[encoded.size] = 0;
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(bytes.data(), encoded.size + 1),
            packet.session_generation)
            .status == CodecStatus::TrailingData);
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(bytes.data(), encoded.size),
            packet.session_generation + 1)
            .status == CodecStatus::WrongGeneration);

  auto bad_order = packet;
  bad_order.batches[1].batch_id = bad_order.batches[0].batch_id;
  CHECK(NetPlay::EncodeRollbackSIInputPacket(bad_order, bytes).status ==
        CodecStatus::NonIncreasingBatchIds);
  auto bad_mask = packet;
  bad_mask.batches[0].pad_mask = 0;
  CHECK(NetPlay::EncodeRollbackSIInputPacket(bad_mask, bytes).status ==
        CodecStatus::InvalidPadMask);
  return true;
}

bool TestVariablePollJournalAndReplayTrigger() {
  Journal journal(Config());
  CHECK(journal.GetConfigurationStatus() ==
        Journal::ConfigurationStatus::Valid);

  // Two polls in frame 100, no polls in frame 101, then one in frame 102.
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(10, 100, 0)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(11, 100, 1)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(12, 102, 0)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(12, 102, 0)) ==
        Journal::ObserveStatus::Duplicate);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(12, 103, 0)) ==
        Journal::ObserveStatus::ConflictingMetadata);

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
  CHECK(journal.ResolveBatch(11).inputs.pads[1].button == PAD_BUTTON_B);
  CHECK(journal.ResolveBatch(12).inputs.pads[1].button == PAD_BUTTON_B);

  CHECK(journal.SubmitInputBatch(
            1, 42, Journal::InputSource::Remote,
            Batch(11, 0b0010, Pads({}, Pad(PAD_BUTTON_Y)))) ==
        Journal::SubmitStatus::CorrectedPrediction);
  const auto first_trigger = journal.GetReplayTrigger();
  CHECK(first_trigger);
  CHECK(first_trigger->restore_before_emulated_frame == 100);
  CHECK(first_trigger->emulated_frame_request.first_incorrect_frame == 100);
  CHECK(first_trigger->emulated_frame_request.replay_through_frame == 102);
  CHECK(first_trigger->emulated_frame_request.generation ==
        first_trigger->timeline_request.generation);
  CHECK(first_trigger->first_replay_batch_id == 10);
  CHECK(first_trigger->first_incorrect_batch_id == 11);
  CHECK(first_trigger->replay_through_batch_id == 12);
  CHECK(first_trigger->replay_through_emulated_frame == 102);

  CHECK(journal.SubmitInputBatch(
            1, 42, Journal::InputSource::Remote,
            Batch(12, 0b0010, Pads({}, Pad(PAD_BUTTON_X)))) ==
        Journal::SubmitStatus::CorrectedPrediction);
  const auto current_trigger = journal.GetReplayTrigger();
  CHECK(current_trigger);
  CHECK(current_trigger->timeline_request.generation >
        first_trigger->timeline_request.generation);

  const auto corrected_11 = journal.ResolveBatch(11);
  const auto corrected_12 = journal.ResolveBatch(12);
  CHECK(corrected_11 && corrected_12);
  CHECK(corrected_11.inputs.pads[1].button == PAD_BUTTON_Y);
  CHECK(corrected_12.inputs.pads[1].button == PAD_BUTTON_X);
  CHECK(journal.GetConfirmedThroughBatch() == 12);
  return true;
}

bool TestAuthorityGenerationAndScheduleRejection() {
  Journal journal(Config());
  CHECK(journal.ObserveAppliedBatch(2, 42, Applied(10, 1)) ==
        Journal::ObserveStatus::UnsupportedVersion);
  CHECK(journal.ObserveAppliedBatch(1, 43, Applied(10, 1)) ==
        Journal::ObserveStatus::WrongGeneration);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(11, 1)) ==
        Journal::ObserveStatus::NonSequentialBatch);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(10, 1, 1)) ==
        Journal::ObserveStatus::InvalidPollOrder);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(10, 1)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(11, 1, 2)) ==
        Journal::ObserveStatus::InvalidPollOrder);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(11, 2, 0, 0b0001)) ==
        Journal::ObserveStatus::UnsupportedPadSchedule);

  const auto remote_on_local = Batch(10, 0b0001, Pads(Pad(PAD_BUTTON_B)));
  CHECK(journal.SubmitInputBatch(1, 42, Journal::InputSource::Remote,
                                 remote_on_local) ==
        Journal::SubmitStatus::WrongAuthority);
  CHECK(journal.SubmitInputBatch(1, 41, Journal::InputSource::Local,
                                 remote_on_local) ==
        Journal::SubmitStatus::WrongGeneration);

  const auto actual = Batch(10, 0b0001, Pads(Pad(PAD_BUTTON_A)));
  CHECK(journal.SubmitInputBatch(1, 42, Journal::InputSource::Local, actual) ==
        Journal::SubmitStatus::Accepted);
  CHECK(journal.SubmitInputBatch(1, 42, Journal::InputSource::Local, actual) ==
        Journal::SubmitStatus::Duplicate);
  CHECK(journal.SubmitInputBatch(1, 42, Journal::InputSource::Local,
                                 Batch(10, 0b0001, Pads(Pad(PAD_BUTTON_X)))) ==
        Journal::SubmitStatus::ConflictingActual);

  CHECK(journal.ResolveBatch(9).status ==
        Journal::Timeline::ResolveStatus::TooOld);
  CHECK(journal.ResolveBatch(11).status ==
        Journal::Timeline::ResolveStatus::TooFarAhead);
  return true;
}

bool TestPrunedConfirmedPollRemainsReplayableWithItsFrame() {
  auto config = Config();
  config.history_capacity = 3;
  config.max_polls_per_frame = 2;
  Journal journal(config);

  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(10, 100)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(11, 100, 1)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(12, 101)) ==
        Journal::ObserveStatus::Accepted);
  CHECK(journal.ObserveAppliedBatch(1, 42, Applied(13, 102)) ==
        Journal::ObserveStatus::Accepted);

  for (u64 id = 10; id <= 13; ++id) {
    CHECK(
        journal.SubmitInputBatch(1, 42, Journal::InputSource::Local,
                                 Batch(id, 0b0001, Pads(Pad(PAD_BUTTON_A)))) ==
        Journal::SubmitStatus::Accepted);
    if (id == 10) {
      CHECK(journal.SubmitInputBatch(
                1, 42, Journal::InputSource::Remote,
                Batch(id, 0b0010, Pads({}, Pad(PAD_BUTTON_B)))) ==
            Journal::SubmitStatus::Accepted);
    }
    CHECK(journal.ResolveBatch(id));
  }

  // Creating batch 13 forced the timeline's confirmed batch 10 out of its
  // three-slot ring. The SI journal must nevertheless retain poll 0 because
  // unconfirmed poll 1 belongs to the same emulated frame.
  CHECK(journal.SubmitInputBatch(
            1, 42, Journal::InputSource::Remote,
            Batch(11, 0b0010, Pads({}, Pad(PAD_BUTTON_Y)))) ==
        Journal::SubmitStatus::CorrectedPrediction);
  const auto trigger = journal.GetReplayTrigger();
  CHECK(trigger);
  CHECK(trigger->first_replay_batch_id == 10);
  const auto replayed_first_poll = journal.ResolveBatch(10);
  CHECK(replayed_first_poll);
  CHECK(replayed_first_poll.inputs.pads[1].button == PAD_BUTTON_B);
  return true;
}

bool TestInvalidConfiguration() {
  auto config = Config();
  config.session_generation = 0;
  Journal no_generation(config);
  CHECK(no_generation.GetConfigurationStatus() ==
        Journal::ConfigurationStatus::InvalidGeneration);

  config = Config();
  config.history_capacity = 0;
  Journal no_history(config);
  CHECK(no_history.GetConfigurationStatus() ==
        Journal::ConfigurationStatus::InvalidTimelineConfiguration);
  CHECK(no_history.ObserveAppliedBatch(1, 42, Applied(10, 0)) ==
        Journal::ObserveStatus::InvalidConfiguration);

  config = Config();
  config.max_polls_per_frame = 0;
  Journal no_poll_bound(config);
  CHECK(no_poll_bound.GetConfigurationStatus() ==
        Journal::ConfigurationStatus::InvalidPollCapacity);
  return true;
}
} // namespace

int main() {
  if (!TestCodecRoundTripAndFailClosedParsing() ||
      !TestVariablePollJournalAndReplayTrigger() ||
      !TestAuthorityGenerationAndScheduleRejection() ||
      !TestPrunedConfirmedPollRemainsReplayableWithItsFrame() ||
      !TestInvalidConfiguration()) {
    return 1;
  }
  return 0;
}
