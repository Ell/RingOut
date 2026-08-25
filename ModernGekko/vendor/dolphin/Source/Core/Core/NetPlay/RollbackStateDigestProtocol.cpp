// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackStateDigestProtocol.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace NetPlay
{
namespace
{
constexpr u32 MAGIC = 0x52534447;  // "RSDG"
constexpr u16 FLAGS = 0;

class Writer
{
public:
  explicit Writer(std::span<u8> output) : m_output(output) {}
  bool U8(u8 value)
  {
    if (m_position >= m_output.size())
      return false;
    m_output[m_position++] = value;
    return true;
  }
  bool U16(u16 value) { return U8(static_cast<u8>(value >> 8)) && U8(static_cast<u8>(value)); }
  bool U32(u32 value)
  {
    return U16(static_cast<u16>(value >> 16)) && U16(static_cast<u16>(value));
  }
  bool U64(u64 value)
  {
    return U32(static_cast<u32>(value >> 32)) && U32(static_cast<u32>(value));
  }
  std::size_t Size() const { return m_position; }

private:
  std::span<u8> m_output;
  std::size_t m_position = 0;
};

class Reader
{
public:
  explicit Reader(std::span<const u8> input) : m_input(input) {}
  bool U8(u8* value)
  {
    if (m_position >= m_input.size())
      return false;
    *value = m_input[m_position++];
    return true;
  }
  bool U16(u16* value)
  {
    u8 high = 0;
    u8 low = 0;
    if (!U8(&high) || !U8(&low))
      return false;
    *value = static_cast<u16>((static_cast<u16>(high) << 8) | low);
    return true;
  }
  bool U32(u32* value)
  {
    u16 high = 0;
    u16 low = 0;
    if (!U16(&high) || !U16(&low))
      return false;
    *value = (static_cast<u32>(high) << 16) | low;
    return true;
  }
  bool U64(u64* value)
  {
    u32 high = 0;
    u32 low = 0;
    if (!U32(&high) || !U32(&low))
      return false;
    *value = (static_cast<u64>(high) << 32) | low;
    return true;
  }

private:
  std::span<const u8> m_input;
  std::size_t m_position = 0;
};

bool IsValidLogicalFrame(const u32 frame)
{
  return frame != 0 && frame % ROLLBACK_STATE_DIGEST_INTERVAL == 0;
}

bool SameState(const RollbackStateDigest& lhs, const RollbackStateDigest& rhs)
{
  return lhs.mem1_crc32 == rhs.mem1_crc32 && lhs.locked_l1_crc32 == rhs.locked_l1_crc32 &&
         lhs.emulated_timebase == rhs.emulated_timebase;
}
}  // namespace

std::optional<std::size_t> EncodeRollbackStateDigest(const RollbackStateDigest& digest,
                                                     const std::span<u8> output)
{
  if (output.size() < ROLLBACK_STATE_DIGEST_PACKET_SIZE ||
      digest.protocol_version != ROLLBACK_STATE_DIGEST_VERSION ||
      digest.session_generation == 0 || !IsValidLogicalFrame(digest.logical_frame))
  {
    return std::nullopt;
  }

  Writer writer(output.first(ROLLBACK_STATE_DIGEST_PACKET_SIZE));
  if (!writer.U32(MAGIC) || !writer.U16(digest.protocol_version) || !writer.U16(FLAGS) ||
      !writer.U64(digest.session_generation) || !writer.U32(digest.logical_frame) ||
      !writer.U32(digest.mem1_crc32) || !writer.U32(digest.locked_l1_crc32) ||
      !writer.U64(digest.emulated_timebase))
  {
    return std::nullopt;
  }
  return writer.Size();
}

RollbackStateDigestDecodeResult DecodeRollbackStateDigest(const std::span<const u8> input)
{
  if (input.size() != ROLLBACK_STATE_DIGEST_PACKET_SIZE)
    return {.status = RollbackStateDigestDecodeStatus::WrongSize};

  u32 magic = 0;
  u16 flags = 0;
  RollbackStateDigest digest{};
  Reader reader(input);
  if (!reader.U32(&magic) || !reader.U16(&digest.protocol_version) || !reader.U16(&flags) ||
      !reader.U64(&digest.session_generation) || !reader.U32(&digest.logical_frame) ||
      !reader.U32(&digest.mem1_crc32) || !reader.U32(&digest.locked_l1_crc32) ||
      !reader.U64(&digest.emulated_timebase))
  {
    return {.status = RollbackStateDigestDecodeStatus::WrongSize};
  }
  if (magic != MAGIC)
    return {.status = RollbackStateDigestDecodeStatus::WrongMagic};
  if (digest.protocol_version != ROLLBACK_STATE_DIGEST_VERSION)
    return {.status = RollbackStateDigestDecodeStatus::UnsupportedVersion};
  if (flags != FLAGS)
    return {.status = RollbackStateDigestDecodeStatus::UnsupportedFlags};
  if (digest.session_generation == 0)
    return {.status = RollbackStateDigestDecodeStatus::InvalidGeneration};
  if (!IsValidLogicalFrame(digest.logical_frame))
    return {.status = RollbackStateDigestDecodeStatus::InvalidLogicalFrame};
  return {.status = RollbackStateDigestDecodeStatus::Valid, .digest = digest};
}

void RollbackStateDigestCandidates::Reset(const u64 session_generation,
                                          const std::size_t max_pending_frames)
{
  m_session_generation = session_generation;
  m_max_pending_frames = max_pending_frames;
  m_candidates.clear();
}

RollbackStateDigestCandidates::CaptureStatus
RollbackStateDigestCandidates::Capture(Candidate candidate)
{
  if (!IsActive())
    return CaptureStatus::InactiveSession;
  if (candidate.digest.protocol_version != ROLLBACK_STATE_DIGEST_VERSION ||
      candidate.digest.session_generation != m_session_generation ||
      !IsValidLogicalFrame(candidate.digest.logical_frame))
  {
    return CaptureStatus::InvalidDigest;
  }

  const auto existing = m_candidates.find(candidate.digest.logical_frame);
  if (existing != m_candidates.end())
  {
    existing->second = std::move(candidate);
    return CaptureStatus::Replaced;
  }
  if (m_candidates.size() >= m_max_pending_frames)
    return CaptureStatus::Full;
  m_candidates.emplace(candidate.digest.logical_frame, std::move(candidate));
  return CaptureStatus::Captured;
}

std::vector<RollbackStateDigest> RollbackStateDigestCandidates::TakeConfirmed(
    const std::optional<u64> confirmed_through_batch, const bool correction_pending)
{
  std::vector<RollbackStateDigest> confirmed;
  if (!IsActive() || correction_pending)
    return confirmed;

  for (auto candidate = m_candidates.begin(); candidate != m_candidates.end();)
  {
    const std::optional<u64> required = candidate->second.required_confirmed_batch;
    if (required && (!confirmed_through_batch || *confirmed_through_batch < *required))
    {
      // Candidates are frame ordered and their batch dependencies are
      // monotonic. Never let a later report overtake an earlier unconfirmed
      // checkpoint and violate the server's reliable cadence contract.
      break;
    }
    confirmed.push_back(candidate->second.digest);
    candidate = m_candidates.erase(candidate);
  }
  return confirmed;
}

void RollbackStateDigestTracker::Reset(Config config)
{
  m_config = std::move(config);
  m_expected_players.clear();
  m_reports.clear();
  m_latest_by_player.clear();
  m_retired_through.reset();
  m_active = false;

  if (m_config.session_generation == 0 || m_config.expected_players.empty() ||
      m_config.max_pending_frames == 0 || m_config.max_lead_frames == 0)
  {
    return;
  }
  for (const PlayerId player : m_config.expected_players)
  {
    if (player == 0 || !m_expected_players.insert(player).second)
    {
      m_expected_players.clear();
      return;
    }
  }
  m_active = true;
}

RollbackStateDigestTracker::SubmitResult
RollbackStateDigestTracker::Submit(const PlayerId player, const RollbackStateDigest& digest)
{
  if (!m_active)
    return {.status = SubmitStatus::InactiveSession};
  if (digest.protocol_version != ROLLBACK_STATE_DIGEST_VERSION ||
      !IsValidLogicalFrame(digest.logical_frame))
  {
    return {.status = SubmitStatus::Malformed};
  }
  if (digest.session_generation != m_config.session_generation)
    return {.status = SubmitStatus::BadSession};
  if (!m_expected_players.contains(player))
    return {.status = SubmitStatus::UnexpectedPlayer};
  const auto frame_it = m_reports.find(digest.logical_frame);
  if (frame_it != m_reports.end() && frame_it->second.contains(player))
    return {.status = SubmitStatus::Duplicate};
  if (m_retired_through && digest.logical_frame <= *m_retired_through)
    return {.status = SubmitStatus::Stale};
  const auto latest = m_latest_by_player.find(player);
  const u64 expected_frame = latest == m_latest_by_player.end() ?
                                 ROLLBACK_STATE_DIGEST_INTERVAL :
                                 static_cast<u64>(latest->second) +
                                     ROLLBACK_STATE_DIGEST_INTERVAL;
  if (digest.logical_frame < expected_frame)
  {
    return {.status = SubmitStatus::Stale};
  }
  if (digest.logical_frame > expected_frame)
    return {.status = SubmitStatus::Future};

  u32 slowest_frame = std::numeric_limits<u32>::max();
  for (const PlayerId expected_player : m_expected_players)
  {
    const auto progress = m_latest_by_player.find(expected_player);
    slowest_frame = std::min(slowest_frame,
                             progress == m_latest_by_player.end() ? 0u : progress->second);
  }
  if (digest.logical_frame > slowest_frame &&
      digest.logical_frame - slowest_frame > m_config.max_lead_frames)
  {
    return {.status = SubmitStatus::Future};
  }

  if (!m_reports.contains(digest.logical_frame) &&
      m_reports.size() >= m_config.max_pending_frames)
  {
    return {.status = SubmitStatus::Future};
  }

  FrameReports& reports = m_reports[digest.logical_frame];
  reports.emplace(player, digest);
  m_latest_by_player[player] = digest.logical_frame;

  const auto accepted = m_reports.find(digest.logical_frame);
  if (accepted == m_reports.end() || accepted->second.size() < m_expected_players.size())
    return {.status = SubmitStatus::Accepted};

  const FrameReports complete = accepted->second;
  const RollbackStateDigest& reference = complete.begin()->second;
  const bool matched = std::ranges::all_of(complete, [&](const auto& entry) {
    return SameState(reference, entry.second);
  });
  const PlayerId blamed_player = matched ? 0 : FindUniqueOutlier(complete);
  m_retired_through = std::max(m_retired_through.value_or(0), digest.logical_frame);
  m_reports.erase(accepted);
  return {.status = matched ? SubmitStatus::Matched : SubmitStatus::Mismatch,
          .blamed_player = blamed_player};
}

PlayerId RollbackStateDigestTracker::FindUniqueOutlier(const FrameReports& reports) const
{
  if (reports.size() < 3)
    return 0;
  for (const auto& [player, report] : reports)
  {
    const auto reference = std::ranges::find_if(
        reports, [&](const auto& other) { return other.first != player; });
    if (reference == reports.end() || SameState(report, reference->second))
      continue;
    const bool remaining_agree = std::ranges::all_of(reports, [&](const auto& other) {
      return other.first == player || SameState(reference->second, other.second);
    });
    if (remaining_agree)
      return player;
  }
  return 0;
}

}  // namespace NetPlay
