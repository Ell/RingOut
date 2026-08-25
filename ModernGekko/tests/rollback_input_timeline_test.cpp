// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackInputTimeline.h"

#include <array>
#include <cstdio>
#include <limits>

namespace {
using Timeline = NetPlay::RollbackInputTimeline;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "rollback_input_timeline_test:%d: %s\n", __LINE__,  \
                   #condition);                                                \
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

Timeline::Config TwoRemotePadsConfig(const std::size_t capacity = 8,
                                     const u64 prediction_frames = 4) {
  return {.pad_authority = {Timeline::PadAuthority::Local,
                            Timeline::PadAuthority::Remote,
                            Timeline::PadAuthority::Remote,
                            Timeline::PadAuthority::Inactive},
          .history_capacity = capacity,
          .max_prediction_frames = prediction_frames,
          .first_frame = 100};
}

bool TestPredictionConfirmationAndMultiPadLayout() {
  Timeline timeline(TwoRemotePadsConfig());
  const GCPadStatus local = Pad(PAD_BUTTON_A, 140);
  const GCPadStatus remote_two = Pad(PAD_BUTTON_X, 110);

  CHECK(timeline.SubmitLocalInput(100, 0, local) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.SubmitRemoteInput(100, 2, remote_two) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(!timeline.GetConfirmedThrough());

  const Timeline::ResolveResult predicted = timeline.ResolveFrame(100);
  CHECK(predicted);
  CHECK(predicted.frame.active_pad_mask == 0b0111);
  CHECK(predicted.frame.predicted_pad_mask == 0b0010);
  CHECK(predicted.frame.pads[0].button == PAD_BUTTON_A);
  CHECK(predicted.frame.pads[1].button == 0);
  CHECK(predicted.frame.pads[1].stickX == GCPadStatus::MAIN_STICK_CENTER_X);
  CHECK(predicted.frame.pads[2].button == PAD_BUTTON_X);
  CHECK(predicted.frame.pads[3].button == 0);

  CHECK(timeline.SubmitRemoteInput(100, 1, GCPadStatus{}) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.GetConfirmedThrough() == 100);
  CHECK(!timeline.GetPendingRollback());
  CHECK(timeline.SubmitRemoteInput(100, 1, GCPadStatus{}) ==
        Timeline::SubmitStatus::Duplicate);
  CHECK(timeline.SubmitRemoteInput(100, 1, Pad(PAD_BUTTON_B)) ==
        Timeline::SubmitStatus::ConflictingActual);
  return true;
}

bool TestLateCorrectionAndRaceSafeAcknowledgement() {
  Timeline timeline(TwoRemotePadsConfig());
  CHECK(timeline.SubmitLocalInput(100, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.SubmitRemoteInput(100, 1, Pad(PAD_BUTTON_B)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.SubmitRemoteInput(100, 2, Pad(PAD_BUTTON_X)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.GetConfirmedThrough() == 100);

  CHECK(timeline.SubmitLocalInput(101, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::Accepted);
  const Timeline::ResolveResult first_prediction = timeline.ResolveFrame(101);
  CHECK(first_prediction);
  CHECK(first_prediction.frame.predicted_pad_mask == 0b0110);
  CHECK(first_prediction.frame.pads[1].button == PAD_BUTTON_B);
  CHECK(timeline.SubmitLocalInput(102, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::Accepted);
  const Timeline::ResolveResult second_prediction = timeline.ResolveFrame(102);
  CHECK(second_prediction);
  CHECK(second_prediction.frame.pads[1].button == PAD_BUTTON_B);

  CHECK(timeline.SubmitRemoteInput(101, 1, Pad(PAD_BUTTON_Y)) ==
        Timeline::SubmitStatus::CorrectedPrediction);
  const auto first_request = timeline.GetPendingRollback();
  CHECK(first_request);
  CHECK(first_request->first_incorrect_frame == 101);
  CHECK(first_request->replay_through_frame == 102);

  // A second correction invalidates the request already handed to a replay
  // worker; acknowledging that stale generation must not lose the new one.
  CHECK(timeline.SubmitRemoteInput(101, 2, Pad(PAD_BUTTON_START)) ==
        Timeline::SubmitStatus::CorrectedPrediction);
  CHECK(!timeline.AcknowledgeRollback(*first_request));
  const auto current_request = timeline.GetPendingRollback();
  CHECK(current_request);
  CHECK(current_request->generation > first_request->generation);

  const Timeline::ResolveResult corrected = timeline.ResolveFrame(101);
  CHECK(corrected);
  CHECK(corrected.frame.predicted_pad_mask == 0);
  CHECK(corrected.frame.pads[1].button == PAD_BUTTON_Y);
  CHECK(corrected.frame.pads[2].button == PAD_BUTTON_START);
  const Timeline::ResolveResult corrected_successor =
      timeline.ResolveFrame(102);
  CHECK(corrected_successor);
  CHECK(corrected_successor.frame.predicted_pad_mask == 0b0110);
  CHECK(corrected_successor.frame.pads[1].button == PAD_BUTTON_Y);
  CHECK(corrected_successor.frame.pads[2].button == PAD_BUTTON_START);
  CHECK(timeline.GetConfirmedThrough() == 101);
  CHECK(timeline.AcknowledgeRollback(*current_request));
  CHECK(!timeline.GetPendingRollback());
  return true;
}

bool TestPredictionHorizonAndLocalInputs() {
  Timeline timeline(TwoRemotePadsConfig(8, 2));
  CHECK(timeline.ResolveFrame(100).status ==
        Timeline::ResolveStatus::MissingLocalInput);
  CHECK(timeline.SubmitLocalInput(100, 1, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::WrongAuthority);
  CHECK(timeline.SubmitRemoteInput(100, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::WrongAuthority);
  CHECK(timeline.SubmitLocalInput(100, 4, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::InvalidPad);

  for (u64 frame = 100; frame <= 102; ++frame) {
    CHECK(timeline.SubmitLocalInput(frame, 0, Pad(PAD_BUTTON_A)) ==
          Timeline::SubmitStatus::Accepted);
  }
  CHECK(timeline.SubmitRemoteInput(100, 1, Pad(PAD_BUTTON_B)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.SubmitRemoteInput(100, 2, Pad(PAD_BUTTON_X)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.ResolveFrame(101));
  CHECK(timeline.ResolveFrame(102));

  CHECK(timeline.SubmitLocalInput(103, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.ResolveFrame(103).status ==
        Timeline::ResolveStatus::PredictionHorizonExceeded);
  return true;
}

bool TestOutOfOrderConfirmationAndBoundedPruning() {
  Timeline::Config config{.pad_authority = {Timeline::PadAuthority::Local},
                          .history_capacity = 3,
                          .max_prediction_frames = 0,
                          .first_frame = 10};
  Timeline timeline(config);

  CHECK(timeline.SubmitLocalInput(11, 0, Pad(PAD_BUTTON_B)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(!timeline.GetConfirmedThrough());
  CHECK(timeline.SubmitLocalInput(10, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.GetConfirmedThrough() == 11);
  CHECK(timeline.ResolveFrame(10));
  CHECK(timeline.ResolveFrame(11));

  CHECK(timeline.SubmitLocalInput(12, 0, Pad(PAD_BUTTON_X)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.ResolveFrame(12));
  CHECK(timeline.GetStoredFrameCount() == 3);
  CHECK(timeline.SubmitLocalInput(13, 0, Pad(PAD_BUTTON_Y)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.GetStoredFrameCount() == 3);
  CHECK(timeline.GetFirstRetainedFrame() == 11);
  CHECK(timeline.SubmitLocalInput(10, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::TooOld);
  CHECK(timeline.SubmitLocalInput(17, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::TooFarAhead);
  return true;
}

bool TestSubmissionOrderDoesNotChangeResolvedPads() {
  Timeline first(TwoRemotePadsConfig());
  Timeline second(TwoRemotePadsConfig());
  const std::array<GCPadStatus, 3> pads = {
      Pad(PAD_BUTTON_A, 130), Pad(PAD_BUTTON_B, 120), Pad(PAD_BUTTON_X, 110)};

  CHECK(first.SubmitLocalInput(100, 0, pads[0]) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(first.SubmitRemoteInput(100, 1, pads[1]) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(first.SubmitRemoteInput(100, 2, pads[2]) ==
        Timeline::SubmitStatus::Accepted);

  CHECK(second.SubmitRemoteInput(100, 2, pads[2]) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(second.SubmitRemoteInput(100, 1, pads[1]) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(second.SubmitLocalInput(100, 0, pads[0]) ==
        Timeline::SubmitStatus::Accepted);

  const auto first_result = first.ResolveFrame(100);
  const auto second_result = second.ResolveFrame(100);
  CHECK(first_result && second_result);
  for (std::size_t pad = 0; pad < Timeline::PAD_COUNT; ++pad) {
    CHECK(first_result.frame.pads[pad].button ==
          second_result.frame.pads[pad].button);
    CHECK(first_result.frame.pads[pad].stickX ==
          second_result.frame.pads[pad].stickX);
  }
  CHECK(first_result.frame.predicted_pad_mask ==
        second_result.frame.predicted_pad_mask);
  return true;
}

bool TestConfirmedButUnconsumedInputIsNotPruned() {
  Timeline timeline({.pad_authority = {Timeline::PadAuthority::Local},
                     .history_capacity = 2,
                     .first_frame = 20});
  CHECK(timeline.SubmitLocalInput(20, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.SubmitLocalInput(21, 0, Pad(PAD_BUTTON_B)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.GetConfirmedThrough() == 21);
  CHECK(timeline.SubmitLocalInput(22, 0, Pad(PAD_BUTTON_X)) ==
        Timeline::SubmitStatus::HistoryFull);

  CHECK(timeline.ResolveFrame(20));
  CHECK(timeline.SubmitLocalInput(22, 0, Pad(PAD_BUTTON_X)) ==
        Timeline::SubmitStatus::Accepted);
  CHECK(timeline.GetFirstRetainedFrame() == 21);
  return true;
}

bool TestInvalidConfiguration() {
  Timeline zero_capacity({.pad_authority = {Timeline::PadAuthority::Local},
                          .history_capacity = 0});
  CHECK(zero_capacity.GetConfigurationStatus() ==
        Timeline::ConfigurationStatus::ZeroHistoryCapacity);
  CHECK(zero_capacity.SubmitLocalInput(0, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::InvalidConfiguration);
  CHECK(zero_capacity.ResolveFrame(0).status ==
        Timeline::ResolveStatus::InvalidConfiguration);

  Timeline no_active_pads({.history_capacity = 1});
  CHECK(no_active_pads.GetConfigurationStatus() ==
        Timeline::ConfigurationStatus::NoActivePads);
  CHECK(no_active_pads.SubmitLocalInput(0, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::InvalidConfiguration);
  CHECK(no_active_pads.ResolveFrame(0).status ==
        Timeline::ResolveStatus::InvalidConfiguration);

  Timeline exhausted_frames({.pad_authority = {Timeline::PadAuthority::Local},
                             .history_capacity = 1,
                             .first_frame = std::numeric_limits<u64>::max()});
  CHECK(exhausted_frames.GetConfigurationStatus() ==
        Timeline::ConfigurationStatus::FirstFrameOutOfRange);
  CHECK(exhausted_frames.SubmitLocalInput(0, 0, Pad(PAD_BUTTON_A)) ==
        Timeline::SubmitStatus::InvalidConfiguration);
  CHECK(exhausted_frames.ResolveFrame(0).status ==
        Timeline::ResolveStatus::InvalidConfiguration);
  return true;
}
} // namespace

int main() {
  if (!TestPredictionConfirmationAndMultiPadLayout() ||
      !TestLateCorrectionAndRaceSafeAcknowledgement() ||
      !TestPredictionHorizonAndLocalInputs() ||
      !TestOutOfOrderConfirmationAndBoundedPruning() ||
      !TestSubmissionOrderDoesNotChangeResolvedPads() ||
      !TestConfirmedButUnconsumedInputIsNotPruned() ||
      !TestInvalidConfiguration()) {
    return 1;
  }
  return 0;
}
