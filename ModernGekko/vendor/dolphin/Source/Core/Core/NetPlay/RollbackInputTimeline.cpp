// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackInputTimeline.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace NetPlay
{

RollbackInputTimeline::RollbackInputTimeline(Config config)
    : m_config(std::move(config)), m_active_pad_mask([this] {
        u8 mask = 0;
        for (std::size_t pad = 0; pad < PAD_COUNT; ++pad)
        {
          if (m_config.pad_authority[pad] != PadAuthority::Inactive)
            mask = static_cast<u8>(mask | PadBit(pad));
        }
        return mask;
      }()),
      m_configuration_status(ValidateConfig(m_config, m_active_pad_mask)),
      m_first_retained_frame(m_config.first_frame), m_next_unconfirmed_frame(m_config.first_frame)
{
}

RollbackInputTimeline::SubmitStatus
RollbackInputTimeline::SubmitLocalInput(const u64 frame, const std::size_t pad,
                                        const GCPadStatus& input)
{
  return SubmitInput(frame, pad, PadAuthority::Local, input);
}

RollbackInputTimeline::SubmitStatus
RollbackInputTimeline::SubmitRemoteInput(const u64 frame, const std::size_t pad,
                                         const GCPadStatus& input)
{
  return SubmitInput(frame, pad, PadAuthority::Remote, input);
}

RollbackInputTimeline::SubmitStatus
RollbackInputTimeline::SubmitInput(const u64 frame, const std::size_t pad,
                                   const PadAuthority expected_authority, const GCPadStatus& input)
{
  std::lock_guard guard(m_mutex);
  if (m_configuration_status != ConfigurationStatus::Valid)
    return SubmitStatus::InvalidConfiguration;
  if (pad >= PAD_COUNT)
    return SubmitStatus::InvalidPad;
  if (m_config.pad_authority[pad] != expected_authority)
    return SubmitStatus::WrongAuthority;

  FrameRecord* record = nullptr;
  switch (GetOrCreateFrame(frame, &record))
  {
  case FrameAccess::TooOld:
    return SubmitStatus::TooOld;
  case FrameAccess::TooFarAhead:
    return SubmitStatus::TooFarAhead;
  case FrameAccess::HistoryFull:
    return SubmitStatus::HistoryFull;
  case FrameAccess::Available:
    break;
  }

  const u8 bit = PadBit(pad);
  if ((record->actual_pad_mask & bit) != 0)
  {
    return PadInputsEqual(record->actual_inputs[pad], input) ? SubmitStatus::Duplicate :
                                                               SubmitStatus::ConflictingActual;
  }

  const bool corrected_prediction = (record->predicted_pad_mask & bit) != 0 &&
                                    !PadInputsEqual(record->resolved_inputs[pad], input);
  record->actual_inputs[pad] = input;
  record->actual_pad_mask = static_cast<u8>(record->actual_pad_mask | bit);

  if (corrected_prediction)
    RecordPredictionMismatch(frame);
  AdvanceConfirmedThrough();
  return corrected_prediction ? SubmitStatus::CorrectedPrediction : SubmitStatus::Accepted;
}

RollbackInputTimeline::ResolveResult RollbackInputTimeline::ResolveFrame(const u64 frame)
{
  std::lock_guard guard(m_mutex);
  if (m_configuration_status != ConfigurationStatus::Valid)
    return {.status = ResolveStatus::InvalidConfiguration};
  FrameRecord* record = nullptr;
  switch (GetOrCreateFrame(frame, &record))
  {
  case FrameAccess::TooOld:
    return {.status = ResolveStatus::TooOld};
  case FrameAccess::TooFarAhead:
    return {.status = ResolveStatus::TooFarAhead};
  case FrameAccess::HistoryFull:
    return {.status = ResolveStatus::HistoryFull};
  case FrameAccess::Available:
    break;
  }

  ResolvedFrame resolved{.frame = frame, .active_pad_mask = m_active_pad_mask};
  u8 predicted_mask = 0;
  for (std::size_t pad = 0; pad < PAD_COUNT; ++pad)
  {
    const u8 bit = PadBit(pad);
    if ((m_active_pad_mask & bit) == 0)
      continue;

    if ((record->actual_pad_mask & bit) != 0)
    {
      resolved.pads[pad] = record->actual_inputs[pad];
      continue;
    }

    if (m_config.pad_authority[pad] == PadAuthority::Local)
      return {.status = ResolveStatus::MissingLocalInput};

    const std::optional<GCPadStatus> prediction = PredictRemoteInput(frame, pad);
    if (!prediction)
      return {.status = ResolveStatus::PredictionHorizonExceeded};
    resolved.pads[pad] = *prediction;
    predicted_mask = static_cast<u8>(predicted_mask | bit);
  }

  resolved.predicted_pad_mask = predicted_mask;
  record->resolved_inputs = resolved.pads;
  record->resolved_pad_mask = m_active_pad_mask;
  record->predicted_pad_mask = predicted_mask;
  if (!m_highest_resolved_frame || frame > *m_highest_resolved_frame)
    m_highest_resolved_frame = frame;
  // A correction can arrive between two SI polls in the same emulated frame.
  // Keep the pending replay frontier at the latest batch that normal execution
  // has actually consumed; otherwise restoring that frame would run out of
  // journaled input partway through its deterministic poll schedule.
  if (m_pending_rollback && frame > m_pending_rollback->replay_through_frame)
    m_pending_rollback->replay_through_frame = frame;
  return {.status = ResolveStatus::Resolved, .frame = resolved};
}

std::optional<u64> RollbackInputTimeline::GetConfirmedThrough() const
{
  std::lock_guard guard(m_mutex);
  return m_confirmed_through;
}

std::optional<RollbackInputTimeline::RollbackRequest>
RollbackInputTimeline::GetPendingRollback() const
{
  std::lock_guard guard(m_mutex);
  return m_pending_rollback;
}

bool RollbackInputTimeline::AcknowledgeRollback(const RollbackRequest& request)
{
  std::lock_guard guard(m_mutex);
  if (!m_pending_rollback || *m_pending_rollback != request)
    return false;
  if (m_highest_resolved_frame && request.replay_through_frame < *m_highest_resolved_frame)
    return false;
  m_pending_rollback.reset();
  return true;
}

u64 RollbackInputTimeline::GetFirstRetainedFrame() const
{
  std::lock_guard guard(m_mutex);
  return m_first_retained_frame;
}

std::size_t RollbackInputTimeline::GetStoredFrameCount() const
{
  std::lock_guard guard(m_mutex);
  return m_frames.size();
}

RollbackInputTimeline::FrameAccess
RollbackInputTimeline::GetOrCreateFrame(const u64 frame, FrameRecord** const record)
{
  // Reserve the final value as the end-of-timeline sentinel so advancing the
  // contiguous confirmation frontier can never wrap to frame zero.
  if (frame == std::numeric_limits<u64>::max())
    return FrameAccess::TooFarAhead;
  if (frame < m_first_retained_frame)
    return FrameAccess::TooOld;

  // The confirmed frontier is also the base of the bounded receive window.
  // Subtraction is safe after the comparison and avoids addition overflow.
  if (frame >= m_next_unconfirmed_frame &&
      frame - m_next_unconfirmed_frame >= m_config.history_capacity)
  {
    return FrameAccess::TooFarAhead;
  }

  if (const auto existing = m_frames.find(frame); existing != m_frames.end())
  {
    *record = &existing->second;
    return FrameAccess::Available;
  }

  while (m_frames.size() >= m_config.history_capacity)
  {
    if (!PruneOneConfirmedFrame())
      return FrameAccess::HistoryFull;
  }

  *record = &m_frames.try_emplace(frame).first->second;
  return FrameAccess::Available;
}

bool RollbackInputTimeline::PruneOneConfirmedFrame()
{
  if (m_frames.empty())
    return false;

  const auto oldest = m_frames.begin();
  if (oldest->first >= m_next_unconfirmed_frame)
    return false;
  // Network confirmation may run ahead of emulation. Never discard an input
  // frame which the core has not consumed at least once.
  if ((oldest->second.resolved_pad_mask & m_active_pad_mask) != m_active_pad_mask)
    return false;
  if (m_pending_rollback && oldest->first >= m_pending_rollback->first_incorrect_frame)
    return false;

  for (std::size_t pad = 0; pad < PAD_COUNT; ++pad)
  {
    const u8 bit = PadBit(pad);
    if ((oldest->second.actual_pad_mask & bit) != 0)
    {
      m_pruned_actual_inputs[pad] = oldest->second.actual_inputs[pad];
      m_pruned_actual_frames[pad] = oldest->first;
    }
  }
  m_first_retained_frame = oldest->first + 1;
  m_frames.erase(oldest);
  return true;
}

void RollbackInputTimeline::AdvanceConfirmedThrough()
{
  while (true)
  {
    const auto current = m_frames.find(m_next_unconfirmed_frame);
    if (current == m_frames.end() ||
        (current->second.actual_pad_mask & m_active_pad_mask) != m_active_pad_mask)
    {
      return;
    }

    m_confirmed_through = m_next_unconfirmed_frame;
    ++m_next_unconfirmed_frame;
  }
}

std::optional<GCPadStatus> RollbackInputTimeline::PredictRemoteInput(const u64 frame,
                                                                     const std::size_t pad) const
{
  auto candidate = m_frames.upper_bound(frame);
  while (candidate != m_frames.begin())
  {
    --candidate;
    const u8 bit = PadBit(pad);
    if ((candidate->second.actual_pad_mask & bit) != 0)
    {
      const u64 distance = frame - candidate->first;
      if (distance > m_config.max_prediction_frames)
        return std::nullopt;
      return candidate->second.actual_inputs[pad];
    }
  }

  if (m_pruned_actual_frames[pad])
  {
    const u64 distance = frame - *m_pruned_actual_frames[pad];
    if (distance > m_config.max_prediction_frames)
      return std::nullopt;
    return m_pruned_actual_inputs[pad];
  }

  // Before the first remote sample, neutral is the deterministic baseline.
  // Frame first_frame is prediction one, not zero, so a zero horizon blocks it.
  const u64 distance_from_start = frame - m_config.first_frame;
  if (distance_from_start >= m_config.max_prediction_frames)
    return std::nullopt;
  return GCPadStatus{};
}

void RollbackInputTimeline::RecordPredictionMismatch(const u64 frame)
{
  ++m_rollback_generation;
  const u64 replay_through = std::max(frame, m_highest_resolved_frame.value_or(frame));
  if (m_pending_rollback)
  {
    m_pending_rollback->first_incorrect_frame =
        std::min(frame, m_pending_rollback->first_incorrect_frame);
    m_pending_rollback->replay_through_frame =
        std::max(replay_through, m_pending_rollback->replay_through_frame);
    m_pending_rollback->generation = m_rollback_generation;
  }
  else
  {
    m_pending_rollback = RollbackRequest{.first_incorrect_frame = frame,
                                         .replay_through_frame = replay_through,
                                         .generation = m_rollback_generation};
  }
}

bool RollbackInputTimeline::PadInputsEqual(const GCPadStatus& lhs, const GCPadStatus& rhs)
{
  // Field-wise comparison avoids making padding bytes part of deterministic
  // input identity.
  return lhs.button == rhs.button && lhs.stickX == rhs.stickX && lhs.stickY == rhs.stickY &&
         lhs.substickX == rhs.substickX && lhs.substickY == rhs.substickY &&
         lhs.triggerLeft == rhs.triggerLeft && lhs.triggerRight == rhs.triggerRight &&
         lhs.analogA == rhs.analogA && lhs.analogB == rhs.analogB && lhs.switches == rhs.switches &&
         lhs.isConnected == rhs.isConnected;
}

RollbackInputTimeline::ConfigurationStatus
RollbackInputTimeline::ValidateConfig(const Config& config, const u8 active_pad_mask)
{
  if (config.history_capacity == 0)
    return ConfigurationStatus::ZeroHistoryCapacity;
  if (active_pad_mask == 0)
    return ConfigurationStatus::NoActivePads;
  if (config.first_frame == std::numeric_limits<u64>::max())
    return ConfigurationStatus::FirstFrameOutOfRange;
  return ConfigurationStatus::Valid;
}

}  // namespace NetPlay
