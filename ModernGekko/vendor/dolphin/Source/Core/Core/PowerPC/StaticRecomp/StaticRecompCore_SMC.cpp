// RecompCore: StaticRecomp CPU core - SMC and chunk validation.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/JitInterface.h"
#include "Common/Logging/Log.h"
#include <algorithm>
#include <cstdio>

int StaticRecompCore::GetAddressLookupIndex(u32 address) const
{
  if (address >= 0x80000000u && address < 0x80000000u + m_lookup_ram_size)
    return static_cast<int>((address - 0x80000000u) >> 2);
  if (address >= 0x90000000u && address < 0x90000000u + m_lookup_exram_size)
    return static_cast<int>((m_lookup_ram_size >> 2) + ((address - 0x90000000u) >> 2));
  return -1;
}

void StaticRecompCore::InitLookupTable(u32 ram_size, u32 exram_size)
{
  if (m_lookup_ram_size == ram_size && m_lookup_exram_size == exram_size)
    return;

  m_lookup_ram_size = ram_size;
  m_lookup_exram_size = exram_size;

  if (!m_module)
  {
    m_chunk_lookup_table.clear();
    // Or the dispatcher keeps answering from a bitmap for a module that is
    // gone. FastDispatchableAt no longer consults m_chunk_lookup_table, so
    // clearing that alone is not enough any more.
    m_fast_entry_bits.clear();
    m_fast_entry_count = 0;
    return;
  }

  u32 total_instructions = (ram_size + exram_size) >> 2;
  m_chunk_lookup_table.assign(total_instructions, -1);

  // Entry points (ABI v3). Read only for a v3 descriptor -- on a v2 one these
  // fields are past the end of the struct it allocated.
  m_entry_bitmap.clear();
  if (m_module->abi_version >= 3u && m_module->entry_points != nullptr &&
      m_module->num_entry_points != 0)
  {
    m_entry_bitmap.assign(total_instructions, false);
    for (u32 i = 0; i < m_module->num_entry_points; ++i)
    {
      const int idx = GetAddressLookupIndex(m_module->entry_points[i]);
      if (idx >= 0 && idx < static_cast<int>(m_entry_bitmap.size()))
        m_entry_bitmap[idx] = true;
    }
  }

  for (u32 i = 0; i < m_module->num_chunk_ranges; ++i)
  {
    const auto& chunk = m_module->chunk_ranges[i];
    int start_idx = GetAddressLookupIndex(chunk.start);
    int end_idx = GetAddressLookupIndex(chunk.end);

    if (start_idx >= 0 && end_idx >= start_idx)
    {
      for (int idx = start_idx; idx < end_idx; ++idx)
      {
        m_chunk_lookup_table[idx] = static_cast<int>(i);
      }
    }
  }

  // Fold the three arrays into the dispatcher's bit array. Last, because it
  // reads all of them.
  RebuildFastEntryBits();
}

int StaticRecompCore::ChunkIndexOf(u32 address) const
{
  if (!m_module_active || m_chunk_lookup_table.empty())
    return -1;

  int idx = GetAddressLookupIndex(address);
  if (idx < 0 || idx >= static_cast<int>(m_chunk_lookup_table.size()))
    return -1;

  return m_chunk_lookup_table[idx];
}

// Being inside a verified chunk is necessary but not sufficient once the module
// declares its entry points: a guest jump-table target or a mid-block rfi resume
// lands in range yet has no case in the entry switch, and dispatching there
// returns without executing anything.
//
// BOTH predicates must ask this. DispatchableAt is the gate that chooses the
// native path at all, and the dispatch inside it is unconditional -- the do/while
// body runs before its condition is tested. A gate that says yes at an address
// the loop condition then rejects dispatches there anyway, makes no progress,
// falls out, and is re-entered at the same pc: a live spin at billions of
// dispatches per run with zero frames and zero fallbacks. It is also what lets
// the interpreter loop below stop at an address the module can actually enter.
bool StaticRecompCore::IsModuleEntry(u32 address) const
{
  if (m_entry_bitmap.empty())
    return true;  // Pre-v3 module: every in-range address is an entry.
  const int idx = GetAddressLookupIndex(address);
  return idx >= 0 && idx < static_cast<int>(m_entry_bitmap.size()) && m_entry_bitmap[idx];
}

// Hot: called once per dispatch, tens of millions of times a run. Written out
// rather than composed from ChunkIndexOf + IsModuleEntry because those two each
// call GetAddressLookupIndex, so the composed version derived the SAME index
// from the SAME address twice per dispatch. Semantics are identical -- the entry
// bitmap is indexed like the chunk lookup table, which is what makes the single
// index valid for both -- and the profile that motivated this had
// FastDispatchableAt at 2.32% of the CPU thread.
bool StaticRecompCore::FastDispatchableAt(u32 address) const
{
  // ONE load from a 2.75 MB bit array, where this used to walk three arrays --
  // an 88 MB vector<int>, the chunk state, and a bool bitmap -- at a random
  // index per dispatch. The answer is identical; see RebuildFastEntryChunk.
  if (!m_module_active)
    return false;

  const int idx = GetAddressLookupIndex(address);
  if (idx < 0 || static_cast<std::size_t>(idx) >= m_fast_entry_count)
    return false;

  return (m_fast_entry_bits[static_cast<std::size_t>(idx) >> 6] >>
          (static_cast<unsigned>(idx) & 63u)) &
         1u;
}

// The AND of the three arrays, for one chunk's address range. Called on every
// chunk state transition -- there are only four, and a session sees a couple of
// hundred of them against tens of millions of dispatches.
void StaticRecompCore::RebuildFastEntryChunk(u32 chunk_index)
{
  if (m_fast_entry_bits.empty() || m_module == nullptr ||
      chunk_index >= m_module->num_chunk_ranges)
    return;

  const auto& chunk = m_module->chunk_ranges[chunk_index];
  const int start_idx = GetAddressLookupIndex(chunk.start);
  const int end_idx = GetAddressLookupIndex(chunk.end);
  if (start_idx < 0 || end_idx < start_idx)
    return;

  const bool verified = m_chunk_state[chunk_index] == CHUNK_VERIFIED;
  // A pre-v3 module ships no entry-point list, and every in-range address is an
  // entry -- the same fallback the three-array form had.
  const bool all_entries = m_entry_bitmap.empty();

  for (int idx = start_idx; idx < end_idx; ++idx)
  {
    if (static_cast<std::size_t>(idx) >= m_fast_entry_count)
      break;
    // Only claim slots this chunk actually owns: ranges can overlap in a
    // malformed module, and claiming another chunk's slot would dispatch into
    // the wrong code.
    if (static_cast<std::size_t>(idx) < m_chunk_lookup_table.size() &&
        m_chunk_lookup_table[idx] != static_cast<int>(chunk_index))
      continue;

    const bool entry =
        verified && (all_entries || (static_cast<std::size_t>(idx) < m_entry_bitmap.size() &&
                                     m_entry_bitmap[idx]));
    const std::size_t word = static_cast<std::size_t>(idx) >> 6;
    const u64 bit = 1ull << (static_cast<unsigned>(idx) & 63u);
    if (entry)
      m_fast_entry_bits[word] |= bit;
    else
      m_fast_entry_bits[word] &= ~bit;
  }
}

void StaticRecompCore::RebuildFastEntryBits()
{
  m_fast_entry_count = m_chunk_lookup_table.size();
  m_fast_entry_bits.assign((m_fast_entry_count + 63u) / 64u, 0ull);
  if (m_module == nullptr)
    return;
  for (u32 i = 0; i < m_module->num_chunk_ranges; ++i)
    RebuildFastEntryChunk(i);
}

bool StaticRecompCore::DispatchableAt(u32 address)
{
  const int index = ChunkIndexOf(address);
  if (index < 0)
    return false;
  if (m_chunk_state[index] == CHUNK_UNVERIFIED)
    VerifyChunk(static_cast<u32>(index));
  return m_chunk_state[index] == CHUNK_VERIFIED && IsModuleEntry(address);
}

void StaticRecompCore::VerifyChunk(u32 index)
{
  const auto& chunk = m_module->chunk_ranges[index];
  auto& memory = m_system.GetMemory();
  const u32 ram_size = memory.GetRamSizeReal();
  const u32 offset = chunk.start - 0x80000000u;
  const u32 length = chunk.end - chunk.start;
  ++m_verifications;

  if (chunk.start < 0x80000000u || offset >= ram_size || length > ram_size - offset)
  {
    m_chunk_state[index] = CHUNK_FAILED;
    RebuildFastEntryChunk(index);
    ++m_failed_chunks;
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: chunk [0x{:08X},0x{:08X}) outside guest RAM",
                  chunk.start, chunk.end);
    return;
  }

  // FNV-1a 64, matching gen_module_tables.py.
  const u8* bytes = memory.GetRAM() + offset;
  u64 hash = 0xCBF29CE484222325ull;
  for (u32 i = 0; i < length; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }

  if (hash == m_module->chunk_hashes[index])
  {
    m_chunk_state[index] = CHUNK_VERIFIED;
    RebuildFastEntryChunk(index);
  }
  else
  {
    m_chunk_state[index] = CHUNK_FAILED;
    RebuildFastEntryChunk(index);
    ++m_failed_chunks;
    std::fprintf(stderr,
                 "[staticrecomp] SMC: chunk [0x%08X,0x%08X) hash mismatch; interpreter until "
                 "next invalidation (%u failed)\n",
                 chunk.start, chunk.end, m_failed_chunks);
    WARN_LOG_FMT(POWERPC,
                 "StaticRecomp: chunk [0x{:08X},0x{:08X}) failed verification (guest code "
                 "differs from module); interpreter until next invalidation",
                 chunk.start, chunk.end);
  }
}

void StaticRecompCore::OnICacheInvalidate(u32 address, u32 length)
{
  if (m_fallback_jit)
  {
    m_fallback_jit->GetBlockCache()->InvalidateICache(address, length, false);
  }

  if (!m_module_active || length == 0)
    return;
  const u32 last = address + (length - 1u);

  // Binary search to find the first chunk index that could possibly overlap (chunk.end > address)
  u32 lo = 0;
  u32 hi = m_module->num_chunk_ranges;
  while (lo < hi)
  {
    const u32 mid = lo + (hi - lo) / 2;
    if (m_module->chunk_ranges[mid].end <= address)
      lo = mid + 1;
    else
      hi = mid;
  }

  for (u32 i = lo; i < m_module->num_chunk_ranges; ++i)
  {
    const auto& chunk = m_module->chunk_ranges[i];
    if (chunk.start > last)
      break;

    if (m_chunk_state[i] != CHUNK_UNVERIFIED)
    {
      if (m_chunk_state[i] == CHUNK_FAILED)
        --m_failed_chunks;
      m_chunk_state[i] = CHUNK_UNVERIFIED;
      RebuildFastEntryChunk(i);
      ++m_reverify_events;
    }
  }
}
