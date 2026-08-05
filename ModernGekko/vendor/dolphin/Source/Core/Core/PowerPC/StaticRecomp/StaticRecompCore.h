// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/RecompDeterminism.h"

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;
}

namespace StaticRecompLockstep
{
class StaticRecompLockstepVerifier;
}

// Executes statically recompiled per-game native code when the PC is covered by
// a loaded module; falls back to Dolphin's interpreter for everything else.
// With no module loaded this core is exactly an interpreter loop.
class StaticRecompCore : public JitBase
{
public:
  friend class StaticRecompLockstep::StaticRecompLockstepVerifier;

  explicit StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source);
  StaticRecompCore(const StaticRecompCore&) = delete;
  StaticRecompCore(StaticRecompCore&&) = delete;
  StaticRecompCore& operator=(const StaticRecompCore&) = delete;
  StaticRecompCore& operator=(StaticRecompCore&&) = delete;
  ~StaticRecompCore() override;

  void Init() override;
  void Shutdown() override;

  void Run() override;
  void SingleStep() override;
  bool IsModuleActive() const;
  bool DispatchableAt(u32 address);
  bool FastDispatchableAt(u32 address) const;

  void ClearCache() override;
  void Jit(u32 em_address) override {}
  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }
  void EraseSingleBlock(const JitBlock& block) override {}
  std::vector<MemoryStats> GetMemoryStats() const override { return {}; }
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }
  const char* GetName() const override { return "StaticRecomp"; }

private:
  // JitBaseBlockCache with no generated blocks; exists so generic
  // icache-invalidation plumbing in JitInterface has a real object to talk
  // to, and to feed every invalidation into the SMC demotion guard (D4).
  class EmptyBlockCache : public JitBaseBlockCache
  {
  public:
    explicit EmptyBlockCache(StaticRecompCore& core) : JitBaseBlockCache(core), m_core(core) {}
    void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override {}

  protected:
    void InvalidateICacheInternal(u32 physical_address, u32 address, u32 length,
                                  bool forced) override
    {
      m_core.OnICacheInvalidate(address, length);
    }

  private:
    StaticRecompCore& m_core;
  };

  void LoadModule();

  // D4 SMC guard, verify-on-entry model. Every chunk starts Unverified; the
  // first native dispatch into it hashes its guest RAM against the module's
  // recorded hash of the original text. An icache invalidation touching a
  // chunk resets it to Unverified (Dolphin invalidates while *loading* code,
  // so invalidation alone must not retire coverage); a hash mismatch (real
  // SMC) marks it Failed, interpreter-only until the next invalidation.
  enum ChunkState : u8
  {
    CHUNK_UNVERIFIED = 0,
    CHUNK_VERIFIED = 1,
    CHUNK_FAILED = 2,
  };

  void OnICacheInvalidate(u32 address, u32 length);
  int ChunkIndexOf(u32 address) const;
  void VerifyChunk(u32 index);

  static void SetPPCStateFromGuestState(const CPUState& s, PowerPC::PowerPCState& ppc);

  // D1 state residency: registers live in m_guest while native code runs;
  // full sync at every native-burst boundary.
  void SyncIn();   // Dolphin PowerPCState -> m_guest
  void SyncOut();  // m_guest -> Dolphin PowerPCState

  // FMV HLE: called when the guest invokes mwPlyStartAfs (a Sofdec movie
  // start). fno is the AFS file number = which movie in movie.afs.
  void OnFmvStartAfs(u32 fno, u32 patid, u32 handle);
  void OnFmvExecObserve(u32 handle);
  void OnFmvGetFrm(u32 handle, u32 desc);
  void OnFmvCnvFrm(u32 handle, u32 desc, u32 dst);
  u32 GuestRead32(u32 addr) const;
  void GuestWrite32(u32 addr, u32 value);

  // CPUState hooks (module -> chassis environment). `cpu->external_user_data`
  // is the StaticRecompCore*.
  static u64 HookExternalRead(CPUState* cpu, u32 ea, u8 size);
  static void HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size);
  static u32 HookExternalRead32(CPUState* cpu, u32 ea, u8 rid);
  static void HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid);
  static void* HookExternalPointer(CPUState* cpu, u32 ea, u32 size);
  static void HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia);

  // Keep Dolphin's MSR-derived state (translation mode, feature flags) in step
  // with the guest MSR before any MMU access or exception delivery.
  void PropagateGuestMSR();

  // Determinism write watch (RINGOUT_DETERMINISM_WATCH). Installs the module's
  // write journal permanently and reports the dispatching block whenever a
  // store lands in the watched range, turning a diverging address into the
  // function that wrote it. Covers native stores only: a silent watch means the
  // write did not come from recompiled guest code, which is itself the answer.
  void InitDeterminismWatch();
  static void DeterminismWatchTrampoline(u32 offset, u32 size, void* user);

  // Polls the watched word and reports a change, naming where it was noticed.
  // The cached compare keeps this to a load and a branch, so it can sit on the
  // per-dispatch path; only an actual change calls out.
  void PollDeterminismWatch(const char* site, u32 pc)
  {
    const u32 value = GuestRead32(0x80000000u + m_watch_offset);
    if (value == m_watch_last) [[likely]]
      return;
    m_watch_last = value;
    RecompDeterminism::PollWatch(site, pc, value);
  }

  std::unique_ptr<StaticRecompLockstep::StaticRecompLockstepVerifier> m_lockstep_verifier;

  EmptyBlockCache m_block_cache{*this};

  CPUState m_guest{};
  Common::DynamicLibrary m_library;
  StaticRecompModuleSource m_module_source;
  const StaticRecompModuleDesc* m_module = nullptr;
  bool m_module_active = false;
  std::unique_ptr<JitBase> m_fallback_jit;
  // Whether the fallback JIT has run since its cache was last cleared. See
  // ClearCache: clearing an already-empty Jit64 code space still costs 14-16 ms.
  bool m_fallback_jit_used = false;

  u64 m_native_dispatches = 0;
  u64 m_fallback_steps = 0;
  u64 m_native_exceptions = 0;
  u64 m_hook_fallback_instructions = 0;
  u64 m_bursts = 0;          // SyncIn..SyncOut native runs (diagnostic)
  u64 m_charged_cycles = 0;  // cycles flushed from module charges (diagnostic)
  // Carried CPU-cycle remainder for advancing the guest Time Base at the
  // hardware rate (CPU clock / TIMER_RATIO). Reset each SyncIn.
  u64 m_tb_cycle_remainder = 0;

  // D4 guard state: parallel to m_module->chunk_ranges.
  std::vector<u8> m_chunk_state;
  u32 m_failed_chunks = 0;    // chunks currently failing verification (real SMC)
  u64 m_verifications = 0;    // chunk hash checks performed
  u64 m_reverify_events = 0;  // invalidations that reset a chunk to Unverified

  // Lookup table optimization for O(1) chunk searches
  std::vector<int> m_chunk_lookup_table;
  u32 m_lookup_ram_size = 0;
  u32 m_lookup_exram_size = 0;
  int GetAddressLookupIndex(u32 address) const;
  void InitLookupTable(u32 ram_size, u32 exram_size);

  // Dispatch locality: most control transfers stay inside one chunk, so the
  // last hit short-circuits the chunk binary search on the hot path.
  mutable u32 m_last_chunk_index = 0;

  u32 m_idle_pc = 0;

  // GQR quantisation survey (STATICRECOMP_GQRLOG=1). Records which values the
  // eight GQRs actually hold over a run, sampled per burst -- they change rarely
  // and a map lookup has no business on the per-dispatch path.
  //
  // READ THE OUTPUT CAREFULLY: this is what the registers CONTAIN, not how often
  // each setting is USED. The two diverge badly here, and the difference already
  // sent one optimisation down the wrong path. This survey reports GQR0 as
  // 0x00000000 for 100% of samples, and 93% of the 4555 psq call sites name
  // GQR0 -- from which a `gqr == 0` fast path looks obviously right. Counting
  // actual executions inside the module said otherwise: gqr==0 is 49.5% of
  // executed loads and only 15.3% of stores, because GQR4 is 3.5% of sites but
  // 58.7% of store executions. A register that is usually zero is not the same
  // as a hot path that is usually zero.
  //
  // So this answers "what settings does the game use", cheaply and without a
  // module rebuild. For "which settings does the hot code hit", instrument
  // ppc_psq_load/store directly instead; see work/perf-fmv/psq_dynamic_counts.log.
  void SampleGQRs();
  void ReportGQRSurvey() const;
  std::map<u32, u64> m_gqr_seen[8];
  u64 m_gqr_samples = 0;
  bool m_gqr_log = false;

  // Determinism write watch state. m_watch_block_* is refreshed before each
  // native dispatch so the journal callback -- which is handed nothing but an
  // offset -- can attribute the store to a block.
  bool m_watch_armed = false;
  u32 m_watch_offset = 0;  // RAM byte offset of the watched range
  u32 m_watch_size = 0;
  u32 m_watch_block_pc = 0;
  u32 m_watch_block_lr = 0;
  u32 m_watch_last = 0;  // last polled value, so the poll costs a compare
};

extern StaticRecompCore* g_static_recomp_core;

// True when a module is loaded/active and the given guest PC is covered by it.
// Used by the fallback JIT dispatcher to hand covered PCs back to the module
// instead of JIT-compiling them. Safe to call with no core/module (returns false).
bool StaticRecompCoveredAt(u32 pc);
