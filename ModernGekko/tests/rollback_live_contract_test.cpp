// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Executable contract for the live rollback integration.  This test
// deliberately depends only on the already-landed codec and SI journal, so it
// can run while the NetPlayClient/NetPlayServer seams are being connected.  The
// small negotiation function below is a test oracle, not shipped session code.

#include "Core/NetPlay/RollbackSIInputJournal.h"
#include "Core/NetPlay/RollbackSIInputProtocol.h"

#include <array>
#include <cstdio>
#include <span>
#include <string_view>

namespace {
using CodecStatus = NetPlay::RollbackSIInputCodecStatus;
using Journal = NetPlay::RollbackSIInputJournal;
using Timeline = NetPlay::RollbackInputTimeline;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "rollback_live_contract_test:%d: %s\n", __LINE__,   \
                   #condition);                                                \
      return false;                                                            \
    }                                                                          \
  } while (false)

constexpr u32 CAP_ROLLBACK_INPUT = 1 << 0;
constexpr u32 CAP_FULL_STATE_RESTORE = 1 << 1;

struct PeerCapabilities {
  u16 protocol_version = NetPlay::ROLLBACK_SI_INPUT_VERSION;
  u32 feature_bits = CAP_ROLLBACK_INPUT | CAP_FULL_STATE_RESTORE;
  u32 max_prediction_batches = 8;
  u32 max_polls_per_frame = 8;
  std::string_view compatibility_fingerprint = "same-build-and-game";
  std::string_view state_format = "dolphin-full-v1";
  bool requests_rollback = true;
};

enum class NegotiationResult {
  Rollback,
  FixedDelay,
  RejectCompatibility,
  RejectRollbackContract,
};

NegotiationResult Negotiate(const PeerCapabilities &host,
                            const PeerCapabilities &guest) {
  if (host.compatibility_fingerprint != guest.compatibility_fingerprint)
    return NegotiationResult::RejectCompatibility;
  if (!host.requests_rollback || !guest.requests_rollback)
    return NegotiationResult::FixedDelay;

  constexpr u32 required = CAP_ROLLBACK_INPUT | CAP_FULL_STATE_RESTORE;
  if (host.protocol_version != guest.protocol_version ||
      (host.feature_bits & required) != required ||
      (guest.feature_bits & required) != required ||
      host.state_format != guest.state_format ||
      host.max_prediction_batches == 0 || guest.max_prediction_batches == 0 ||
      host.max_polls_per_frame == 0 || guest.max_polls_per_frame == 0) {
    return NegotiationResult::RejectRollbackContract;
  }
  return NegotiationResult::Rollback;
}

GCPadStatus Pad(const u16 buttons) {
  GCPadStatus pad{};
  pad.button = buttons;
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
                              const u32 ordinal) {
  return {.batch_id = id,
          .emulated_frame = frame,
          .poll_ordinal = ordinal,
          .requested_pad_mask = 0b0011};
}

Journal::Config Config(const bool first_peer, const u64 first_batch = 20,
                       const u64 prediction_horizon = 3) {
  return {.session_generation = 0x1234,
          .first_batch_id = first_batch,
          .history_capacity = 16,
          .max_prediction_batches = prediction_horizon,
          .max_polls_per_frame = 4,
          .pad_authority = first_peer
                               ? std::array{Timeline::PadAuthority::Local,
                                            Timeline::PadAuthority::Remote,
                                            Timeline::PadAuthority::Inactive,
                                            Timeline::PadAuthority::Inactive}
                               : std::array{Timeline::PadAuthority::Remote,
                                            Timeline::PadAuthority::Local,
                                            Timeline::PadAuthority::Inactive,
                                            Timeline::PadAuthority::Inactive}};
}

bool SameInputs(const Journal::ResolveResult &lhs,
                const Journal::ResolveResult &rhs) {
  return lhs && rhs && lhs.inputs.pads[0].button == rhs.inputs.pads[0].button &&
         lhs.inputs.pads[1].button == rhs.inputs.pads[1].button;
}

Journal::SubmitStatus Deliver(Journal &receiver,
                              const Journal::InputSource source,
                              const NetPlay::RollbackSIInputBatch &batch) {
  NetPlay::RollbackSIInputPacket packet;
  packet.session_generation = 0x1234;
  packet.batch_count = 1;
  packet.batches[0] = batch;
  std::array<u8, NetPlay::ROLLBACK_SI_MAX_PACKET_SIZE> bytes{};
  const auto encoded = NetPlay::EncodeRollbackSIInputPacket(packet, bytes);
  if (!encoded)
    return Journal::SubmitStatus::PartialFailure;
  const auto decoded = NetPlay::DecodeRollbackSIInputPacket(
      std::span<const u8>(bytes.data(), encoded.size), 0x1234);
  if (!decoded)
    return Journal::SubmitStatus::PartialFailure;
  return receiver.SubmitInputBatch(NetPlay::ROLLBACK_SI_INPUT_VERSION, 0x1234,
                                   source, decoded.packet.batches[0]);
}

bool TestCapabilityNegotiationContract() {
  const PeerCapabilities host;
  PeerCapabilities guest;
  CHECK(Negotiate(host, guest) == NegotiationResult::Rollback);

  guest.requests_rollback = false;
  CHECK(Negotiate(host, guest) == NegotiationResult::FixedDelay);
  guest = {};
  guest.feature_bits &= ~CAP_FULL_STATE_RESTORE;
  CHECK(Negotiate(host, guest) == NegotiationResult::RejectRollbackContract);
  guest = {};
  guest.protocol_version += 1;
  CHECK(Negotiate(host, guest) == NegotiationResult::RejectRollbackContract);
  guest = {};
  guest.max_prediction_batches = 0;
  CHECK(Negotiate(host, guest) == NegotiationResult::RejectRollbackContract);
  guest = {};
  guest.max_polls_per_frame = 0;
  CHECK(Negotiate(host, guest) == NegotiationResult::RejectRollbackContract);
  guest = {};
  guest.state_format = "incompatible-state";
  CHECK(Negotiate(host, guest) == NegotiationResult::RejectRollbackContract);
  guest = {};
  guest.compatibility_fingerprint = "other-build";
  CHECK(Negotiate(host, guest) == NegotiationResult::RejectCompatibility);
  return true;
}

bool TestTwoPeerLateAuthoritativeCorrectionAndVariablePolling() {
  Journal first(Config(true));
  Journal second(Config(false));
  const std::array schedule = {Applied(20, 500, 0), Applied(21, 500, 1),
                               Applied(22, 502, 0)};
  for (const auto &applied : schedule) {
    CHECK(first.ObserveAppliedBatch(1, 0x1234, applied) ==
          Journal::ObserveStatus::Accepted);
    CHECK(second.ObserveAppliedBatch(1, 0x1234, applied) ==
          Journal::ObserveStatus::Accepted);
  }

  const auto first_20 = Batch(20, 0b0001, Pads(Pad(PAD_BUTTON_A)));
  const auto second_20 = Batch(20, 0b0010, Pads({}, Pad(PAD_BUTTON_B)));
  CHECK(first.SubmitInputBatch(1, 0x1234, Journal::InputSource::Local,
                               first_20) == Journal::SubmitStatus::Accepted);
  CHECK(second.SubmitInputBatch(1, 0x1234, Journal::InputSource::Local,
                                second_20) == Journal::SubmitStatus::Accepted);
  CHECK(Deliver(first, Journal::InputSource::Remote, second_20) ==
        Journal::SubmitStatus::Accepted);
  CHECK(Deliver(second, Journal::InputSource::Remote, first_20) ==
        Journal::SubmitStatus::Accepted);
  CHECK(SameInputs(first.ResolveBatch(20), second.ResolveBatch(20)));

  // The second peer changes input during the second poll of frame 500.  Hold
  // both authoritative packets until each side has simulated through a later
  // frame using repeat-last for its remote pad.
  const auto first_21 = Batch(21, 0b0001, Pads(Pad(PAD_BUTTON_A)));
  const auto second_21 = Batch(21, 0b0010, Pads({}, Pad(PAD_BUTTON_X)));
  const auto first_22 = Batch(22, 0b0001, Pads(Pad(PAD_BUTTON_A)));
  const auto second_22 = Batch(22, 0b0010, Pads({}, Pad(PAD_BUTTON_X)));
  CHECK(first.SubmitInputBatch(1, 0x1234, Journal::InputSource::Local,
                               first_21) == Journal::SubmitStatus::Accepted);
  CHECK(second.SubmitInputBatch(1, 0x1234, Journal::InputSource::Local,
                                second_21) == Journal::SubmitStatus::Accepted);
  const auto speculative_first_21 = first.ResolveBatch(21);
  const auto speculative_second_21 = second.ResolveBatch(21);
  CHECK(speculative_first_21 && speculative_second_21);
  CHECK(speculative_first_21.inputs.pads[1].button == PAD_BUTTON_B);
  CHECK(speculative_second_21.inputs.pads[1].button == PAD_BUTTON_X);
  CHECK(!SameInputs(speculative_first_21, speculative_second_21));

  CHECK(first.SubmitInputBatch(1, 0x1234, Journal::InputSource::Local,
                               first_22) == Journal::SubmitStatus::Accepted);
  CHECK(second.SubmitInputBatch(1, 0x1234, Journal::InputSource::Local,
                                second_22) == Journal::SubmitStatus::Accepted);
  CHECK(first.ResolveBatch(22));
  CHECK(second.ResolveBatch(22));

  CHECK(Deliver(first, Journal::InputSource::Remote, second_21) ==
        Journal::SubmitStatus::CorrectedPrediction);
  CHECK(Deliver(second, Journal::InputSource::Remote, first_21) ==
        Journal::SubmitStatus::Accepted);
  CHECK(Deliver(first, Journal::InputSource::Remote, second_22) ==
        Journal::SubmitStatus::CorrectedPrediction);
  CHECK(Deliver(second, Journal::InputSource::Remote, first_22) ==
        Journal::SubmitStatus::Accepted);

  const auto trigger = first.GetReplayTrigger();
  CHECK(trigger);
  CHECK(trigger->restore_before_emulated_frame == 500);
  CHECK(trigger->first_replay_batch_id == 20);
  CHECK(trigger->first_incorrect_batch_id == 21);
  CHECK(trigger->replay_through_batch_id == 22);
  CHECK(trigger->replay_through_emulated_frame == 502);
  CHECK(SameInputs(first.ResolveBatch(21), second.ResolveBatch(21)));
  CHECK(SameInputs(first.ResolveBatch(22), second.ResolveBatch(22)));
  return true;
}

bool TestPredictionHorizonForcesFallback() {
  Journal journal(Config(true, 40, 2));
  const std::array schedule = {Applied(40, 700, 0), Applied(41, 700, 1),
                               Applied(42, 703, 0), Applied(43, 704, 0)};
  for (const auto &applied : schedule) {
    CHECK(journal.ObserveAppliedBatch(1, 0x1234, applied) ==
          Journal::ObserveStatus::Accepted);
    CHECK(journal.SubmitInputBatch(
              1, 0x1234, Journal::InputSource::Local,
              Batch(applied.batch_id, 0b0001, Pads(Pad(PAD_BUTTON_A)))) ==
          Journal::SubmitStatus::Accepted);
  }
  CHECK(journal.SubmitInputBatch(
            1, 0x1234, Journal::InputSource::Remote,
            Batch(40, 0b0010, Pads({}, Pad(PAD_BUTTON_B)))) ==
        Journal::SubmitStatus::Accepted);
  CHECK(journal.ResolveBatch(40));
  CHECK(journal.ResolveBatch(41));
  CHECK(journal.ResolveBatch(42));
  CHECK(journal.ResolveBatch(43).status ==
        Timeline::ResolveStatus::PredictionHorizonExceeded);
  return true;
}

bool TestMalformedAndEveryTruncatedPacketFailClosed() {
  NetPlay::RollbackSIInputPacket maximum;
  maximum.session_generation = 0x1234;
  maximum.has_contiguous_ack = true;
  maximum.contiguous_ack = 99;
  maximum.batch_count = NetPlay::ROLLBACK_SI_MAX_BATCHES_PER_PACKET;
  for (std::size_t i = 0; i < maximum.batch_count; ++i)
    maximum.batches[i] =
        Batch(100 + i, 0b1111, Pads(Pad(1), Pad(2), Pad(4), Pad(8)));

  std::array<u8, NetPlay::ROLLBACK_SI_MAX_PACKET_SIZE + 1> bytes{};
  const auto encoded = NetPlay::EncodeRollbackSIInputPacket(maximum, bytes);
  CHECK(encoded);
  CHECK(encoded.size == NetPlay::ROLLBACK_SI_MAX_PACKET_SIZE);
  for (std::size_t length = 0; length < encoded.size; ++length) {
    CHECK(!NetPlay::DecodeRollbackSIInputPacket(
        std::span<const u8>(bytes.data(), length), 0x1234));
  }
  bytes[encoded.size] = 0;
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(bytes.data(), encoded.size + 1), 0x1234)
            .status == CodecStatus::TrailingData);

  NetPlay::RollbackSIInputPacket one;
  one.session_generation = 0x1234;
  one.batch_count = 1;
  one.batches[0] = Batch(1, 0b0001, Pads(Pad(PAD_BUTTON_A)));
  std::array<u8, NetPlay::ROLLBACK_SI_MAX_PACKET_SIZE> original{};
  const auto one_encoded = NetPlay::EncodeRollbackSIInputPacket(one, original);
  CHECK(one_encoded);
  auto corrupt = original;
  corrupt[0] ^= 1;
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(corrupt.data(), one_encoded.size), 0x1234)
            .status == CodecStatus::BadMagic);
  corrupt = original;
  corrupt[5] = 2;
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(corrupt.data(), one_encoded.size), 0x1234)
            .status == CodecStatus::UnsupportedVersion);
  corrupt = original;
  corrupt[7] = 2;
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(corrupt.data(), one_encoded.size), 0x1234)
            .status == CodecStatus::InvalidFlags);
  corrupt = original;
  for (std::size_t i = 8; i < 16; ++i)
    corrupt[i] = 0;
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(corrupt.data(), one_encoded.size), 0)
            .status == CodecStatus::InvalidGeneration);
  corrupt = original;
  corrupt[24] = 0;
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(corrupt.data(), one_encoded.size), 0x1234)
            .status == CodecStatus::InvalidBatchCount);
  corrupt = original;
  corrupt[33] = 0;
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(corrupt.data(), one_encoded.size), 0x1234)
            .status == CodecStatus::InvalidPadMask);
  corrupt = original;
  corrupt[45] = 2;
  CHECK(NetPlay::DecodeRollbackSIInputPacket(
            std::span<const u8>(corrupt.data(), one_encoded.size), 0x1234)
            .status == CodecStatus::InvalidConnectedFlag);

  CHECK(NetPlay::EncodeRollbackSIInputPacket(
            one, std::span<u8>(original.data(), one_encoded.size - 1))
            .status == CodecStatus::OutputTooSmall);
  return true;
}
} // namespace

int main() {
  if (!TestCapabilityNegotiationContract() ||
      !TestTwoPeerLateAuthoritativeCorrectionAndVariablePolling() ||
      !TestPredictionHorizonForcesFallback() ||
      !TestMalformedAndEveryTruncatedPacketFailClosed()) {
    return 1;
  }
  return 0;
}
