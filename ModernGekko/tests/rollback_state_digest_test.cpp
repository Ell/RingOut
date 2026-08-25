// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackStateDigestProtocol.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
void Check(const bool condition, const char* expression, const int line)
{
  if (condition)
    return;
  std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
  std::exit(1);
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

NetPlay::RollbackStateDigest Digest(const u32 frame = 60, const u64 generation = 77,
                                    const u32 mem1 = 0x12345678,
                                    const u32 l1 = 0x9abcdef0, const u64 timebase = 0x11223344)
{
  return {.session_generation = generation,
          .logical_frame = frame,
          .mem1_crc32 = mem1,
          .locked_l1_crc32 = l1,
          .emulated_timebase = timebase};
}
}  // namespace

int main()
{
  using DecodeStatus = NetPlay::RollbackStateDigestDecodeStatus;
  using SubmitStatus = NetPlay::RollbackStateDigestTracker::SubmitStatus;

  std::array<u8, NetPlay::ROLLBACK_STATE_DIGEST_PACKET_SIZE> encoded{};
  const auto size = NetPlay::EncodeRollbackStateDigest(Digest(), encoded);
  CHECK(size && *size == encoded.size());
  const auto decoded = NetPlay::DecodeRollbackStateDigest(encoded);
  CHECK(static_cast<bool>(decoded));
  CHECK(decoded.digest == Digest());

  auto short_packet = encoded;
  CHECK(NetPlay::DecodeRollbackStateDigest(
            std::span<const u8>(short_packet.data(), short_packet.size() - 1))
            .status == DecodeStatus::WrongSize);
  auto bad_magic = encoded;
  bad_magic[0] ^= 1;
  CHECK(NetPlay::DecodeRollbackStateDigest(bad_magic).status == DecodeStatus::WrongMagic);
  auto bad_version = encoded;
  bad_version[5] = 2;
  CHECK(NetPlay::DecodeRollbackStateDigest(bad_version).status ==
        DecodeStatus::UnsupportedVersion);
  auto bad_flags = encoded;
  bad_flags[7] = 1;
  CHECK(NetPlay::DecodeRollbackStateDigest(bad_flags).status == DecodeStatus::UnsupportedFlags);
  auto bad_generation = encoded;
  for (std::size_t i = 8; i < 16; ++i)
    bad_generation[i] = 0;
  CHECK(NetPlay::DecodeRollbackStateDigest(bad_generation).status ==
        DecodeStatus::InvalidGeneration);
  auto bad_frame = encoded;
  bad_frame[19] = 61;
  CHECK(NetPlay::DecodeRollbackStateDigest(bad_frame).status ==
        DecodeStatus::InvalidLogicalFrame);

  NetPlay::RollbackStateDigestTracker tracker;
  tracker.Reset({.session_generation = 77, .expected_players = {1, 2, 3}});
  CHECK(tracker.IsActive());
  CHECK(tracker.Submit(1, Digest()).status == SubmitStatus::Accepted);
  CHECK(tracker.Submit(1, Digest()).status == SubmitStatus::Duplicate);
  CHECK(tracker.Submit(2, Digest()).status == SubmitStatus::Accepted);
  CHECK(tracker.Submit(3, Digest()).status == SubmitStatus::Matched);
  CHECK(tracker.GetPendingFrameCount() == 0);
  CHECK(tracker.Submit(1, Digest()).status == SubmitStatus::Stale);

  tracker.Reset({.session_generation = 77, .expected_players = {1, 2, 3}});
  CHECK(tracker.Submit(1, Digest()).status == SubmitStatus::Accepted);
  CHECK(tracker.Submit(2, Digest()).status == SubmitStatus::Accepted);
  const auto mismatch = tracker.Submit(3, Digest(60, 77, 0xdeadbeef));
  CHECK(mismatch.status == SubmitStatus::Mismatch);
  CHECK(mismatch.blamed_player == 3);

  tracker.Reset({.session_generation = 77, .expected_players = {1, 2}});
  CHECK(tracker.Submit(1, Digest(540)).status == SubmitStatus::Future);
  CHECK(tracker.Submit(1, Digest(60, 78)).status == SubmitStatus::BadSession);
  CHECK(tracker.Submit(3, Digest()).status == SubmitStatus::UnexpectedPlayer);
  CHECK(tracker.Submit(1, Digest(61)).status == SubmitStatus::Malformed);
  CHECK(tracker.Submit(1, Digest()).status == SubmitStatus::Accepted);
  CHECK(tracker.Submit(1, Digest(600)).status == SubmitStatus::Future);
  CHECK(tracker.Submit(1, Digest(120)).status == SubmitStatus::Accepted);
  CHECK(tracker.Submit(1, Digest()).status == SubmitStatus::Duplicate);
  // Each reliable sender must report every checkpoint in order. A peer cannot
  // skip frame 60 just because another peer has already reached frame 120.
  CHECK(tracker.Submit(2, Digest(120)).status == SubmitStatus::Future);
  CHECK(tracker.Submit(2, Digest()).status == SubmitStatus::Matched);
  CHECK(tracker.Submit(2, Digest(120)).status == SubmitStatus::Matched);

  tracker.Reset({.session_generation = 77,
                 .expected_players = {1, 2},
                 .max_pending_frames = 2,
                 .max_lead_frames = 600});
  CHECK(tracker.Submit(1, Digest(60)).status == SubmitStatus::Accepted);
  CHECK(tracker.Submit(1, Digest(120)).status == SubmitStatus::Accepted);
  // Do not silently retire an incomplete comparison to make space. Reliable
  // ordered delivery makes a missing periodic report a session fault.
  CHECK(tracker.Submit(1, Digest(180)).status == SubmitStatus::Future);
  CHECK(tracker.GetPendingFrameCount() == 2);
  CHECK(!tracker.GetRetiredThroughFrame());
  CHECK(tracker.Submit(2, Digest(60)).status == SubmitStatus::Matched);

  // A sender cannot ratchet the global highest frame forward by repeatedly
  // taking individually legal lead-window steps.
  tracker.Reset({.session_generation = 77,
                 .expected_players = {1, 2},
                 .max_pending_frames = 8,
                 .max_lead_frames = 120});
  CHECK(tracker.Submit(1, Digest(60)).status == SubmitStatus::Accepted);
  CHECK(tracker.Submit(1, Digest(120)).status == SubmitStatus::Accepted);
  CHECK(tracker.Submit(1, Digest(180)).status == SubmitStatus::Future);

  // Two peers establish disagreement but cannot establish blame. With three,
  // attribution is valid only for a single report against an N-1 consensus.
  tracker.Reset({.session_generation = 77, .expected_players = {1, 2}});
  CHECK(tracker.Submit(1, Digest()).status == SubmitStatus::Accepted);
  const auto two_way = tracker.Submit(2, Digest(60, 77, 0xdeadbeef));
  CHECK(two_way.status == SubmitStatus::Mismatch);
  CHECK(two_way.blamed_player == 0);

  tracker.Reset({.session_generation = 77, .expected_players = {1, 2, 3}});
  CHECK(tracker.Submit(1, Digest()).status == SubmitStatus::Accepted);
  CHECK(tracker.Submit(2, Digest(60, 77, 0x11111111)).status == SubmitStatus::Accepted);
  const auto all_different = tracker.Submit(3, Digest(60, 77, 0x22222222));
  CHECK(all_different.status == SubmitStatus::Mismatch);
  CHECK(all_different.blamed_player == 0);

  NetPlay::RollbackStateDigestCandidates candidates;
  candidates.Reset(77, 2);
  CHECK(candidates.IsActive());
  CHECK(candidates.Capture({.digest = Digest(), .required_confirmed_batch = 9}) ==
        NetPlay::RollbackStateDigestCandidates::CaptureStatus::Captured);
  CHECK(candidates.TakeConfirmed(8, false).empty());
  CHECK(candidates.TakeConfirmed(9, true).empty());
  // A corrected replay of frame 60 replaces the speculative state.
  CHECK(candidates.Capture(
            {.digest = Digest(60, 77, 0xfeedbeef), .required_confirmed_batch = 9}) ==
        NetPlay::RollbackStateDigestCandidates::CaptureStatus::Replaced);
  const auto corrected = candidates.TakeConfirmed(9, false);
  CHECK(corrected.size() == 1);
  CHECK(corrected[0] == Digest(60, 77, 0xfeedbeef));
  CHECK(candidates.TakeConfirmed(9, false).empty());

  // Frames without an SI poll have no batch dependency and can publish once
  // the caller establishes that no replay correction is pending.
  CHECK(candidates.Capture({.digest = Digest(120), .required_confirmed_batch = std::nullopt}) ==
        NetPlay::RollbackStateDigestCandidates::CaptureStatus::Captured);
  const auto no_poll = candidates.TakeConfirmed(std::nullopt, false);
  CHECK(no_poll.size() == 1 && no_poll[0].logical_frame == 120);
  candidates.Reset(77, 2);
  CHECK(candidates.Capture({.digest = Digest(60), .required_confirmed_batch = 10}) ==
        NetPlay::RollbackStateDigestCandidates::CaptureStatus::Captured);
  CHECK(candidates.Capture({.digest = Digest(120), .required_confirmed_batch = 5}) ==
        NetPlay::RollbackStateDigestCandidates::CaptureStatus::Captured);
  CHECK(candidates.TakeConfirmed(5, false).empty());
  CHECK(candidates.Size() == 2);
  candidates.Reset(78, 1);
  CHECK(candidates.Size() == 0);
  CHECK(candidates.Capture({.digest = Digest(60, 77)}) ==
        NetPlay::RollbackStateDigestCandidates::CaptureStatus::InvalidDigest);
  CHECK(candidates.Capture({.digest = Digest(60, 78)}) ==
        NetPlay::RollbackStateDigestCandidates::CaptureStatus::Captured);
  CHECK(candidates.Capture({.digest = Digest(120, 78)}) ==
        NetPlay::RollbackStateDigestCandidates::CaptureStatus::Full);

  NetPlay::RollbackStateDigestTracker inactive;
  inactive.Reset({.session_generation = 0, .expected_players = {1, 2}});
  CHECK(!inactive.IsActive());
  CHECK(inactive.Submit(1, Digest()).status == SubmitStatus::InactiveSession);
  inactive.Reset({.session_generation = 77, .expected_players = {1, 1}});
  CHECK(!inactive.IsActive());

  std::puts("rollback state digest protocol/tracker test passed");
  return 0;
}
