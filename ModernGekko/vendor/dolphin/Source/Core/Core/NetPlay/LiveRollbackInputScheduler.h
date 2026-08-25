// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <optional>

#include "Common/CommonTypes.h"
#include "Core/NetPlay/RollbackSIInputJournal.h"
#include "Core/NetPlay/RollbackSIInputProtocol.h"

namespace NetPlay
{

// CPU-thread scheduler for the grouped GC-controller SI batches used by live
// rollback. Network-thread delivery enters only through SubmitRemotePacket;
// RollbackSIInputJournal supplies the synchronization for that path.
//
// Input is sampled base_delay_batches ahead of consumption. The first delayed
// slots are deterministic connected-neutral actual input on every peer. A
// missing remote slot is repeat-last predicted only within the configured hard
// horizon; beyond it the caller must stop advancing the guest and wait/fallback.
class LiveRollbackInputScheduler final
{
public:
  using Journal = RollbackSIInputJournal;
  using Timeline = RollbackInputTimeline;

  struct Config
  {
    u16 protocol_version = ROLLBACK_SI_INPUT_VERSION;
    u64 session_generation = 0;
    u64 first_batch_id = 0;
    std::size_t history_capacity = 0;
    u64 base_delay_batches = 0;
    u64 max_prediction_batches = 0;
    u32 max_polls_per_frame = 0;
    std::size_t redundant_batch_count = 3;
    std::array<Timeline::PadAuthority, Timeline::PAD_COUNT> pad_authority{};
  };

  enum class ConfigurationStatus : u8
  {
    Valid,
    InvalidJournal,
    NoLocalPads,
    NoRemotePads,
    DelayOutOfRange,
    InsufficientHistory,
    InvalidRedundancy,
  };

  enum class BatchStatus : u8
  {
    Resolved,
    AwaitingRemoteInput,
    ReplayComplete,
    UnexpectedMode,
    InvalidPadMask,
    InvalidFrameOrder,
    InvalidReplayMapping,
    SubmitFailed,
    ResolveFailed,
    InvalidConfiguration,
  };

  enum class RemotePacketStatus : u8
  {
    Accepted,
    CorrectedPrediction,
    DuplicateOrRetired,
    WrongGeneration,
    InvalidPadMask,
    SubmitFailed,
    InvalidConfiguration,
  };

  struct BatchResult
  {
    BatchStatus status = BatchStatus::InvalidConfiguration;
    Journal::AppliedBatch applied{};
    Timeline::ResolvedFrame inputs{};
    std::optional<RollbackSIInputPacket> outgoing_packet;

    explicit operator bool() const { return status == BatchStatus::Resolved; }
  };

  explicit LiveRollbackInputScheduler(Config config);

  ConfigurationStatus GetConfigurationStatus() const { return m_configuration_status; }
  u8 GetActivePadMask() const { return m_active_pad_mask; }
  u8 GetLocalPadMask() const { return m_local_pad_mask; }
  u8 GetRemotePadMask() const { return m_remote_pad_mask; }
  u64 GetNextNormalBatchId() const { return m_next_normal_batch_id; }
  bool IsReplaying() const { return m_replay_trigger.has_value(); }

  // Called once at the beginning of a complete UpdateDevices poll. Retrying the
  // same batch after AwaitingRemoteInput reuses the originally sampled local
  // input; physical input is never resampled while the guest is stalled.
  BatchResult BeginNormalBatch(u64 emulated_frame, u32 poll_ordinal, u8 sampled_local_pad_mask,
                               const std::array<GCPadStatus, Timeline::PAD_COUNT>& sampled_pads);

  // Called once per replayed SI poll. The frame/poll mapping must exactly match
  // the locally observed first pass; sender-provided frame labels never exist.
  BatchResult BeginReplayBatch(u64 emulated_frame, u32 poll_ordinal);
  bool StartReplay(const Journal::ReplayTrigger& trigger);
  void CancelReplay();

  // Network-thread safe through the journal. The whole packet is authority-
  // checked before any batch mutates the timeline.
  RemotePacketStatus SubmitRemotePacket(const RollbackSIInputPacket& packet);

  std::optional<Journal::ReplayTrigger> GetReplayTrigger() const;
  Journal& GetJournal() { return m_journal; }

private:
  struct PendingNormalBatch
  {
    u64 batch_id = 0;
    RollbackSIInputBatch sampled_future{};
    RollbackSIInputPacket packet{};
  };

  static Journal::Config MakeJournalConfig(const Config& config);
  static GCPadStatus ConnectedNeutralPad();
  ConfigurationStatus ValidateConfiguration() const;
  bool SeedDelayWindow();
  std::optional<RollbackSIInputPacket> BuildOutgoingPacket(const RollbackSIInputBatch& newest);
  static bool IsAcceptedSubmitStatus(Journal::SubmitStatus status);

  const Config m_config;
  Journal m_journal;
  const u8 m_active_pad_mask;
  const u8 m_local_pad_mask;
  const u8 m_remote_pad_mask;
  ConfigurationStatus m_configuration_status;
  u64 m_next_normal_batch_id;
  std::optional<PendingNormalBatch> m_pending_normal;
  std::deque<RollbackSIInputBatch> m_local_history;
  std::optional<Journal::ReplayTrigger> m_replay_trigger;
  u64 m_next_replay_batch_id = 0;
};

}  // namespace NetPlay
