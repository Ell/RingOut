// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/LiveRollbackInputScheduler.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace NetPlay
{
namespace
{
u8 PadBit(const std::size_t pad)
{
  return static_cast<u8>(u8{1} << pad);
}

u8 AuthorityMask(const LiveRollbackInputScheduler::Config& config,
                 const RollbackInputTimeline::PadAuthority authority)
{
  u8 mask = 0;
  for (std::size_t pad = 0; pad < config.pad_authority.size(); ++pad)
  {
    if (config.pad_authority[pad] == authority)
      mask = static_cast<u8>(mask | PadBit(pad));
  }
  return mask;
}
}  // namespace

LiveRollbackInputScheduler::LiveRollbackInputScheduler(Config config)
    : m_config(std::move(config)), m_journal(MakeJournalConfig(m_config)),
      m_active_pad_mask(static_cast<u8>(AuthorityMask(m_config, Timeline::PadAuthority::Local) |
                                        AuthorityMask(m_config, Timeline::PadAuthority::Remote))),
      m_local_pad_mask(AuthorityMask(m_config, Timeline::PadAuthority::Local)),
      m_remote_pad_mask(AuthorityMask(m_config, Timeline::PadAuthority::Remote)),
      m_configuration_status(ValidateConfiguration()),
      m_next_normal_batch_id(m_config.first_batch_id)
{
  if (m_configuration_status == ConfigurationStatus::Valid && !SeedDelayWindow())
    m_configuration_status = ConfigurationStatus::InvalidJournal;
}

LiveRollbackInputScheduler::BatchResult LiveRollbackInputScheduler::BeginNormalBatch(
    const u64 emulated_frame, const u32 poll_ordinal, const u8 sampled_local_pad_mask,
    const std::array<GCPadStatus, Timeline::PAD_COUNT>& sampled_pads)
{
  if (m_configuration_status != ConfigurationStatus::Valid)
    return {.status = BatchStatus::InvalidConfiguration};
  if (m_replay_trigger)
    return {.status = BatchStatus::UnexpectedMode};
  if (sampled_local_pad_mask != m_local_pad_mask)
    return {.status = BatchStatus::InvalidPadMask};

  if (!m_pending_normal)
  {
    if (m_next_normal_batch_id > std::numeric_limits<u64>::max() - m_config.base_delay_batches)
    {
      return {.status = BatchStatus::SubmitFailed};
    }

    RollbackSIInputBatch future{.batch_id = m_next_normal_batch_id + m_config.base_delay_batches,
                                .pad_mask = m_local_pad_mask,
                                .pads = sampled_pads};
    const Journal::SubmitStatus submit =
        m_journal.SubmitInputBatch(m_config.protocol_version, m_config.session_generation,
                                   Journal::InputSource::Local, future);
    if (!IsAcceptedSubmitStatus(submit))
      return {.status = BatchStatus::SubmitFailed};

    const std::optional<RollbackSIInputPacket> packet = BuildOutgoingPacket(future);
    if (!packet)
      return {.status = BatchStatus::SubmitFailed};
    m_pending_normal = PendingNormalBatch{
        .batch_id = m_next_normal_batch_id, .sampled_future = future, .packet = *packet};
  }

  const Journal::AppliedBatch applied{.batch_id = m_next_normal_batch_id,
                                      .emulated_frame = emulated_frame,
                                      .poll_ordinal = poll_ordinal,
                                      .requested_pad_mask = m_active_pad_mask};
  const Journal::ObserveStatus observed = m_journal.ObserveAppliedBatch(
      m_config.protocol_version, m_config.session_generation, applied);
  if (observed != Journal::ObserveStatus::Accepted && observed != Journal::ObserveStatus::Duplicate)
  {
    return {.status = observed == Journal::ObserveStatus::InvalidPollOrder ?
                          BatchStatus::InvalidFrameOrder :
                          BatchStatus::ResolveFailed};
  }

  const Journal::ResolveResult resolved = m_journal.ResolveBatch(m_next_normal_batch_id);
  if (!resolved)
  {
    if (resolved.status == Timeline::ResolveStatus::PredictionHorizonExceeded)
    {
      return {.status = BatchStatus::AwaitingRemoteInput,
              .applied = applied,
              .outgoing_packet = m_pending_normal->packet};
    }
    return {.status = BatchStatus::ResolveFailed, .applied = applied};
  }

  BatchResult result{.status = BatchStatus::Resolved,
                     .applied = resolved.applied,
                     .inputs = resolved.inputs,
                     .outgoing_packet = m_pending_normal->packet};
  ++m_next_normal_batch_id;
  m_pending_normal.reset();
  return result;
}

LiveRollbackInputScheduler::BatchResult
LiveRollbackInputScheduler::BeginReplayBatch(const u64 emulated_frame, const u32 poll_ordinal)
{
  if (m_configuration_status != ConfigurationStatus::Valid)
    return {.status = BatchStatus::InvalidConfiguration};
  if (!m_replay_trigger)
    return {.status = BatchStatus::UnexpectedMode};
  if (m_next_replay_batch_id > m_replay_trigger->replay_through_batch_id)
    return {.status = BatchStatus::ReplayComplete};

  const std::optional<Journal::AppliedBatch> applied =
      m_journal.GetAppliedBatch(m_next_replay_batch_id);
  if (!applied || applied->emulated_frame != emulated_frame ||
      applied->poll_ordinal != poll_ordinal)
  {
    return {.status = BatchStatus::InvalidReplayMapping};
  }

  const Journal::ResolveResult resolved = m_journal.ResolveBatch(m_next_replay_batch_id);
  if (!resolved)
    return {.status = BatchStatus::ResolveFailed, .applied = *applied};

  BatchResult result{
      .status = BatchStatus::Resolved, .applied = resolved.applied, .inputs = resolved.inputs};
  ++m_next_replay_batch_id;
  return result;
}

bool LiveRollbackInputScheduler::StartReplay(const Journal::ReplayTrigger& trigger)
{
  if (m_configuration_status != ConfigurationStatus::Valid || m_replay_trigger ||
      trigger.first_replay_batch_id > trigger.replay_through_batch_id)
  {
    return false;
  }
  const std::optional<Journal::ReplayTrigger> pending = m_journal.GetReplayTrigger();
  if (!pending || *pending != trigger)
    return false;
  m_replay_trigger = trigger;
  m_next_replay_batch_id = trigger.first_replay_batch_id;
  return true;
}

void LiveRollbackInputScheduler::CancelReplay()
{
  m_replay_trigger.reset();
  m_next_replay_batch_id = 0;
}

LiveRollbackInputScheduler::RemotePacketStatus
LiveRollbackInputScheduler::SubmitRemotePacket(const RollbackSIInputPacket& packet)
{
  if (m_configuration_status != ConfigurationStatus::Valid)
    return RemotePacketStatus::InvalidConfiguration;
  if (packet.protocol_version != m_config.protocol_version ||
      packet.session_generation != m_config.session_generation)
  {
    return RemotePacketStatus::WrongGeneration;
  }
  if (packet.batch_count == 0 || packet.batch_count > packet.batches.size())
    return RemotePacketStatus::SubmitFailed;
  for (std::size_t i = 0; i < packet.batch_count; ++i)
  {
    if (packet.batches[i].pad_mask == 0 || (packet.batches[i].pad_mask & ~m_remote_pad_mask) != 0)
    {
      return RemotePacketStatus::InvalidPadMask;
    }
  }

  bool accepted = false;
  bool corrected = false;
  for (std::size_t i = 0; i < packet.batch_count; ++i)
  {
    const Journal::SubmitStatus submit =
        m_journal.SubmitInputBatch(m_config.protocol_version, m_config.session_generation,
                                   Journal::InputSource::Remote, packet.batches[i]);
    switch (submit)
    {
    case Journal::SubmitStatus::Accepted:
      accepted = true;
      break;
    case Journal::SubmitStatus::CorrectedPrediction:
      corrected = true;
      break;
    case Journal::SubmitStatus::Duplicate:
    case Journal::SubmitStatus::TooOld:
      break;
    default:
      return RemotePacketStatus::SubmitFailed;
    }
  }
  if (corrected)
    return RemotePacketStatus::CorrectedPrediction;
  if (accepted)
    return RemotePacketStatus::Accepted;
  return RemotePacketStatus::DuplicateOrRetired;
}

std::optional<LiveRollbackInputScheduler::Journal::ReplayTrigger>
LiveRollbackInputScheduler::GetReplayTrigger() const
{
  return m_journal.GetReplayTrigger();
}

LiveRollbackInputScheduler::Journal::Config
LiveRollbackInputScheduler::MakeJournalConfig(const Config& config)
{
  return {.protocol_version = config.protocol_version,
          .session_generation = config.session_generation,
          .first_batch_id = config.first_batch_id,
          .history_capacity = config.history_capacity,
          .max_prediction_batches = config.max_prediction_batches,
          .max_polls_per_frame = config.max_polls_per_frame,
          .pad_authority = config.pad_authority};
}

GCPadStatus LiveRollbackInputScheduler::ConnectedNeutralPad()
{
  GCPadStatus pad{};
  pad.isConnected = true;
  return pad;
}

LiveRollbackInputScheduler::ConfigurationStatus
LiveRollbackInputScheduler::ValidateConfiguration() const
{
  if (m_journal.GetConfigurationStatus() != Journal::ConfigurationStatus::Valid)
    return ConfigurationStatus::InvalidJournal;
  if (m_local_pad_mask == 0)
    return ConfigurationStatus::NoLocalPads;
  if (m_remote_pad_mask == 0)
    return ConfigurationStatus::NoRemotePads;
  if (m_config.base_delay_batches == 0 ||
      m_config.first_batch_id > std::numeric_limits<u64>::max() - m_config.base_delay_batches)
  {
    return ConfigurationStatus::DelayOutOfRange;
  }
  const u64 required_history = m_config.base_delay_batches + m_config.max_prediction_batches;
  if (required_history < m_config.base_delay_batches ||
      required_history > std::numeric_limits<u64>::max() - m_config.max_polls_per_frame ||
      m_config.history_capacity < required_history + m_config.max_polls_per_frame)
  {
    return ConfigurationStatus::InsufficientHistory;
  }
  if (m_config.redundant_batch_count == 0 ||
      m_config.redundant_batch_count > ROLLBACK_SI_MAX_BATCHES_PER_PACKET)
  {
    return ConfigurationStatus::InvalidRedundancy;
  }
  return ConfigurationStatus::Valid;
}

bool LiveRollbackInputScheduler::SeedDelayWindow()
{
  const GCPadStatus neutral = ConnectedNeutralPad();
  for (u64 offset = 0; offset < m_config.base_delay_batches; ++offset)
  {
    if (m_config.first_batch_id > std::numeric_limits<u64>::max() - offset)
      return false;
    const u64 batch_id = m_config.first_batch_id + offset;
    RollbackSIInputBatch local{.batch_id = batch_id, .pad_mask = m_local_pad_mask};
    RollbackSIInputBatch remote{.batch_id = batch_id, .pad_mask = m_remote_pad_mask};
    local.pads.fill(neutral);
    remote.pads.fill(neutral);
    if (!IsAcceptedSubmitStatus(m_journal.SubmitInputBatch(m_config.protocol_version,
                                                           m_config.session_generation,
                                                           Journal::InputSource::Local, local)) ||
        !IsAcceptedSubmitStatus(m_journal.SubmitInputBatch(m_config.protocol_version,
                                                           m_config.session_generation,
                                                           Journal::InputSource::Remote, remote)))
    {
      return false;
    }
  }
  return true;
}

std::optional<RollbackSIInputPacket>
LiveRollbackInputScheduler::BuildOutgoingPacket(const RollbackSIInputBatch& newest)
{
  if (!m_local_history.empty() && newest.batch_id <= m_local_history.back().batch_id)
    return std::nullopt;
  m_local_history.push_back(newest);
  while (m_local_history.size() > m_config.redundant_batch_count)
    m_local_history.pop_front();

  RollbackSIInputPacket packet{.protocol_version = m_config.protocol_version,
                               .session_generation = m_config.session_generation};
  if (const std::optional<u64> confirmed = m_journal.GetConfirmedThroughBatch())
  {
    packet.has_contiguous_ack = true;
    packet.contiguous_ack = *confirmed;
  }
  packet.batch_count = m_local_history.size();
  std::copy(m_local_history.begin(), m_local_history.end(), packet.batches.begin());
  return packet;
}

bool LiveRollbackInputScheduler::IsAcceptedSubmitStatus(const Journal::SubmitStatus status)
{
  return status == Journal::SubmitStatus::Accepted || status == Journal::SubmitStatus::Duplicate ||
         status == Journal::SubmitStatus::CorrectedPrediction;
}

}  // namespace NetPlay
