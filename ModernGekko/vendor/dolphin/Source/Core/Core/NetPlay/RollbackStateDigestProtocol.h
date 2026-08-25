// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/NetPlay/NetPlayProto.h"

namespace NetPlay
{

constexpr u16 ROLLBACK_STATE_DIGEST_VERSION = 1;
constexpr u32 ROLLBACK_STATE_DIGEST_INTERVAL = 60;
constexpr std::size_t ROLLBACK_STATE_DIGEST_PACKET_SIZE = 36;
constexpr std::size_t ROLLBACK_STATE_DIGEST_MAX_PENDING_FRAMES = 8;
constexpr u32 ROLLBACK_STATE_DIGEST_MAX_LEAD_FRAMES =
    ROLLBACK_STATE_DIGEST_INTERVAL * ROLLBACK_STATE_DIGEST_MAX_PENDING_FRAMES;

// This is an error detector, not an authentication primitive. The connection
// compatibility fingerprint establishes an identical executable/content
// contract; these independent fields then localize live deterministic drift.
struct RollbackStateDigest
{
  u16 protocol_version = ROLLBACK_STATE_DIGEST_VERSION;
  u64 session_generation = 0;
  u32 logical_frame = 0;
  u32 mem1_crc32 = 0;
  u32 locked_l1_crc32 = 0;
  u64 emulated_timebase = 0;

  bool operator==(const RollbackStateDigest&) const = default;
};

enum class RollbackStateDigestDecodeStatus : u8
{
  Valid,
  WrongSize,
  WrongMagic,
  UnsupportedVersion,
  UnsupportedFlags,
  InvalidGeneration,
  InvalidLogicalFrame,
};

struct RollbackStateDigestDecodeResult
{
  RollbackStateDigestDecodeStatus status = RollbackStateDigestDecodeStatus::WrongSize;
  RollbackStateDigest digest{};

  explicit operator bool() const { return status == RollbackStateDigestDecodeStatus::Valid; }
};

std::optional<std::size_t> EncodeRollbackStateDigest(const RollbackStateDigest& digest,
                                                     std::span<u8> output);
RollbackStateDigestDecodeResult DecodeRollbackStateDigest(std::span<const u8> input);

// CPU-thread-owned staging for periodic checkpoints. A candidate is captured
// at the exact end of its logical frame, including during hidden replay, but it
// is released only after the input journal's contiguous actual frontier covers
// the final SI batch on which that frame depended. Replaying the same logical
// frame replaces its speculative candidate rather than publishing both.
class RollbackStateDigestCandidates final
{
public:
  struct Candidate
  {
    RollbackStateDigest digest{};
    std::optional<u64> required_confirmed_batch;
  };

  enum class CaptureStatus : u8
  {
    Captured,
    Replaced,
    InactiveSession,
    InvalidDigest,
    Full,
  };

  void Reset(u64 session_generation,
             std::size_t max_pending_frames = ROLLBACK_STATE_DIGEST_MAX_PENDING_FRAMES);
  CaptureStatus Capture(Candidate candidate);
  std::vector<RollbackStateDigest> TakeConfirmed(std::optional<u64> confirmed_through_batch,
                                                 bool correction_pending);

  bool IsActive() const { return m_session_generation != 0 && m_max_pending_frames != 0; }
  std::size_t Size() const { return m_candidates.size(); }

private:
  u64 m_session_generation = 0;
  std::size_t m_max_pending_frames = 0;
  std::map<u32, Candidate> m_candidates;
};

// Server-side, session-scoped aggregation. The tracker never grows with an
// attacker-controlled frame number: it accepts one report per expected player
// per periodic frame, bounds peer lead, and retains at most eight incomplete
// checkpoints. It is deliberately socket-free so every rejection rule has a
// deterministic unit test.
class RollbackStateDigestTracker final
{
public:
  struct Config
  {
    u64 session_generation = 0;
    std::vector<PlayerId> expected_players;
    std::size_t max_pending_frames = ROLLBACK_STATE_DIGEST_MAX_PENDING_FRAMES;
    u32 max_lead_frames = ROLLBACK_STATE_DIGEST_MAX_LEAD_FRAMES;
  };

  enum class SubmitStatus : u8
  {
    Accepted,
    Matched,
    Mismatch,
    InactiveSession,
    BadSession,
    UnexpectedPlayer,
    Malformed,
    Duplicate,
    Stale,
    Future,
  };

  struct SubmitResult
  {
    SubmitStatus status = SubmitStatus::InactiveSession;
    PlayerId blamed_player = 0;
  };

  void Reset(Config config);
  SubmitResult Submit(PlayerId player, const RollbackStateDigest& digest);

  bool IsActive() const { return m_active; }
  std::size_t GetPendingFrameCount() const { return m_reports.size(); }
  std::optional<u32> GetRetiredThroughFrame() const { return m_retired_through; }

private:
  using FrameReports = std::map<PlayerId, RollbackStateDigest>;

  PlayerId FindUniqueOutlier(const FrameReports& reports) const;

  Config m_config{};
  std::set<PlayerId> m_expected_players;
  std::map<u32, FrameReports> m_reports;
  std::map<PlayerId, u32> m_latest_by_player;
  std::optional<u32> m_retired_through;
  bool m_active = false;
};

}  // namespace NetPlay
