// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>

#include "Common/CommonTypes.h"
#include "Core/NetPlay/RollbackInputTimeline.h"
#include "Core/NetPlay/RollbackSIInputProtocol.h"

namespace NetPlay
{

class RollbackCoordinator;

// Connects versioned network input slots to the exact place where the emulated
// SI consumed them. RollbackInputTimeline remains the prediction/confirmation
// authority; this class supplies the SI-specific application journal needed to
// turn a corrected batch into a frame-boundary replay request.
class RollbackSIInputJournal final
{
public:
  using Timeline = RollbackInputTimeline;

  struct Config
  {
    u16 protocol_version = ROLLBACK_SI_INPUT_VERSION;
    u64 session_generation = 0;
    u64 first_batch_id = 0;
    std::size_t history_capacity = 0;
    u64 max_prediction_batches = 0;
    // Bounds metadata retained for a partially confirmed emulated frame and
    // guarantees a complete frame can be replayed after a frame-start restore.
    u32 max_polls_per_frame = 0;
    std::array<Timeline::PadAuthority, Timeline::PAD_COUNT> pad_authority{};
  };

  enum class ConfigurationStatus : u8
  {
    Valid,
    UnsupportedVersion,
    InvalidGeneration,
    InvalidFirstBatch,
    InvalidPollCapacity,
    InvalidTimelineConfiguration,
  };

  struct AppliedBatch
  {
    u64 batch_id = 0;
    u64 emulated_frame = 0;
    u32 poll_ordinal = 0;
    u8 requested_pad_mask = 0;

    bool operator==(const AppliedBatch&) const = default;
  };

  enum class ObserveStatus : u8
  {
    Accepted,
    Duplicate,
    UnsupportedVersion,
    WrongGeneration,
    NonSequentialBatch,
    InvalidPollOrder,
    UnsupportedPadSchedule,
    ConflictingMetadata,
    TooOld,
    InvalidConfiguration,
  };

  enum class InputSource : u8
  {
    Local,
    Remote,
  };

  enum class SubmitStatus : u8
  {
    Accepted,
    Duplicate,
    CorrectedPrediction,
    UnsupportedVersion,
    WrongGeneration,
    InvalidPadMask,
    WrongAuthority,
    ConflictingActual,
    TooOld,
    TooFarAhead,
    HistoryFull,
    PartialFailure,
    InvalidConfiguration,
  };

  struct ResolveResult
  {
    Timeline::ResolveStatus status = Timeline::ResolveStatus::Resolved;
    AppliedBatch applied{};
    Timeline::ResolvedFrame inputs{};

    explicit operator bool() const { return status == Timeline::ResolveStatus::Resolved; }
  };

  struct ReplayTrigger
  {
    Timeline::RollbackRequest timeline_request{};
    // RollbackCoordinator operates on emulated-frame checkpoints, while the
    // timeline request above operates on SI batch IDs.
    Timeline::RollbackRequest emulated_frame_request{};
    u64 restore_before_emulated_frame = 0;
    u64 first_replay_batch_id = 0;
    u64 first_incorrect_batch_id = 0;
    u64 replay_through_batch_id = 0;
    u64 replay_through_emulated_frame = 0;

    bool operator==(const ReplayTrigger&) const = default;
  };

  explicit RollbackSIInputJournal(Config config);

  ConfigurationStatus GetConfigurationStatus() const { return m_configuration_status; }

  ObserveStatus ObserveAppliedBatch(u16 protocol_version, u64 session_generation,
                                    const AppliedBatch& applied);
  SubmitStatus SubmitInputBatch(u16 protocol_version, u64 session_generation, InputSource source,
                                const RollbackSIInputBatch& batch);
  ResolveResult ResolveBatch(u64 batch_id);

  std::optional<AppliedBatch> GetAppliedBatch(u64 batch_id) const;
  std::optional<u64> GetConfirmedThroughBatch() const;
  std::optional<ReplayTrigger> GetReplayTrigger() const;

private:
  friend class RollbackCoordinator;

  struct ActualBatch
  {
    std::array<GCPadStatus, Timeline::PAD_COUNT> pads{};
    u8 pad_mask = 0;
  };

  static Timeline::Config MakeTimelineConfig(const Config& config);
  static bool PadInputsEqual(const GCPadStatus& lhs, const GCPadStatus& rhs);
  static u8 PadBit(std::size_t pad) { return static_cast<u8>(u8{1} << pad); }

  SubmitStatus ConvertTimelineSubmitStatus(Timeline::SubmitStatus status) const;
  bool AcknowledgeAndCommitReplay(const ReplayTrigger& trigger, RollbackCoordinator& coordinator);
  void PruneMetadata();

  const Config m_config;
  Timeline m_timeline;
  const ConfigurationStatus m_configuration_status;
  const u8 m_active_pad_mask;
  mutable std::mutex m_mutex;
  std::map<u64, AppliedBatch> m_applied_batches;
  std::map<u64, ActualBatch> m_actual_batches;
  std::map<u64, Timeline::ResolvedFrame> m_resolved_batches;
  u64 m_next_applied_batch;
  std::optional<AppliedBatch> m_last_applied_batch;
};

}  // namespace NetPlay
