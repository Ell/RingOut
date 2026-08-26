// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/FrameDispatchProfiler.h"

#include <algorithm>
#include <limits>

namespace PowerPC
{

FrameDispatchProfiler::FrameDispatchProfiler(const std::uint64_t warmup_frames,
                                             const std::uint64_t maximum_frames)
    : m_warmup_frames(warmup_frames), m_maximum_frames(maximum_frames)
{
}

void FrameDispatchProfiler::RecordDispatch(const std::uint32_t pc)
{
  if (IsComplete())
    return;
  const std::uint64_t ordinal = m_current_dispatch_ordinal++;
  auto found = m_current_hits.find(pc);
  if (found == m_current_hits.end())
  {
    if (m_current_hits.size() >= MAX_TRACKED_PCS)
      return;
    found = m_current_hits.emplace(pc, FrameHit{}).first;
  }
  auto& hit = found->second;
  if (hit.count == 0)
    hit.first_ordinal = ordinal;
  hit.last_ordinal = ordinal;
  if (hit.count != std::numeric_limits<std::uint32_t>::max())
    ++hit.count;
}

void FrameDispatchProfiler::EndVideoFrame()
{
  ++m_observed_frames;
  if (m_observed_frames <= m_warmup_frames)
  {
    m_current_hits.clear();
    m_current_dispatch_ordinal = 0;
    return;
  }
  if (IsComplete())
  {
    m_current_hits.clear();
    m_current_dispatch_ordinal = 0;
    return;
  }

  ++m_profiled_frames;
  for (const auto& [pc, hit] : m_current_hits)
  {
    auto found = m_candidates.find(pc);
    if (found == m_candidates.end())
    {
      if (m_candidates.size() >= MAX_TRACKED_PCS)
        continue;
      found = m_candidates.emplace(pc, Candidate{}).first;
    }
    Candidate& candidate = found->second;
    candidate.pc = pc;
    ++candidate.frames_with_hits;
    candidate.exactly_once_frames += hit.count == 1 ? 1 : 0;
    candidate.total_hits += hit.count;
    candidate.min_hits =
        candidate.min_hits == 0 ? hit.count : std::min(candidate.min_hits, hit.count);
    candidate.max_hits = std::max(candidate.max_hits, hit.count);
    if (candidate.frames_with_hits == 1)
    {
      candidate.first_ordinal_min = candidate.first_ordinal_max = hit.first_ordinal;
      candidate.last_ordinal_min = candidate.last_ordinal_max = hit.last_ordinal;
    }
    else
    {
      candidate.first_ordinal_min = std::min(candidate.first_ordinal_min, hit.first_ordinal);
      candidate.first_ordinal_max = std::max(candidate.first_ordinal_max, hit.first_ordinal);
      candidate.last_ordinal_min = std::min(candidate.last_ordinal_min, hit.last_ordinal);
      candidate.last_ordinal_max = std::max(candidate.last_ordinal_max, hit.last_ordinal);
    }
  }
  m_current_hits.clear();
  m_current_dispatch_ordinal = 0;
}

std::vector<FrameDispatchProfiler::Candidate>
FrameDispatchProfiler::GetCandidates(const bool require_every_frame) const
{
  std::vector<Candidate> result;
  result.reserve(m_candidates.size());
  for (const auto& [pc, candidate] : m_candidates)
  {
    if (candidate.exactly_once_frames == 0)
      continue;
    if (require_every_frame && (candidate.frames_with_hits != m_profiled_frames ||
                                candidate.exactly_once_frames != m_profiled_frames))
    {
      continue;
    }
    result.push_back(candidate);
  }
  std::sort(result.begin(), result.end(), [](const Candidate& lhs, const Candidate& rhs) {
    if (lhs.exactly_once_frames != rhs.exactly_once_frames)
      return lhs.exactly_once_frames > rhs.exactly_once_frames;
    if (lhs.frames_with_hits != rhs.frames_with_hits)
      return lhs.frames_with_hits > rhs.frames_with_hits;
    return lhs.pc < rhs.pc;
  });
  return result;
}

}  // namespace PowerPC
