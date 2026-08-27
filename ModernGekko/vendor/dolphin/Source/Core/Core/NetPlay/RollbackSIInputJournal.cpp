// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackSIInputJournal.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

#include "Core/NetPlay/RollbackCoordinator.h"

namespace NetPlay
{
namespace
{
RollbackSIInputJournal::ConfigurationStatus
ValidateJournalConfig(const RollbackSIInputJournal::Config& config,
                      const RollbackInputTimeline::ConfigurationStatus timeline_status)
{
  if (config.protocol_version != ROLLBACK_SI_INPUT_VERSION)
    return RollbackSIInputJournal::ConfigurationStatus::UnsupportedVersion;
  if (config.session_generation == 0)
    return RollbackSIInputJournal::ConfigurationStatus::InvalidGeneration;
  if (config.first_batch_id == std::numeric_limits<u64>::max())
    return RollbackSIInputJournal::ConfigurationStatus::InvalidFirstBatch;
  if (timeline_status != RollbackInputTimeline::ConfigurationStatus::Valid)
    return RollbackSIInputJournal::ConfigurationStatus::InvalidTimelineConfiguration;
  if (config.max_polls_per_frame == 0 || config.history_capacity < config.max_polls_per_frame)
    return RollbackSIInputJournal::ConfigurationStatus::InvalidPollCapacity;
  return RollbackSIInputJournal::ConfigurationStatus::Valid;
}
}  // namespace

RollbackSIInputJournal::RollbackSIInputJournal(Config config)
    : m_config(std::move(config)), m_timeline(MakeTimelineConfig(m_config)),
      m_configuration_status(ValidateJournalConfig(m_config, m_timeline.GetConfigurationStatus())),
      m_active_pad_mask([this] {
        u8 mask = 0;
        for (std::size_t pad = 0; pad < Timeline::PAD_COUNT; ++pad)
        {
          if (m_config.pad_authority[pad] != Timeline::PadAuthority::Inactive)
            mask = static_cast<u8>(mask | PadBit(pad));
        }
        return mask;
      }()),
      m_next_applied_batch(m_config.first_batch_id)
{
}

RollbackSIInputJournal::ObserveStatus RollbackSIInputJournal::ObserveAppliedBatch(
    const u16 protocol_version, const u64 session_generation, const AppliedBatch& applied)
{
  std::lock_guard guard(m_mutex);
  if (m_configuration_status != ConfigurationStatus::Valid)
    return ObserveStatus::InvalidConfiguration;
  if (protocol_version != m_config.protocol_version)
    return ObserveStatus::UnsupportedVersion;
  if (session_generation != m_config.session_generation)
    return ObserveStatus::WrongGeneration;
  // RollbackInputTimeline currently has a fixed active-pad mask. Ordinary
  // UpdateDevices polling requests every mapped pad, but guest-initiated
  // RunSIBuffer transfers can request only one. Do not silently turn that
  // different schedule into a full-pad sample; live partial-poll support needs
  // a dynamic required mask in the timeline first.
  if (applied.requested_pad_mask != m_active_pad_mask)
    return ObserveStatus::UnsupportedPadSchedule;

  if (const auto existing = m_applied_batches.find(applied.batch_id);
      existing != m_applied_batches.end())
  {
    return existing->second == applied ? ObserveStatus::Duplicate :
                                         ObserveStatus::ConflictingMetadata;
  }
  if (applied.batch_id < m_next_applied_batch)
    return ObserveStatus::TooOld;
  if (applied.batch_id != m_next_applied_batch ||
      applied.batch_id == std::numeric_limits<u64>::max())
  {
    return ObserveStatus::NonSequentialBatch;
  }

  if (!m_last_applied_batch)
  {
    if (applied.poll_ordinal != 0)
      return ObserveStatus::InvalidPollOrder;
  }
  else if (applied.emulated_frame == m_last_applied_batch->emulated_frame)
  {
    if (m_last_applied_batch->poll_ordinal == std::numeric_limits<u32>::max() ||
        applied.poll_ordinal != m_last_applied_batch->poll_ordinal + 1 ||
        applied.poll_ordinal >= m_config.max_polls_per_frame)
    {
      return ObserveStatus::InvalidPollOrder;
    }
  }
  else
  {
    if (applied.emulated_frame < m_last_applied_batch->emulated_frame || applied.poll_ordinal != 0)
      return ObserveStatus::InvalidPollOrder;
  }

  m_applied_batches.emplace(applied.batch_id, applied);
  m_last_applied_batch = applied;
  ++m_next_applied_batch;
  PruneMetadata();
  return ObserveStatus::Accepted;
}

RollbackSIInputJournal::SubmitStatus
RollbackSIInputJournal::SubmitInputBatch(const u16 protocol_version, const u64 session_generation,
                                         const InputSource source,
                                         const RollbackSIInputBatch& batch)
{
  std::lock_guard guard(m_mutex);
  if (m_configuration_status != ConfigurationStatus::Valid)
    return SubmitStatus::InvalidConfiguration;
  if (protocol_version != m_config.protocol_version)
    return SubmitStatus::UnsupportedVersion;
  if (session_generation != m_config.session_generation)
    return SubmitStatus::WrongGeneration;
  if (batch.pad_mask == 0 || (batch.pad_mask & 0xf0) != 0)
    return SubmitStatus::InvalidPadMask;

  const Timeline::PadAuthority expected =
      source == InputSource::Local ? Timeline::PadAuthority::Local : Timeline::PadAuthority::Remote;
  for (std::size_t pad = 0; pad < Timeline::PAD_COUNT; ++pad)
  {
    if ((batch.pad_mask & PadBit(pad)) != 0 && m_config.pad_authority[pad] != expected)
      return SubmitStatus::WrongAuthority;
  }

  const auto existing = m_actual_batches.find(batch.batch_id);
  if (existing != m_actual_batches.end())
  {
    for (std::size_t pad = 0; pad < Timeline::PAD_COUNT; ++pad)
    {
      const u8 bit = PadBit(pad);
      if ((batch.pad_mask & bit) != 0 && (existing->second.pad_mask & bit) != 0 &&
          !PadInputsEqual(batch.pads[pad], existing->second.pads[pad]))
      {
        return SubmitStatus::ConflictingActual;
      }
    }
  }

  bool accepted = false;
  bool corrected = false;
  bool submitted_any = false;
  for (std::size_t pad = 0; pad < Timeline::PAD_COUNT; ++pad)
  {
    const u8 bit = PadBit(pad);
    if ((batch.pad_mask & bit) == 0)
      continue;

    const Timeline::SubmitStatus timeline_status =
        source == InputSource::Local ?
            m_timeline.SubmitLocalInput(batch.batch_id, pad, batch.pads[pad]) :
            m_timeline.SubmitRemoteInput(batch.batch_id, pad, batch.pads[pad]);
    const SubmitStatus converted = ConvertTimelineSubmitStatus(timeline_status);
    if (converted != SubmitStatus::Accepted && converted != SubmitStatus::Duplicate &&
        converted != SubmitStatus::CorrectedPrediction)
    {
      return submitted_any ? SubmitStatus::PartialFailure : converted;
    }

    submitted_any = true;
    accepted = accepted || converted == SubmitStatus::Accepted;
    corrected = corrected || converted == SubmitStatus::CorrectedPrediction;
    auto& actual = m_actual_batches[batch.batch_id];
    actual.pads[pad] = batch.pads[pad];
    actual.pad_mask = static_cast<u8>(actual.pad_mask | bit);
  }

  PruneMetadata();
  if (corrected)
    return SubmitStatus::CorrectedPrediction;
  return accepted ? SubmitStatus::Accepted : SubmitStatus::Duplicate;
}

RollbackSIInputJournal::ResolveResult RollbackSIInputJournal::ResolveBatch(const u64 batch_id)
{
  std::lock_guard guard(m_mutex);
  if (m_configuration_status != ConfigurationStatus::Valid)
    return {.status = Timeline::ResolveStatus::InvalidConfiguration};
  const auto applied = m_applied_batches.find(batch_id);
  if (applied == m_applied_batches.end())
  {
    return {.status = batch_id < m_next_applied_batch ? Timeline::ResolveStatus::TooOld :
                                                        Timeline::ResolveStatus::TooFarAhead};
  }

  const AppliedBatch applied_copy = applied->second;
  const Timeline::ResolveResult result = m_timeline.ResolveFrame(batch_id);
  if (result.status == Timeline::ResolveStatus::TooOld)
  {
    if (const auto cached = m_resolved_batches.find(batch_id); cached != m_resolved_batches.end())
      return {.status = Timeline::ResolveStatus::Resolved,
              .applied = applied_copy,
              .inputs = cached->second};
  }
  if (result)
    m_resolved_batches[batch_id] = result.frame;
  PruneMetadata();
  return {.status = result.status, .applied = applied_copy, .inputs = result.frame};
}

std::optional<RollbackSIInputJournal::AppliedBatch>
RollbackSIInputJournal::GetAppliedBatch(const u64 batch_id) const
{
  std::lock_guard guard(m_mutex);
  const auto applied = m_applied_batches.find(batch_id);
  return applied == m_applied_batches.end() ? std::nullopt : std::optional{applied->second};
}

std::optional<u64> RollbackSIInputJournal::GetConfirmedThroughBatch() const
{
  std::lock_guard guard(m_mutex);
  return m_timeline.GetConfirmedThrough();
}

std::optional<u64>
RollbackSIInputJournal::GetLastAppliedBatchThroughEmulatedFrame(const u64 emulated_frame) const
{
  std::lock_guard guard(m_mutex);
  for (auto applied = m_applied_batches.rbegin(); applied != m_applied_batches.rend(); ++applied)
  {
    if (applied->second.emulated_frame <= emulated_frame)
      return applied->first;
  }
  return std::nullopt;
}

bool RollbackSIInputJournal::IsEmulatedFrameAuthoritative(const u64 emulated_frame) const
{
  std::lock_guard guard(m_mutex);
  if (m_configuration_status != ConfigurationStatus::Valid || m_timeline.GetPendingRollback())
    return false;
  const std::optional<u64> confirmed = m_timeline.GetConfirmedThrough();
  if (!confirmed || !m_last_applied_batch || m_last_applied_batch->emulated_frame != emulated_frame)
    return false;
  return *confirmed >= m_last_applied_batch->batch_id &&
         m_resolved_batches.contains(m_last_applied_batch->batch_id);
}

std::optional<RollbackSIInputJournal::ReplayTrigger>
RollbackSIInputJournal::GetReplayTrigger() const
{
  std::lock_guard guard(m_mutex);
  const std::optional<Timeline::RollbackRequest> request = m_timeline.GetPendingRollback();
  if (!request)
    return std::nullopt;

  const auto incorrect = m_applied_batches.find(request->first_incorrect_frame);
  const auto replay_through = m_applied_batches.find(request->replay_through_frame);
  if (incorrect == m_applied_batches.end() || replay_through == m_applied_batches.end())
    return std::nullopt;

  auto first_replay = incorrect;
  while (first_replay != m_applied_batches.begin())
  {
    const auto previous = std::prev(first_replay);
    if (previous->second.emulated_frame != incorrect->second.emulated_frame)
      break;
    first_replay = previous;
  }

  return ReplayTrigger{
      .timeline_request = *request,
      .emulated_frame_request = {.first_incorrect_frame = incorrect->second.emulated_frame,
                                 .replay_through_frame = replay_through->second.emulated_frame,
                                 .generation = request->generation},
      .restore_before_emulated_frame = incorrect->second.emulated_frame,
      .first_replay_batch_id = first_replay->first,
      .first_incorrect_batch_id = incorrect->first,
      .replay_through_batch_id = replay_through->first,
      .replay_through_emulated_frame = replay_through->second.emulated_frame};
}

bool RollbackSIInputJournal::AcknowledgeSelectiveReplay(const ReplayTrigger& trigger)
{
  std::lock_guard guard(m_mutex);
  const std::optional<Timeline::RollbackRequest> pending = m_timeline.GetPendingRollback();
  if (!pending || *pending != trigger.timeline_request)
    return false;
  if (!m_timeline.AcknowledgeRollback(trigger.timeline_request))
    return false;
  PruneMetadata();
  return true;
}

bool RollbackSIInputJournal::AcknowledgeAndCommitReplay(const ReplayTrigger& trigger,
                                                        RollbackCoordinator& coordinator)
{
  std::lock_guard guard(m_mutex);
  const std::optional<Timeline::RollbackRequest> pending = m_timeline.GetPendingRollback();
  if (!pending || *pending != trigger.timeline_request || !coordinator.CanCommitReplay(trigger))
    return false;

  const bool acknowledged = m_timeline.AcknowledgeRollback(trigger.timeline_request);
  if (!acknowledged)
    return false;

  // Publishing while m_mutex remains held makes acknowledgement and output
  // visibility one linearizable operation relative to network-thread input
  // submission. RollbackOutputGate::EndHiddenReplay must not re-enter this
  // journal.
  const bool published = coordinator.CommitAcknowledgedReplay(trigger);
  PruneMetadata();
  return published;
}

RollbackSIInputJournal::Timeline::Config
RollbackSIInputJournal::MakeTimelineConfig(const Config& config)
{
  return {.pad_authority = config.pad_authority,
          .history_capacity = config.history_capacity,
          .max_prediction_frames = config.max_prediction_batches,
          .first_frame = config.first_batch_id};
}

bool RollbackSIInputJournal::PadInputsEqual(const GCPadStatus& lhs, const GCPadStatus& rhs)
{
  return lhs.button == rhs.button && lhs.stickX == rhs.stickX && lhs.stickY == rhs.stickY &&
         lhs.substickX == rhs.substickX && lhs.substickY == rhs.substickY &&
         lhs.triggerLeft == rhs.triggerLeft && lhs.triggerRight == rhs.triggerRight &&
         lhs.analogA == rhs.analogA && lhs.analogB == rhs.analogB && lhs.switches == rhs.switches &&
         lhs.isConnected == rhs.isConnected;
}

RollbackSIInputJournal::SubmitStatus
RollbackSIInputJournal::ConvertTimelineSubmitStatus(const Timeline::SubmitStatus status) const
{
  switch (status)
  {
  case Timeline::SubmitStatus::Accepted:
    return SubmitStatus::Accepted;
  case Timeline::SubmitStatus::Duplicate:
    return SubmitStatus::Duplicate;
  case Timeline::SubmitStatus::CorrectedPrediction:
    return SubmitStatus::CorrectedPrediction;
  case Timeline::SubmitStatus::InvalidPad:
    return SubmitStatus::InvalidPadMask;
  case Timeline::SubmitStatus::WrongAuthority:
    return SubmitStatus::WrongAuthority;
  case Timeline::SubmitStatus::ConflictingActual:
    return SubmitStatus::ConflictingActual;
  case Timeline::SubmitStatus::TooOld:
    return SubmitStatus::TooOld;
  case Timeline::SubmitStatus::TooFarAhead:
    return SubmitStatus::TooFarAhead;
  case Timeline::SubmitStatus::HistoryFull:
    return SubmitStatus::HistoryFull;
  case Timeline::SubmitStatus::InvalidConfiguration:
    return SubmitStatus::InvalidConfiguration;
  }
  return SubmitStatus::PartialFailure;
}

void RollbackSIInputJournal::PruneMetadata()
{
  u64 keep_from = m_timeline.GetFirstRetainedFrame();
  const auto retain_complete_frame = [this, &keep_from](const u64 batch_id) {
    const auto anchor = m_applied_batches.find(batch_id);
    if (anchor == m_applied_batches.end())
      return;
    auto first = anchor;
    while (first != m_applied_batches.begin())
    {
      const auto previous = std::prev(first);
      if (previous->second.emulated_frame != anchor->second.emulated_frame)
        break;
      first = previous;
    }
    keep_from = std::min(keep_from, first->first);
  };

  // The current frame may receive another poll, so do not discard its first
  // polls merely because all inputs observed so far are confirmed.
  if (m_last_applied_batch)
    retain_complete_frame(m_last_applied_batch->batch_id);

  // If the next batch is unconfirmed, retain every earlier poll in its frame.
  const std::optional<u64> confirmed = m_timeline.GetConfirmedThrough();
  if (!confirmed)
    retain_complete_frame(m_config.first_batch_id);
  else if (*confirmed != std::numeric_limits<u64>::max())
    retain_complete_frame(*confirmed + 1);

  if (const auto pending = m_timeline.GetPendingRollback())
    retain_complete_frame(pending->first_incorrect_frame);

  m_applied_batches.erase(m_applied_batches.begin(), m_applied_batches.lower_bound(keep_from));
  m_actual_batches.erase(m_actual_batches.begin(), m_actual_batches.lower_bound(keep_from));
  m_resolved_batches.erase(m_resolved_batches.begin(), m_resolved_batches.lower_bound(keep_from));
}

}  // namespace NetPlay
