// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace NetPlay
{

// This class deliberately owns only rollback input bookkeeping. It does not save
// state, restore the core, or replay frames. Keeping that boundary makes it safe
// to exercise before the emulator's output and side effects are rollback-aware.
class RollbackInputTimeline final
{
public:
  static constexpr std::size_t PAD_COUNT = 4;

  enum class PadAuthority : u8
  {
    Inactive,
    Local,
    Remote,
  };

  struct Config
  {
    std::array<PadAuthority, PAD_COUNT> pad_authority{};
    std::size_t history_capacity = 0;
    u64 max_prediction_frames = 0;
    u64 first_frame = 0;
  };

  enum class ConfigurationStatus : u8
  {
    Valid,
    ZeroHistoryCapacity,
    NoActivePads,
    FirstFrameOutOfRange,
  };

  enum class SubmitStatus : u8
  {
    Accepted,
    Duplicate,
    CorrectedPrediction,
    InvalidPad,
    WrongAuthority,
    ConflictingActual,
    TooOld,
    TooFarAhead,
    HistoryFull,
    InvalidConfiguration,
  };

  enum class ResolveStatus : u8
  {
    Resolved,
    TooOld,
    TooFarAhead,
    HistoryFull,
    MissingLocalInput,
    PredictionHorizonExceeded,
    InvalidConfiguration,
  };

  struct ResolvedFrame
  {
    u64 frame = 0;
    std::array<GCPadStatus, PAD_COUNT> pads{};
    u8 active_pad_mask = 0;
    u8 predicted_pad_mask = 0;
  };

  struct ResolveResult
  {
    ResolveStatus status = ResolveStatus::Resolved;
    ResolvedFrame frame{};

    explicit operator bool() const { return status == ResolveStatus::Resolved; }
  };

  // A generation makes acknowledgement race-safe: if another correction
  // arrives while the caller is replaying, the stale acknowledgement fails and
  // a fresh request must be consumed.
  struct RollbackRequest
  {
    u64 first_incorrect_frame = 0;
    u64 replay_through_frame = 0;
    u64 generation = 0;

    bool operator==(const RollbackRequest&) const = default;
  };

  explicit RollbackInputTimeline(Config config);

  ConfigurationStatus GetConfigurationStatus() const { return m_configuration_status; }

  SubmitStatus SubmitLocalInput(u64 frame, std::size_t pad, const GCPadStatus& input);
  SubmitStatus SubmitRemoteInput(u64 frame, std::size_t pad, const GCPadStatus& input);
  ResolveResult ResolveFrame(u64 frame);

  std::optional<u64> GetConfirmedThrough() const;
  std::optional<RollbackRequest> GetPendingRollback() const;
  bool AcknowledgeRollback(const RollbackRequest& request);

  u64 GetFirstRetainedFrame() const;
  std::size_t GetStoredFrameCount() const;

private:
  struct FrameRecord
  {
    std::array<GCPadStatus, PAD_COUNT> actual_inputs{};
    std::array<GCPadStatus, PAD_COUNT> resolved_inputs{};
    u8 actual_pad_mask = 0;
    u8 resolved_pad_mask = 0;
    u8 predicted_pad_mask = 0;
  };

  enum class FrameAccess : u8
  {
    Available,
    TooOld,
    TooFarAhead,
    HistoryFull,
  };

  SubmitStatus SubmitInput(u64 frame, std::size_t pad, PadAuthority expected_authority,
                           const GCPadStatus& input);
  FrameAccess GetOrCreateFrame(u64 frame, FrameRecord** record);
  bool PruneOneConfirmedFrame();
  void AdvanceConfirmedThrough();
  std::optional<GCPadStatus> PredictRemoteInput(u64 frame, std::size_t pad) const;
  void RecordPredictionMismatch(u64 frame);

  static bool PadInputsEqual(const GCPadStatus& lhs, const GCPadStatus& rhs);
  static ConfigurationStatus ValidateConfig(const Config& config, u8 active_pad_mask);
  static u8 PadBit(std::size_t pad) { return static_cast<u8>(u8{1} << pad); }

  const Config m_config;
  const u8 m_active_pad_mask;
  const ConfigurationStatus m_configuration_status;
  mutable std::mutex m_mutex;
  std::map<u64, FrameRecord> m_frames;
  std::array<GCPadStatus, PAD_COUNT> m_pruned_actual_inputs{};
  std::array<std::optional<u64>, PAD_COUNT> m_pruned_actual_frames{};
  u64 m_first_retained_frame;
  u64 m_next_unconfirmed_frame;
  std::optional<u64> m_confirmed_through;
  std::optional<u64> m_highest_resolved_frame;
  std::optional<RollbackRequest> m_pending_rollback;
  u64 m_rollback_generation = 0;
};

}  // namespace NetPlay
