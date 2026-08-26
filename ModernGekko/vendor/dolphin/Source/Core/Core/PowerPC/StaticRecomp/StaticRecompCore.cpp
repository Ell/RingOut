// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>

#include <cstdio>
#include <cstring>

#include "Common/Config/Config.h"
#include "Common/DynamicLibrary.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/PowerPC/StaticRecomp/FrameDispatchProfiler.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/RecompDeterminism.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif
#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif

StaticRecompCore* g_static_recomp_core = nullptr;

bool StaticRecompCoveredAt(u32 pc)
{
  return g_static_recomp_core && g_static_recomp_core->IsModuleActive() &&
         g_static_recomp_core->DispatchableAt(pc);
}

void StaticRecompVideoFrameBoundary()
{
  if (g_static_recomp_core)
    g_static_recomp_core->NotifyVideoFrameBoundary();
}

bool InstallStaticRecompDispatchHook(StaticRecompDispatchHook* const hook)
{
  return g_static_recomp_core && g_static_recomp_core->InstallDispatchHook(hook);
}

void UninstallStaticRecompDispatchHook(StaticRecompDispatchHook* const hook)
{
  if (g_static_recomp_core)
    g_static_recomp_core->UninstallDispatchHook(hook);
}

namespace
{
u64 ReadBoundedFrameCount(const char* const name, const u64 fallback, const u64 minimum,
                          const u64 maximum)
{
  const char* const text = std::getenv(name);
  if (!text)
    return fallback;

  char* end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum)
  {
    std::fprintf(stderr, "[sc2-hook-profile] ignored invalid %s=%s (allowed %llu..%llu)\n", name,
                 text, static_cast<unsigned long long>(minimum),
                 static_cast<unsigned long long>(maximum));
    return fallback;
  }
  return static_cast<u64>(parsed);
}

bool RangesAreSorted(const StaticRecompRange* ranges, u32 count)
{
  if (!ranges || count == 0)
    return false;
  for (u32 i = 0; i < count; ++i)
  {
    if (ranges[i].start >= ranges[i].end || (i != 0 && ranges[i - 1].end > ranges[i].start))
      return false;
  }
  return true;
}

bool AddressIsCovered(const StaticRecompRange* ranges, u32 count, u32 address)
{
  for (u32 i = 0; i < count; ++i)
  {
    if (address >= ranges[i].start && address < ranges[i].end)
      return true;
  }
  return false;
}

bool ChunksTileCode(const StaticRecompModuleDesc& desc)
{
  if (!RangesAreSorted(desc.chunk_ranges, desc.num_chunk_ranges) || !desc.chunk_hashes)
    return false;
  u32 chunk = 0;
  for (u32 code = 0; code < desc.num_code_ranges; ++code)
  {
    u32 cursor = desc.code_ranges[code].start;
    while (chunk < desc.num_chunk_ranges &&
           desc.chunk_ranges[chunk].start < desc.code_ranges[code].end)
    {
      if (desc.chunk_ranges[chunk].start != cursor ||
          desc.chunk_ranges[chunk].end > desc.code_ranges[code].end)
        return false;
      cursor = desc.chunk_ranges[chunk++].end;
    }
    if (cursor != desc.code_ranges[code].end)
      return false;
  }
  return chunk == desc.num_chunk_ranges;
}
}  // namespace

bool StaticRecompCore::IsModuleActive() const
{
  return m_module_active;
}

StaticRecompCore::StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source)
    : JitBase(system), m_module_source(std::move(module_source))
{
}

StaticRecompCore::~StaticRecompCore() = default;

// GX command and vertex traffic reaches the host as ordinary guest stores to the
// write-gather pipe page, and every one of them crossed the .so boundary into
// HookExternalWrite to move a few bytes into a buffer. On a draw-call-heavy
// scene that is the busiest thing the chassis does.
//
// Hand the module the buffer instead. It writes the bytes inline and calls back
// only when the pipe fills: once per 32 bytes rather than once per word. This is
// what Dolphin's own JITs do with optimizeGatherPipe.
//
// Installed through an optional export rather than a CPUState field, so no ABI
// bump and no coupling of module and runtime deployment (the same reasoning as
// the lazy-FPRF note in the module's cpu.h). A module without the symbol keeps
// the old path and still works with this runtime.
void StaticRecompCore::InstallGatherPipeFastPath()
{
  if (!m_module)
    return;
  const auto set_gather_pipe =
      reinterpret_cast<SetGatherPipeFn>(m_library.GetSymbolAddress("ppc_set_gather_pipe"));
  if (!set_gather_pipe)
  {
    std::fprintf(stderr, "[staticrecomp] module lacks ppc_set_gather_pipe export; gather-pipe "
                         "stores stay on the external-write hook.\n");
    return;
  }

  auto& ppc_state = m_system.GetPPCState();
  // Addresses, not values: GPFifoManager::Init() may not have run yet, so the
  // module re-reads both every time rather than snapshotting a NULL here.
  //
  // m_ls_journaling is the stand-down flag. While the lockstep verifier is
  // journaling it needs every MMIO write recorded by the hook, so the module
  // must route these stores back through HookExternalWrite instead of servicing
  // them silently. bool is one byte, hence the unsigned char view.
  set_gather_pipe(&ppc_state.gather_pipe_ptr, &ppc_state.gather_pipe_base_ptr,
                  &StaticRecompCore::GatherPipeFlushTrampoline, this,
                  reinterpret_cast<const unsigned char*>(&m_lockstep_verifier->m_ls_journaling));
}

// Called by the module only when the pipe has reached its fill mark.
// FastCheckGatherPipe re-checks the count and flushes; it is deliberately the
// Fast variant, for the reason spelled out in HookExternalWrite.
void StaticRecompCore::GatherPipeFlushTrampoline(void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  core->m_system.GetGPFifo().FastCheckGatherPipe();
}

void StaticRecompCore::Init()
{
  g_static_recomp_core = this;
  RefreshConfig();
  jo.enableBlocklink = false;
  jo.fastmem = false;
  jo.fastmem_arena = false;

  m_block_cache.Init();

  m_guest = CPUState{};
  m_guest.external_read = HookExternalRead;
  m_guest.external_write = HookExternalWrite;
  m_guest.external_read32 = HookExternalRead32;
  m_guest.external_write32 = HookExternalWrite32;
  m_guest.instruction_fallback = HookInstructionFallback;
  m_guest.host_call = nullptr;
  m_guest.external_user_data = this;

  std::fprintf(stderr, "[staticrecomp] core init\n");

  LoadModule();
  m_idle_pc = Config::Get(Config::MAIN_STATICRECOMP_IDLE_PC);
  // Env override for experimenting with idle-loop skipping: when the guest sits
  // in an OS scheduler spin loop waiting for an interrupt, CoreTiming::Idle()
  // fast-forwards to the next event instead of burning real wall-time spinning.
  if (const char* e = std::getenv("STATICRECOMP_IDLE_PC"))
    m_idle_pc = static_cast<u32>(std::strtoul(e, nullptr, 0));
  m_lockstep_verifier = std::make_unique<StaticRecompLockstep::StaticRecompLockstepVerifier>(*this);
  m_lockstep_verifier->Init();
  InstallGatherPipeFastPath();
  InitDeterminismWatch();
  m_gqr_log = std::getenv("STATICRECOMP_GQRLOG") != nullptr;
  if (std::getenv("RINGOUT_SC2_HOOK_PROFILE"))
  {
    const u64 warmup_frames = ReadBoundedFrameCount(
        "RINGOUT_SC2_HOOK_PROFILE_WARMUP_FRAMES", 120, 0, 36000);
    const u64 sample_frames = ReadBoundedFrameCount(
        "RINGOUT_SC2_HOOK_PROFILE_SAMPLE_FRAMES", 600, 60, 3600);
    m_frame_dispatch_diagnostic_limit = ReadBoundedFrameCount(
        "RINGOUT_SC2_HOOK_PROFILE_DIAGNOSTIC_LIMIT", 0, 0, 4096);
    m_frame_dispatch_profiler =
        std::make_unique<PowerPC::FrameDispatchProfiler>(warmup_frames, sample_frames);
    if (const char* const arm_file = std::getenv("RINGOUT_SC2_HOOK_PROFILE_ARM_FILE");
        arm_file && *arm_file)
    {
      m_frame_dispatch_profile_arm_file = arm_file;
    }
    else
    {
      m_frame_dispatch_profile_armed = true;
    }
    std::fprintf(stderr, "[sc2-hook-profile] enabled expected_dol_sha256="
                         "0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5 "
                         "warmup_frames=%llu sample_frames=%llu diagnostic_limit=%llu "
                         "arm=%s\n",
                 static_cast<unsigned long long>(warmup_frames),
                 static_cast<unsigned long long>(sample_frames),
                 static_cast<unsigned long long>(m_frame_dispatch_diagnostic_limit),
                 m_frame_dispatch_profile_armed ? "immediate" : "file");
    if (std::getenv("RINGOUT_SC2_MEMORY_PROFILE"))
    {
      m_sc2_memory_profile_target_ticks =
          ReadBoundedFrameCount("RINGOUT_SC2_MEMORY_PROFILE_TICKS", 60, 1, 600);
      m_sc2_memory_profile_enabled = true;
      std::fprintf(stderr,
                   "[sc2-memory-profile] enabled begin_pc=0x8001ba3c "
                   "return_pc=0x8002d628 page_bytes=4096 target_ticks=%llu\n",
                   static_cast<unsigned long long>(m_sc2_memory_profile_target_ticks));
    }
    if (const char* const replay_probe = std::getenv("RINGOUT_SC2_ENGINE_REPLAY_PROBE");
        replay_probe && (std::strcmp(replay_probe, "FULL_MEM1_ONE_TICK") == 0 ||
                         std::strcmp(replay_probe, "FULL_EMULATOR_ONE_TICK") == 0))
    {
      m_sc2_engine_replay_probe_enabled = true;
      m_sc2_engine_replay_full_emulator =
          std::strcmp(replay_probe, "FULL_EMULATOR_ONE_TICK") == 0;
      std::fprintf(stderr,
                   "[sc2-engine-replay] enabled mode=%s begin_pc=0x8001ba3c "
                   "return_pc=0x8002d628\n",
                   m_sc2_engine_replay_full_emulator ? "full-emulator-one-tick" :
                                                       "full-mem1-one-tick");
    }
  }

  // The "interpreter fallback" is really Dolphin's JIT whenever one exists, so
  // m_fallback_steps stays 0 on a run that fell back constantly -- read
  // m_fallback_jit_used instead. STATICRECOMP_NO_FALLBACK_JIT forces the true
  // interpreter, which is how you tell a module-side bug from a JIT-handoff one.
  if (std::getenv("STATICRECOMP_NO_FALLBACK_JIT") == nullptr)
  {
#ifdef _M_ARM_64
    m_fallback_jit = std::make_unique<JitArm64>(m_system);
#elif defined(_M_X86_64)
    m_fallback_jit = std::make_unique<Jit64>(m_system);
#endif
  }
  if (m_fallback_jit)
  {
    // Core::CpuThread owns the process exception handler and installs it before
    // entering this core's Run().  Installing another one here asserted on
    // Windows and left the two shutdown paths fighting over one global handle.
    m_fallback_jit->Init();
  }
}

void StaticRecompCore::InitDeterminismWatch()
{
  if (!RecompDeterminism::IsWatchArmed())
    return;
  if (!m_module)
  {
    std::fprintf(stderr, "[watch] no module loaded; there are no native stores to watch.\n");
    return;
  }
  // The module has exactly one journal slot, and the lockstep verifier claims
  // and releases it around every checked block. Sharing it would silently drop
  // whichever writes fell outside a lockstep window, so refuse instead.
  if (m_lockstep_verifier->IsEnabled())
  {
    std::fprintf(stderr, "[watch] STATICRECOMP_LOCKSTEP already owns the module's write "
                         "journal; watch DISABLED (run them separately).\n");
    return;
  }
  const auto set_journal = reinterpret_cast<StaticRecompLockstep::SetMemJournalFn>(
      m_library.GetSymbolAddress("ppc_set_mem_write_journal"));
  if (!set_journal)
  {
    std::fprintf(stderr, "[watch] module lacks ppc_set_mem_write_journal export; "
                         "watch DISABLED (rebuild the module).\n");
    return;
  }

  // Cached MEM1 only, which is what the harness's dump covers and therefore the
  // only place an address it reports can be. Rejected loudly rather than left to
  // match nothing, because a silent watch is meant to mean "nobody stored here".
  // Asked of the memory manager, not m_guest: Run() binds m_guest.ram, so its
  // size is still zero this early.
  const u32 address = RecompDeterminism::WatchGuestAddress();
  const u32 size = RecompDeterminism::WatchSize();
  const u32 offset = address - 0x80000000u;
  const u32 ram_size = m_system.GetMemory().GetRamSizeReal();
  if (ram_size == 0 || size > ram_size || offset > ram_size - size)
  {
    std::fprintf(stderr,
                 "[watch] 0x%08X + %u is outside cached MEM1 (0x80000000..0x%08X); "
                 "watch DISABLED.\n",
                 address, size, 0x80000000u + ram_size);
    return;
  }

  m_watch_offset = offset;
  m_watch_size = size;
  m_watch_armed = true;
  // Installed for the whole run, not per block: the write we are hunting for
  // happens once, and we do not know when.
  set_journal(&StaticRecompCore::DeterminismWatchTrampoline, this);
  std::fprintf(stderr, "[watch] armed on 0x%08X (%u bytes, RAM offset 0x%X)\n", address,
               m_watch_size, m_watch_offset);
}

// Called from inside the module's store fast path, before the store commits, so
// this runs on every guest RAM write while the watch is armed -- the range test
// is deliberately the entire body.
void StaticRecompCore::DeterminismWatchTrampoline(u32 offset, u32 size, void* user)
{
  auto* const core = static_cast<StaticRecompCore*>(user);
  if (offset + size <= core->m_watch_offset || offset >= core->m_watch_offset + core->m_watch_size)
    return;

  RecompDeterminism::ReportWatchWrite(core->m_watch_block_pc, core->m_watch_block_lr, size,
                                      core->GuestRead32(0x80000000u + core->m_watch_offset),
                                      core->m_guest.timebase);
}

// A GQR holds two independent settings: the store quantisation in bits 0-15 and
// the load quantisation in bits 16-31, each a type (0 = unquantised f32) plus a
// 6-bit scale exponent. 0x00000000 therefore means "plain float, no scaling"
// both ways, which is the case a specialised fast path would handle.
void StaticRecompCore::SampleGQRs()
{
  ++m_gqr_samples;
  for (int i = 0; i < 8; ++i)
    ++m_gqr_seen[i][m_guest.gqr[i]];
}

void StaticRecompCore::ReportGQRSurvey() const
{
  std::fprintf(stderr, "[gqr] %llu samples over the run\n",
               static_cast<unsigned long long>(m_gqr_samples));
  for (int i = 0; i < 8; ++i)
  {
    std::fprintf(stderr, "[gqr] GQR%d:", i);
    for (const auto& [value, count] : m_gqr_seen[i])
    {
      const double pct = m_gqr_samples ? 100.0 * double(count) / double(m_gqr_samples) : 0.0;
      std::fprintf(stderr, "  0x%08X (ld type=%u scale=%u, st type=%u scale=%u) %.1f%%", value,
                   (value >> 16) & 7u, (value >> 24) & 0x3Fu, value & 7u, (value >> 8) & 0x3Fu,
                   pct);
    }
    std::fprintf(stderr, "\n");
  }
  std::fflush(stderr);
}

void StaticRecompCore::Shutdown()
{
  ReportFrameDispatchProfile();
  m_dispatch_hook = nullptr;
  m_dispatch_hook_pcs.clear();
  if (m_gqr_log)
    ReportGQRSurvey();
  g_static_recomp_core = nullptr;
  std::fprintf(stderr,
               "[staticrecomp] shutdown: native=%llu fallback=%llu native_exc=%llu hook_fb=%llu "
               "smc_failed=%u verifications=%llu reverify_events=%llu\n",
               (unsigned long long)m_native_dispatches, (unsigned long long)m_fallback_steps,
               (unsigned long long)m_native_exceptions,
               (unsigned long long)m_hook_fallback_instructions, m_failed_chunks,
               (unsigned long long)m_verifications, (unsigned long long)m_reverify_events);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: shutdown. native_dispatches={} fallback_steps={} "
                 "native_exceptions={} hook_fallback_instructions={} smc_failed_chunks={} "
                 "verifications={} reverify_events={}",
                 m_native_dispatches, m_fallback_steps, m_native_exceptions,
                 m_hook_fallback_instructions, m_failed_chunks, m_verifications, m_reverify_events);
  if (m_dispatch_loop || m_dispatch_fwd || m_dispatch_cross)
  {
    const double total =
        double(m_dispatch_loop) + double(m_dispatch_fwd) + double(m_dispatch_cross);
    std::fprintf(stderr,
                 "[dispatch] loop-backedge=%llu (%.1f%%) same-chunk-fwd=%llu (%.1f%%) "
                 "cross-chunk=%llu (%.1f%%)\n",
                 (unsigned long long)m_dispatch_loop, 100.0 * m_dispatch_loop / total,
                 (unsigned long long)m_dispatch_fwd, 100.0 * m_dispatch_fwd / total,
                 (unsigned long long)m_dispatch_cross, 100.0 * m_dispatch_cross / total);
  }
  m_lockstep_verifier.reset();
  m_block_cache.Shutdown();
  m_module = nullptr;
  if (m_library.IsOpen())
    m_library.Close();

  if (m_fallback_jit)
  {
    m_fallback_jit->Shutdown();
    m_fallback_jit.reset();
  }
}

bool StaticRecompCore::InstallDispatchHook(StaticRecompDispatchHook* const hook)
{
  if (!hook || m_dispatch_hook)
    return false;
  const std::span<const u32> pcs = hook->GetHookPcs();
  if (pcs.empty() || std::ranges::any_of(pcs, [](const u32 pc) { return pc == 0; }))
    return false;
  m_dispatch_hook_pcs.assign(pcs.begin(), pcs.end());
  std::ranges::sort(m_dispatch_hook_pcs);
  if (std::ranges::adjacent_find(m_dispatch_hook_pcs) != m_dispatch_hook_pcs.end())
  {
    m_dispatch_hook_pcs.clear();
    return false;
  }
  m_dispatch_hook = hook;
  return true;
}

void StaticRecompCore::UninstallDispatchHook(StaticRecompDispatchHook* const hook)
{
  if (m_dispatch_hook != hook)
    return;
  m_dispatch_hook = nullptr;
  m_dispatch_hook_pcs.clear();
}

void StaticRecompCore::NotifyVideoFrameBoundary()
{
  if (m_frame_dispatch_profiler)
  {
    if (!m_frame_dispatch_profile_armed)
    {
      if (!File::Exists(m_frame_dispatch_profile_arm_file))
        return;
      m_frame_dispatch_profile_armed = true;
      std::fprintf(stderr, "[sc2-hook-profile] armed file=%s\n",
                   m_frame_dispatch_profile_arm_file.c_str());
      // The marker is observed at the boundary ending the pre-arm frame. Do
      // not count that empty partial interval as sample frame zero; recording
      // begins immediately after this return and the next boundary closes the
      // first complete profiled frame.
      return;
    }
    m_frame_dispatch_profiler->EndVideoFrame();
    if (m_frame_dispatch_profiler->IsComplete())
      ReportFrameDispatchProfile();
  }
}

void StaticRecompCore::ReportFrameDispatchProfile()
{
  if (!m_frame_dispatch_profiler || m_frame_dispatch_profile_reported)
    return;
  const auto strict = m_frame_dispatch_profiler->GetCandidates();
  const auto diagnostic = m_frame_dispatch_profiler->GetCandidates(false);
  std::fprintf(stderr,
               "[sc2-hook-profile] result observed_frames=%llu profiled_frames=%llu "
               "strict_candidates=%zu diagnostic_candidates=%zu complete=%s\n",
               static_cast<unsigned long long>(m_frame_dispatch_profiler->GetObservedFrames()),
               static_cast<unsigned long long>(m_frame_dispatch_profiler->GetProfiledFrames()),
               strict.size(), diagnostic.size(),
               m_frame_dispatch_profiler->IsComplete() ? "yes" : "no");
  for (const auto& candidate : strict)
  {
    std::fprintf(stderr,
                 "[sc2-hook-profile] candidate pc=0x%08x frames=%llu once=%llu "
                 "hits=%llu min=%u max=%u parity=%llu/%llu first_ordinal=%llu..%llu "
                 "last_ordinal=%llu..%llu caller_lr=0x%08x caller_lr_stable=%s "
                 "predecessor_pc=0x%08x predecessor_stable=%s\n",
                 candidate.pc, static_cast<unsigned long long>(candidate.frames_with_hits),
                 static_cast<unsigned long long>(candidate.exactly_once_frames),
                 static_cast<unsigned long long>(candidate.total_hits), candidate.min_hits,
                 candidate.max_hits,
                 static_cast<unsigned long long>(candidate.even_frames_with_hits),
                 static_cast<unsigned long long>(candidate.odd_frames_with_hits),
                 static_cast<unsigned long long>(candidate.first_ordinal_min),
                 static_cast<unsigned long long>(candidate.first_ordinal_max),
                 static_cast<unsigned long long>(candidate.last_ordinal_min),
                 static_cast<unsigned long long>(candidate.last_ordinal_max), candidate.caller_lr,
                 candidate.caller_lr_stable ? "yes" : "no", candidate.predecessor_pc,
                 candidate.predecessor_pc_stable ? "yes" : "no");
  }
  const std::size_t diagnostic_count =
      std::min<std::size_t>(diagnostic.size(), m_frame_dispatch_diagnostic_limit);
  for (std::size_t i = 0; i < diagnostic_count; ++i)
  {
    const auto& candidate = diagnostic[i];
    std::fprintf(stderr,
                 "[sc2-hook-profile] diagnostic rank=%zu pc=0x%08x frames=%llu once=%llu "
                 "hits=%llu min=%u max=%u parity=%llu/%llu first_ordinal=%llu..%llu "
                 "last_ordinal=%llu..%llu caller_lr=0x%08x caller_lr_stable=%s "
                 "predecessor_pc=0x%08x predecessor_stable=%s\n",
                 i + 1, candidate.pc,
                 static_cast<unsigned long long>(candidate.frames_with_hits),
                 static_cast<unsigned long long>(candidate.exactly_once_frames),
                 static_cast<unsigned long long>(candidate.total_hits), candidate.min_hits,
                 candidate.max_hits,
                 static_cast<unsigned long long>(candidate.even_frames_with_hits),
                 static_cast<unsigned long long>(candidate.odd_frames_with_hits),
                 static_cast<unsigned long long>(candidate.first_ordinal_min),
                 static_cast<unsigned long long>(candidate.first_ordinal_max),
                 static_cast<unsigned long long>(candidate.last_ordinal_min),
                 static_cast<unsigned long long>(candidate.last_ordinal_max), candidate.caller_lr,
                 candidate.caller_lr_stable ? "yes" : "no", candidate.predecessor_pc,
                 candidate.predecessor_pc_stable ? "yes" : "no");
  }
  std::fflush(stderr);
  m_frame_dispatch_profile_reported = true;
}

void StaticRecompCore::ProfileSc2EngineMemory(const u32 pc)
{
  constexpr u32 ENGINE_BEGIN_PC = 0x8001ba3c;
  constexpr u32 ENGINE_RETURN_PC = 0x8002d628;
  constexpr std::size_t PAGE_BYTES = 4096;
  if (!m_sc2_memory_profile_enabled || m_sc2_memory_profile_reported || m_guest.ram == nullptr ||
      m_guest.ram_size == 0)
  {
    return;
  }

  if (pc == ENGINE_BEGIN_PC)
  {
    if (m_sc2_memory_tick_active)
    {
      std::fprintf(stderr,
                   "[sc2-memory-profile] aborted reason=nested-engine-entry ticks=%llu\n",
                   static_cast<unsigned long long>(m_sc2_memory_profile_ticks));
      m_sc2_memory_profile_enabled = false;
      return;
    }
    if (m_sc2_memory_before.size() != m_guest.ram_size)
    {
      m_sc2_memory_before.resize(m_guest.ram_size);
      m_sc2_memory_page_changed_ticks.assign(
          (m_guest.ram_size + PAGE_BYTES - 1) / PAGE_BYTES, 0);
    }
    std::memcpy(m_sc2_memory_before.data(), m_guest.ram, m_guest.ram_size);
    m_sc2_memory_tick_active = true;
    return;
  }

  if (pc != ENGINE_RETURN_PC || !m_sc2_memory_tick_active)
    return;

  for (std::size_t page = 0; page < m_sc2_memory_page_changed_ticks.size(); ++page)
  {
    const std::size_t offset = page * PAGE_BYTES;
    const std::size_t size = std::min(PAGE_BYTES, m_sc2_memory_before.size() - offset);
    if (std::memcmp(m_sc2_memory_before.data() + offset, m_guest.ram + offset, size) != 0)
      ++m_sc2_memory_page_changed_ticks[page];
  }
  m_sc2_memory_tick_active = false;
  ++m_sc2_memory_profile_ticks;
  if (m_sc2_memory_profile_ticks >= m_sc2_memory_profile_target_ticks)
    ReportSc2EngineMemoryProfile();
}

void StaticRecompCore::ReportSc2EngineMemoryProfile()
{
  constexpr std::size_t PAGE_BYTES = 4096;
  if (m_sc2_memory_profile_reported)
    return;

  std::size_t changed_pages = 0;
  std::size_t every_tick_pages = 0;
  for (const u32 changed_ticks : m_sc2_memory_page_changed_ticks)
  {
    changed_pages += changed_ticks != 0 ? 1 : 0;
    every_tick_pages += changed_ticks == m_sc2_memory_profile_ticks ? 1 : 0;
  }
  std::fprintf(stderr,
               "[sc2-memory-profile] result ticks=%llu ram_bytes=%zu page_bytes=%zu "
               "changed_pages=%zu changed_bytes_upper_bound=%zu every_tick_pages=%zu "
               "complete=%s\n",
               static_cast<unsigned long long>(m_sc2_memory_profile_ticks),
               m_sc2_memory_before.size(), PAGE_BYTES, changed_pages,
               changed_pages * PAGE_BYTES, every_tick_pages,
               m_sc2_memory_profile_ticks == m_sc2_memory_profile_target_ticks ? "yes" : "no");

  std::size_t page = 0;
  while (page < m_sc2_memory_page_changed_ticks.size())
  {
    while (page < m_sc2_memory_page_changed_ticks.size() &&
           m_sc2_memory_page_changed_ticks[page] == 0)
    {
      ++page;
    }
    if (page == m_sc2_memory_page_changed_ticks.size())
      break;
    const std::size_t first = page;
    u32 minimum_ticks = m_sc2_memory_page_changed_ticks[page];
    u32 maximum_ticks = minimum_ticks;
    while (page < m_sc2_memory_page_changed_ticks.size() &&
           m_sc2_memory_page_changed_ticks[page] != 0)
    {
      minimum_ticks = std::min(minimum_ticks, m_sc2_memory_page_changed_ticks[page]);
      maximum_ticks = std::max(maximum_ticks, m_sc2_memory_page_changed_ticks[page]);
      ++page;
    }
    const std::size_t offset = first * PAGE_BYTES;
    const std::size_t size =
        std::min((page - first) * PAGE_BYTES, m_sc2_memory_before.size() - offset);
    std::fprintf(stderr,
                 "[sc2-memory-profile] region offset=0x%08zx size=0x%08zx pages=%zu "
                 "changed_ticks=%u..%u\n",
                 offset, size, page - first, minimum_ticks, maximum_ticks);
  }
  std::fflush(stderr);
  m_sc2_memory_profile_reported = true;
}

void StaticRecompCore::ObserveSc2EngineExternalAccess(const bool write, const u32 address,
                                                       const u8 size)
{
  if (!m_sc2_engine_external_profile_active)
    return;

  constexpr std::size_t MAX_SITES = 256;
  auto& sites = write ? m_sc2_engine_external_writes : m_sc2_engine_external_reads;
  u64& total = write ? m_sc2_engine_external_write_count : m_sc2_engine_external_read_count;
  ++total;
  const u64 key = (static_cast<u64>(address) << 8) | size;
  const auto existing = sites.find(key);
  if (existing != sites.end())
  {
    ++existing->second;
  }
  else if (sites.size() < MAX_SITES)
  {
    sites.emplace(key, 1);
  }
  else
  {
    m_sc2_engine_external_profile_overflow = true;
  }
}

void StaticRecompCore::ReportSc2EngineExternalProfile()
{
  if (m_sc2_engine_external_profile_complete)
    return;

  const u64 fallback_delta =
      m_hook_fallback_instructions - m_sc2_engine_external_entry_fallback_count;
  std::fprintf(stderr,
               "[sc2-engine-external] result reads=%llu writes=%llu read_sites=%zu "
               "write_sites=%zu fallback_instructions=%llu overflow=%s complete=%s\n",
               static_cast<unsigned long long>(m_sc2_engine_external_read_count),
               static_cast<unsigned long long>(m_sc2_engine_external_write_count),
               m_sc2_engine_external_reads.size(), m_sc2_engine_external_writes.size(),
               static_cast<unsigned long long>(fallback_delta),
               m_sc2_engine_external_profile_overflow ? "yes" : "no",
               m_sc2_engine_external_profile_overflow ? "no" : "yes");
  const auto report_sites = [](const char* const kind, const std::map<u64, u64>& sites) {
    for (const auto& [key, count] : sites)
    {
      const u32 address = static_cast<u32>(key >> 8);
      const u8 size = static_cast<u8>(key);
      const char* category =
          (address & 0xfffff000u) == 0xcc008000u ? "gather-pipe" :
          (address & 0xffff0000u) == 0xcc000000u ? "mmio" :
          (address & 0xff000000u) == 0xe0000000u ? "locked-cache" : "translated-mmu";
      std::fprintf(stderr,
                   "[sc2-engine-external] %s address=0x%08x size=%u count=%llu category=%s\n",
                   kind, address, size, static_cast<unsigned long long>(count), category);
    }
  };
  report_sites("read", m_sc2_engine_external_reads);
  report_sites("write", m_sc2_engine_external_writes);
  std::fflush(stderr);
  m_sc2_engine_external_profile_complete = true;
}

void StaticRecompCore::ProbeSc2EngineReplay(const u32 pc)
{
  constexpr u32 ENGINE_BEGIN_PC = 0x8001ba3c;
  constexpr u32 ENGINE_RETURN_PC = 0x8002d628;
  if (!m_sc2_engine_replay_probe_enabled || m_sc2_engine_replay_completed ||
      m_guest.ram == nullptr || m_guest.ram_size == 0)
  {
    return;
  }

  auto& memory = m_system.GetMemory();
  u8* const l1 = memory.GetL1Cache();
  const std::size_t l1_size = memory.GetL1CacheSize();
  const auto capture_full_emulator_state = [this](Common::UniqueBuffer<u8>& buffer,
                                                   std::size_t& size) {
    // The static module owns the live registers while this probe runs inside a
    // native burst. Materialize them into Dolphin's PowerPCState before asking
    // the ordinary savestate machinery to serialize the complete machine.
    SyncOut();
    auto& fifo = m_system.GetFifo();
    const bool was_running = Core::GetState(m_system) == Core::State::Running;
    fifo.PauseAndLock();
    const State::ScopedRollbackSnapshot rollback_snapshot_scope;
    size = State::SaveToBuffer(m_system, buffer);
    fifo.RestoreState(was_running);
    return size != 0;
  };
  const auto restore_full_emulator_state = [this](Common::UniqueBuffer<u8>& buffer,
                                                   const std::size_t size) {
    auto& fifo = m_system.GetFifo();
    const bool was_running = Core::GetState(m_system) == Core::State::Running;
    fifo.PauseAndLock();
    const State::ScopedRollbackSnapshot rollback_snapshot_scope;
    const bool loaded = State::LoadFromBuffer(m_system, std::span<u8>{buffer.data(), size});
    fifo.RestoreState(was_running);
    if (loaded)
      SyncIn();
    return loaded;
  };
  if (pc == ENGINE_BEGIN_PC && !m_sc2_engine_replay_have_entry)
  {
    m_sc2_engine_replay_entry_guest = m_guest;
    m_sc2_engine_replay_entry_tb_remainder = m_tb_cycle_remainder;
    if (m_sc2_engine_replay_full_emulator)
    {
      if (!capture_full_emulator_state(m_sc2_engine_replay_entry_state,
                                       m_sc2_engine_replay_entry_state_size))
      {
        std::fprintf(stderr, "[sc2-engine-replay] failed to capture entry emulator state\n");
        m_sc2_engine_replay_completed = true;
        return;
      }
      // SaveToBuffer consumes Dolphin's materialized PowerPCState, but native
      // execution must resume with the exact resident state and sub-TB carry.
      m_guest = m_sc2_engine_replay_entry_guest;
      m_tb_cycle_remainder = m_sc2_engine_replay_entry_tb_remainder;
    }
    else
    {
      m_sc2_engine_replay_entry_ram.assign(m_guest.ram, m_guest.ram + m_guest.ram_size);
      m_sc2_engine_replay_entry_l1.assign(l1, l1 + l1_size);
    }
    m_sc2_engine_replay_have_entry = true;
    if (m_sc2_engine_replay_full_emulator)
    {
      NetPlay::BeginSc2EngineInputCapture();
      m_sc2_engine_external_reads.clear();
      m_sc2_engine_external_writes.clear();
      m_sc2_engine_external_read_count = 0;
      m_sc2_engine_external_write_count = 0;
      m_sc2_engine_external_entry_fallback_count = m_hook_fallback_instructions;
      m_sc2_engine_external_profile_overflow = false;
      m_sc2_engine_external_profile_active = true;
    }
    std::fprintf(stderr,
                 "[sc2-engine-replay] captured ram_bytes=%u l1_bytes=%zu state_bytes=%zu "
                 "lr=0x%08x\n",
                 m_guest.ram_size, l1_size, m_sc2_engine_replay_entry_state_size, m_guest.lr);
    return;
  }

  if (pc != ENGINE_RETURN_PC || !m_sc2_engine_replay_have_entry)
    return;

  if (!m_sc2_engine_replay_replaying)
  {
    if (m_sc2_engine_replay_full_emulator)
    {
      m_sc2_engine_external_profile_active = false;
      ReportSc2EngineExternalProfile();
      // A load intentionally dirties renderer caches. Comparing the original
      // endpoint with a replayed endpoint would therefore compare a no-load
      // path with a post-load path. Discard this first pass and compare two
      // passes which both begin from the same restored entry snapshot.
      m_sc2_engine_replay_input_capture_valid =
          NetPlay::FinishSc2EngineInputCapture(&m_sc2_engine_replay_input_polls);
      if (!m_sc2_engine_replay_input_capture_valid ||
          !restore_full_emulator_state(m_sc2_engine_replay_entry_state,
                                       m_sc2_engine_replay_entry_state_size) ||
          !NetPlay::BeginSc2EngineInputReplay())
      {
        NetPlay::EndSc2EngineInputReplay();
        std::fprintf(stderr,
                     "[sc2-engine-replay] failed to capture input or restore entry state\n");
        m_sc2_engine_replay_completed = true;
        return;
      }
    }
    else
    {
      m_sc2_engine_replay_endpoint_guest = m_guest;
      m_sc2_engine_replay_endpoint_tb_remainder = m_tb_cycle_remainder;
      m_sc2_engine_replay_endpoint_ram.assign(m_guest.ram, m_guest.ram + m_guest.ram_size);
      m_sc2_engine_replay_endpoint_l1.assign(l1, l1 + l1_size);
      std::memcpy(m_guest.ram, m_sc2_engine_replay_entry_ram.data(), m_guest.ram_size);
      if (l1_size != 0)
        std::memcpy(l1, m_sc2_engine_replay_entry_l1.data(), l1_size);
    }
    m_guest = m_sc2_engine_replay_entry_guest;
    m_tb_cycle_remainder = m_sc2_engine_replay_entry_tb_remainder;
    m_sc2_engine_replay_replaying = true;
    std::fprintf(stderr,
                 "[sc2-engine-replay] restored entry; replaying one engine tick%s\n",
                 m_sc2_engine_replay_full_emulator ? " to capture normalized reference" : "");
    return;
  }

  if (m_sc2_engine_replay_full_emulator)
  {
    const CPUState replayed_guest = m_guest;
    const u64 replayed_tb_remainder = m_tb_cycle_remainder;
    if (!m_sc2_engine_replay_have_reference)
    {
      m_sc2_engine_replay_endpoint_guest = replayed_guest;
      m_sc2_engine_replay_endpoint_tb_remainder = replayed_tb_remainder;
      m_sc2_engine_replay_reference_input_valid = NetPlay::FinishSc2EngineInputReplay();
      if (!m_sc2_engine_replay_reference_input_valid ||
          !capture_full_emulator_state(m_sc2_engine_replay_endpoint_state,
                                       m_sc2_engine_replay_endpoint_state_size) ||
          !restore_full_emulator_state(m_sc2_engine_replay_entry_state,
                                       m_sc2_engine_replay_entry_state_size) ||
          !NetPlay::BeginSc2EngineInputReplay())
      {
        NetPlay::EndSc2EngineInputReplay();
        std::fprintf(stderr,
                     "[sc2-engine-replay] failed input replay, reference capture, or entry "
                     "restore\n");
        m_sc2_engine_replay_completed = true;
        return;
      }
      m_guest = m_sc2_engine_replay_entry_guest;
      m_tb_cycle_remainder = m_sc2_engine_replay_entry_tb_remainder;
      m_sc2_engine_replay_have_reference = true;
      std::fprintf(stderr,
                   "[sc2-engine-replay] captured normalized reference; restored entry for "
                   "verification replay\n");
      return;
    }

    std::size_t replayed_state_size = 0;
    const bool verification_input_valid = NetPlay::FinishSc2EngineInputReplay();
    if (!verification_input_valid ||
        !capture_full_emulator_state(m_sc2_engine_replay_replayed_state, replayed_state_size))
    {
      NetPlay::EndSc2EngineInputReplay();
      std::fprintf(stderr,
                   "[sc2-engine-replay] failed input replay or replayed endpoint capture\n");
      m_sc2_engine_replay_completed = true;
      return;
    }
    const bool state_size_match = replayed_state_size == m_sc2_engine_replay_endpoint_state_size;
    const std::size_t comparable_size =
        std::min(replayed_state_size, m_sc2_engine_replay_endpoint_state_size);
    const std::span<const u8> endpoint_state{m_sc2_engine_replay_endpoint_state.data(),
                                             comparable_size};
    const std::span<const u8> replayed_state{m_sc2_engine_replay_replayed_state.data(),
                                             comparable_size};
    const auto mismatch = std::mismatch(endpoint_state.begin(), endpoint_state.end(),
                                        replayed_state.begin(), replayed_state.end());
    const std::size_t state_difference =
        mismatch.first == endpoint_state.end() ? comparable_size :
                                                 static_cast<std::size_t>(mismatch.first -
                                                                          endpoint_state.begin());
    const bool state_match = state_size_match && state_difference == comparable_size;
    std::size_t differing_state_bytes = 0;
    std::size_t last_state_difference = 0;
    for (std::size_t offset = 0; offset < comparable_size; ++offset)
    {
      if (endpoint_state[offset] != replayed_state[offset])
      {
        ++differing_state_bytes;
        last_state_difference = offset;
      }
    }
    differing_state_bytes +=
        replayed_state_size > m_sc2_engine_replay_endpoint_state_size ?
            replayed_state_size - m_sc2_engine_replay_endpoint_state_size :
            m_sc2_engine_replay_endpoint_state_size - replayed_state_size;
    const bool cpu_match =
        std::memcmp(&m_sc2_engine_replay_endpoint_guest, &replayed_guest, sizeof(CPUState)) == 0;
    const bool tb_remainder_match =
        m_sc2_engine_replay_endpoint_tb_remainder == replayed_tb_remainder;
    std::fprintf(stderr,
                 "[sc2-engine-replay] full-state-result state_match=%s cpu_match=%s "
                 "tb_remainder_match=%s input_replay_match=%s input_polls=%zu "
                 "external_profile_complete=%s "
                 "endpoint_bytes=%zu replay_bytes=%zu "
                 "differing_state_bytes=%zu first_state_difference=0x%08zx "
                 "last_state_difference=0x%08zx endpoint_value=0x%02x replay_value=0x%02x "
                 "endpoint_tb=%llu replay_tb=%llu\n",
                 state_match ? "yes" : "no", cpu_match ? "yes" : "no",
                 tb_remainder_match ? "yes" : "no", verification_input_valid ? "yes" : "no",
                 m_sc2_engine_replay_input_polls,
                 m_sc2_engine_external_profile_complete &&
                         !m_sc2_engine_external_profile_overflow ?
                     "yes" : "no",
                 m_sc2_engine_replay_endpoint_state_size,
                 replayed_state_size, differing_state_bytes, state_match ? 0 : state_difference,
                 state_match ? 0 : last_state_difference,
                 state_match || state_difference >= endpoint_state.size() ?
                     0 : endpoint_state[state_difference],
                 state_match || state_difference >= replayed_state.size() ?
                     0 : replayed_state[state_difference],
                 static_cast<unsigned long long>(m_sc2_engine_replay_endpoint_guest.timebase),
                 static_cast<unsigned long long>(replayed_guest.timebase));
    std::fflush(stderr);
    NetPlay::EndSc2EngineInputReplay();
    m_guest = replayed_guest;
    m_tb_cycle_remainder = replayed_tb_remainder;
    m_sc2_engine_replay_replaying = false;
    m_sc2_engine_replay_completed = true;
    return;
  }

  const auto first_difference = [](const std::span<const u8> lhs,
                                   const std::span<const u8> rhs) {
    const auto mismatch = std::mismatch(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    return mismatch.first == lhs.end() ? lhs.size() :
                                         static_cast<std::size_t>(mismatch.first - lhs.begin());
  };
  const std::span<const u8> endpoint_ram{m_sc2_engine_replay_endpoint_ram};
  const std::span<const u8> replay_ram{m_guest.ram, m_guest.ram_size};
  const std::span<const u8> endpoint_l1{m_sc2_engine_replay_endpoint_l1};
  const std::span<const u8> replay_l1{l1, l1_size};
  const std::size_t ram_difference = first_difference(endpoint_ram, replay_ram);
  const std::size_t l1_difference = first_difference(endpoint_l1, replay_l1);
  const bool ram_match = ram_difference == endpoint_ram.size();
  const bool l1_match = l1_difference == endpoint_l1.size();
  const bool cpu_match =
      std::memcmp(&m_sc2_engine_replay_endpoint_guest, &m_guest, sizeof(CPUState)) == 0;
  std::size_t differing_ram_bytes = 0;
  std::size_t differing_ram_pages = 0;
  constexpr std::size_t PAGE_SIZE = 4096;
  for (std::size_t page_begin = 0; page_begin < endpoint_ram.size(); page_begin += PAGE_SIZE)
  {
    const std::size_t page_end = std::min(endpoint_ram.size(), page_begin + PAGE_SIZE);
    bool page_differs = false;
    for (std::size_t offset = page_begin; offset < page_end; ++offset)
    {
      if (endpoint_ram[offset] != replay_ram[offset])
      {
        ++differing_ram_bytes;
        page_differs = true;
      }
    }
    if (page_differs)
      ++differing_ram_pages;
  }
  const auto read_be32 = [](const std::span<const u8> bytes, const std::size_t offset) {
    return (u32{bytes[offset]} << 24) | (u32{bytes[offset + 1]} << 16) |
           (u32{bytes[offset + 2]} << 8) | u32{bytes[offset + 3]};
  };
  const std::size_t ram_word_offset = ram_match ? 0 : ram_difference & ~std::size_t{3};
  const u32 endpoint_word = ram_match ? 0 : read_be32(endpoint_ram, ram_word_offset);
  const u32 replay_word = ram_match ? 0 : read_be32(replay_ram, ram_word_offset);
  const std::span<const u8> endpoint_cpu{
      reinterpret_cast<const u8*>(&m_sc2_engine_replay_endpoint_guest), sizeof(CPUState)};
  const std::span<const u8> replay_cpu{reinterpret_cast<const u8*>(&m_guest), sizeof(CPUState)};
  const std::size_t cpu_difference = first_difference(endpoint_cpu, replay_cpu);
  std::fprintf(stderr,
               "[sc2-engine-replay] result ram_match=%s l1_match=%s cpu_match=%s "
               "differing_ram_bytes=%zu differing_ram_pages=%zu "
               "first_ram_difference=0x%08zx endpoint_word=0x%08x replay_word=0x%08x "
               "first_l1_difference=0x%08zx first_cpu_difference=0x%08zx\n",
               ram_match ? "yes" : "no", l1_match ? "yes" : "no",
               cpu_match ? "yes" : "no", differing_ram_bytes, differing_ram_pages,
               ram_match ? 0 : ram_difference, endpoint_word, replay_word,
               l1_match ? 0 : l1_difference, cpu_match ? 0 : cpu_difference);
  std::fprintf(stderr,
               "[sc2-engine-replay] cpu endpoint pc=0x%08x lr=0x%08x tb=%llu downcount=%lld; "
               "replay pc=0x%08x lr=0x%08x tb=%llu downcount=%lld\n",
               m_sc2_engine_replay_endpoint_guest.pc, m_sc2_engine_replay_endpoint_guest.lr,
               static_cast<unsigned long long>(m_sc2_engine_replay_endpoint_guest.timebase),
               static_cast<long long>(m_sc2_engine_replay_endpoint_guest.downcount), m_guest.pc,
               m_guest.lr, static_cast<unsigned long long>(m_guest.timebase),
               static_cast<long long>(m_guest.downcount));
  std::fflush(stderr);
  m_sc2_engine_replay_replaying = false;
  m_sc2_engine_replay_completed = true;
}

void StaticRecompCore::LoadModule()
{
  if (m_module_source.kind == StaticRecompModuleSource::Kind::None)
  {
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: no explicit module source; interpreter-only.");
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  std::string path = m_module_source.path;
  const StaticRecompModuleDesc* desc = nullptr;
  if (m_module_source.kind == StaticRecompModuleSource::Kind::AttachedDescriptor)
  {
    desc = m_module_source.descriptor;
  }
  else
  {
    if (path.empty() || !File::Exists(path) || !m_library.Open(path.c_str()))
    {
      ERROR_LOG_FMT(POWERPC, "StaticRecomp: failed to open explicit module '{}'.", path);
      return;
    }
    const auto get_module = reinterpret_cast<StaticRecompGetModuleFn>(
        m_library.GetSymbolAddress(STATICRECOMP_GET_MODULE_SYMBOL));
    desc = get_module ? get_module() : nullptr;
  }

  const auto reject = [&](const std::string& why) {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: rejecting module '{}': {}. Interpreter-only.", path, why);
    m_module = nullptr;
    if (m_library.IsOpen())
      m_library.Close();
  };

  if (!desc)
    return reject("missing or null " STATICRECOMP_GET_MODULE_SYMBOL);
  // v2 is still accepted: it is the same struct minus the trailing optional
  // entry_points pair, so everything before that offset is laid out identically
  // and a v2 module keeps loading unchanged. Those two fields must not be read
  // for a v2 descriptor -- they are past the end of what it allocated.
  if (desc->abi_version != STATICRECOMP_ABI_VERSION && desc->abi_version != 2u)
    return reject(fmt::format("abi_version {} is neither {} nor 2", desc->abi_version,
                              STATICRECOMP_ABI_VERSION));
  if (desc->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION)
    return reject(
        fmt::format("cpu_abi_version {} != {}", desc->cpu_abi_version, GXRUNTIME_CPU_ABI_VERSION));
  if (desc->cpu_state_size != sizeof(CPUState))
    return reject(fmt::format("cpu_state_size {} != sizeof(CPUState) {}", desc->cpu_state_size,
                              sizeof(CPUState)));
  if (!desc->dispatch || !desc->code_ranges || desc->num_code_ranges == 0)
    return reject("no dispatch entry or empty code ranges");
  if (!std::memchr(desc->game_id, '\0', sizeof(desc->game_id)) || desc->game_id[0] == '\0')
    return reject("invalid game_id");
  if (!RangesAreSorted(desc->code_ranges, desc->num_code_ranges))
    return reject("malformed or overlapping code ranges");
  if (desc->num_smc_ranges != 0 && !RangesAreSorted(desc->smc_ranges, desc->num_smc_ranges))
    return reject("malformed or overlapping SMC ranges");
  if (!desc->chunk_ranges || desc->num_chunk_ranges == 0 || !desc->chunk_hashes)
    return reject("no chunk ranges/hashes (required for the SMC guard)");
  if (!ChunksTileCode(*desc))
    return reject("chunk ranges do not exactly tile code ranges");
  if (!AddressIsCovered(desc->code_ranges, desc->num_code_ranges, desc->entry_point))
    return reject("entry point is not covered by the module");
  if (!game_id.empty() && game_id != desc->game_id)
    return reject(fmt::format("module game_id '{}' != running game '{}'", desc->game_id, game_id));

  m_module = desc;
  m_module_active = (desc != nullptr);
  m_chunk_state.assign(desc->num_chunk_ranges, CHUNK_UNVERIFIED);
  m_failed_chunks = 0;
  m_lookup_ram_size = 0;
  m_lookup_exram_size = 0;
  m_chunk_lookup_table.clear();

  std::fprintf(stderr, "[staticrecomp] module loaded: %s entry=0x%08X\n", path.c_str(),
               desc->entry_point);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: loaded module '{}' (game_id={} entry=0x{:08X} "
                 "code_ranges={} smc_ranges={})",
                 path, desc->game_id, desc->entry_point, desc->num_code_ranges,
                 desc->num_smc_ranges);
}

void StaticRecompCore::ClearCache()
{
  // Jit64::ClearCache memsets its entire code space: measured 14-16 ms on every
  // call, and ~106 ms on the first. A savestate load calls this unconditionally
  // (JitInterface::DoState), which makes it ~70% of a rollback restore. Skipped
  // when the fallback has not run since the last clear, because then its cache
  // is already empty and the memset is pure cost. The flag is set again the
  // instant it runs, so a populated cache is still cleared exactly as before.
  if (m_fallback_jit && m_fallback_jit_used)
  {
    m_fallback_jit->ClearCache();
    m_fallback_jit_used = false;
  }

  if (!m_module)
    return;
  std::fill(m_chunk_state.begin(), m_chunk_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed_chunks = 0;
  ++m_reverify_events;
}
