// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>

#include <cstdio>
#include <cstring>
#include <limits>

#include "Common/Config/Config.h"
#include "Common/DynamicLibrary.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/Core.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/RollbackUndoSnapshotRing.h"
#include "Core/NetPlay/Sc2RollbackTransactionStore.h"
#include "Core/PowerPC/StaticRecomp/FrameDispatchProfiler.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/RecompDeterminism.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/Fifo.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif
#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif

StaticRecompCore* g_static_recomp_core = nullptr;

struct StaticRecompCore::Sc2TransactionReplayContract
{
  u64 transaction_id = 0;
  CPUState entry_guest{};
  u64 entry_tb_remainder = 0;
  std::vector<NetPlay::Sc2EngineInputPoll> input_polls;
  std::vector<Sc2UpdateExternalEffect> external_effects;
  std::map<u64, std::vector<std::pair<u32, u8>>> handler_post_ram;
  std::map<u64, CPUState> handler_post_guest;
  std::map<u64, u64> handler_post_tb_remainder;
  std::map<u64, std::pair<std::size_t, std::size_t>> handler_effect_ranges;
};

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
                         std::strcmp(replay_probe, "FULL_EMULATOR_ONE_TICK") == 0 ||
                         std::strcmp(replay_probe,
                                     "FULL_EMULATOR_ONE_TICK_CORRECTED_INPUT") == 0 ||
                         std::strcmp(replay_probe, "FULL_EMULATOR_UPDATE_CALL") == 0 ||
                         std::strcmp(replay_probe, "SELECTIVE_UPDATE_CALL") == 0 ||
                         std::strcmp(replay_probe, "SELECTIVE_INPUT_UPDATE_SPAN") == 0 ||
                         std::strcmp(replay_probe,
                                     "SELECTIVE_INPUT_UPDATE_CORRECTED") == 0))
    {
      m_sc2_engine_replay_probe_enabled = true;
      m_sc2_engine_replay_full_emulator =
          std::strcmp(replay_probe, "FULL_MEM1_ONE_TICK") != 0;
      m_sc2_engine_replay_update_call =
          std::strcmp(replay_probe, "FULL_EMULATOR_UPDATE_CALL") == 0 ||
          std::strcmp(replay_probe, "SELECTIVE_UPDATE_CALL") == 0 ||
          std::strcmp(replay_probe, "SELECTIVE_INPUT_UPDATE_SPAN") == 0 ||
          std::strcmp(replay_probe, "SELECTIVE_INPUT_UPDATE_CORRECTED") == 0;
      m_sc2_engine_replay_input_update_span =
          std::strcmp(replay_probe, "SELECTIVE_INPUT_UPDATE_SPAN") == 0 ||
          std::strcmp(replay_probe, "SELECTIVE_INPUT_UPDATE_CORRECTED") == 0;
      m_sc2_engine_replay_selective_update =
          std::strcmp(replay_probe, "SELECTIVE_UPDATE_CALL") == 0 ||
          m_sc2_engine_replay_input_update_span;
      m_sc2_engine_replay_corrected_input =
          std::strcmp(replay_probe, "FULL_EMULATOR_ONE_TICK_CORRECTED_INPUT") == 0 ||
          std::strcmp(replay_probe, "SELECTIVE_INPUT_UPDATE_CORRECTED") == 0;
      m_sc2_engine_replay_tick_span =
          std::strcmp(replay_probe, "FULL_EMULATOR_ONE_TICK_CORRECTED_INPUT") == 0 ? 2 : 1;
      std::fprintf(stderr,
                   "[sc2-engine-replay] enabled mode=%s begin_pc=0x%08x "
                   "return_pc=0x%08x ticks=%u\n",
                   std::strcmp(replay_probe, "FULL_EMULATOR_ONE_TICK_CORRECTED_INPUT") == 0 ?
                       "full-emulator-one-tick-corrected-input" :
                   std::strcmp(replay_probe, "SELECTIVE_INPUT_UPDATE_CORRECTED") == 0 ?
                       "selective-input-update-corrected" :
                   m_sc2_engine_replay_input_update_span ?
                       "selective-input-update-span" :
                   m_sc2_engine_replay_selective_update ? "selective-update-call" :
                   m_sc2_engine_replay_update_call ? "full-emulator-update-call" :
                   m_sc2_engine_replay_full_emulator ? "full-emulator-one-tick" :
                                                       "full-mem1-one-tick",
                   m_sc2_engine_replay_input_update_span ? 0x80011c80 :
                   m_sc2_engine_replay_update_call ? 0x800095c0 : 0x8001ba3c,
                   m_sc2_engine_replay_update_call ? 0x8001bcb0 : 0x8002d628,
                   m_sc2_engine_replay_tick_span);
    }
  }

  const char* const transaction_history =
      std::getenv("RINGOUT_SC2_TRANSACTION_HISTORY_PROBE");
  const char* const transaction_replay =
      std::getenv("RINGOUT_SC2_TRANSACTION_REPLAY_PROBE");
  if (((transaction_history != nullptr &&
        std::strcmp(transaction_history, "HEADLESS_ISOLATED") == 0) ||
       (transaction_replay != nullptr &&
        std::strcmp(transaction_replay, "HEADLESS_ISOLATED") == 0)) &&
      !m_sc2_engine_replay_probe_enabled)
  {
    m_sc2_transaction_history_enabled = true;
    m_sc2_transaction_replay_probe_enabled =
        transaction_replay != nullptr &&
        std::strcmp(transaction_replay, "HEADLESS_ISOLATED") == 0;
    std::fprintf(stderr,
                 "[sc2-transaction-history] enabled capacity=10 max_unique_bytes=1048576 "
                 "begin_pc=0x80011c80 end_pc=0x8001bcb0\n");
    if (m_sc2_transaction_replay_probe_enabled)
    {
      std::fprintf(stderr,
                   "[sc2-transaction-replay] enabled transactions=3 passes=2 "
                   "corrected_input=remote-a\n");
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
  if (m_sc2_transaction_history_active)
  {
    if (m_sc2_engine_set_mem_journal)
      m_sc2_engine_set_mem_journal(nullptr, nullptr);
    NetPlay::EndSc2EngineInputReplay();
    m_sc2_transaction_history_active = false;
  }
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
  ++m_sc2_video_frame;
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

void StaticRecompCore::Sc2RollbackTransactionWriteTrampoline(const u32 offset, const u32 size,
                                                              void* const user)
{
  auto* const core = static_cast<StaticRecompCore*>(user);
  if (!core->m_sc2_transaction_history_active || !core->m_sc2_transaction_store || size == 0 ||
      !core->m_sc2_transaction_store->RecordWrite(
          offset, size, std::span<const u8>{core->m_guest.ram, core->m_guest.ram_size}))
  {
    core->m_sc2_transaction_history_faulted = true;
    return;
  }
  if (core->m_sc2_transaction_replay_active)
    return;
  Sc2EngineDirectCallWriteTrampoline(offset, size, user);
}

void StaticRecompCore::InjectSc2TransactionReplayInput(const u32 pc)
{
  if (!m_sc2_engine_replay_input_update_span || !m_sc2_engine_speculative_active ||
      pc != 0x80183708 || m_guest.lr != 0x801bfa80 || m_guest.gpr[29] >= 4)
  {
    return;
  }

  const int pad = static_cast<int>(m_guest.gpr[29]);
  GCPadStatus status{};
  bool resolved = false;
  if (!NetPlay::ConsumeSc2EngineInputReplay(pad, true, &status, &resolved))
  {
    m_sc2_update_external_valid = false;
    std::fprintf(stderr, "[sc2-input-inject] missing replay poll pad=%d\n", pad);
    return;
  }
  if (!resolved)
    return;

  u32 hi = 0;
  u32 low = 0;
  if (!m_system.GetSerialInterface().EncodeGCControllerPadStatus(pad, status, &hi, &low))
  {
    m_sc2_update_external_valid = false;
    std::fprintf(stderr, "[sc2-input-inject] unsupported device pad=%d\n", pad);
    return;
  }
  const u32 address = m_guest.gpr[30];
  const u32 offset = address - 0x80000000u;
  if (m_sc2_transaction_replay_active &&
      (address < 0x80000000u || offset > m_guest.ram_size || 8 > m_guest.ram_size - offset ||
       !m_sc2_transaction_store->RecordWrite(
           offset, 8, std::span<const u8>{m_guest.ram, m_guest.ram_size})))
  {
    m_sc2_update_external_valid = false;
    std::fprintf(stderr, "[sc2-input-inject] failed to journal replay slot pad=%d\n", pad);
    return;
  }
  GuestWrite32(address, hi);
  GuestWrite32(address + 4, low);
  std::fprintf(stderr,
               "[sc2-input-inject] pad=%d hi=0x%08x low=0x%08x corrected=%s\n", pad, hi,
               low,
               (m_sc2_engine_replay_corrected_input || m_sc2_transaction_replay_active) ?
                   "yes" :
                   "no");
}

bool StaticRecompCore::StartSc2TransactionReplayStep(const u64 transaction_id,
                                                      const CPUState& entry_guest,
                                                      const u64 entry_tb_remainder)
{
  if (transaction_id < m_sc2_transaction_replay_first ||
      transaction_id > m_sc2_transaction_replay_through)
  {
    return false;
  }
  const std::size_t index =
      static_cast<std::size_t>(transaction_id - m_sc2_transaction_replay_first);
  if (index >= m_sc2_transaction_replay_contracts.size() ||
      !m_sc2_transaction_replay_contracts[index])
  {
    return false;
  }
  const Sc2TransactionReplayContract& contract =
      *m_sc2_transaction_replay_contracts[index];
  if (contract.transaction_id != transaction_id || contract.input_polls.empty() ||
      !NetPlay::BeginSc2EngineInputReplayFrom(
          contract.input_polls, transaction_id == m_sc2_transaction_replay_first))
  {
    return false;
  }

  m_sc2_update_external_effects = contract.external_effects;
  m_sc2_update_handler_post_ram = contract.handler_post_ram;
  m_sc2_update_handler_post_guest = contract.handler_post_guest;
  m_sc2_update_handler_post_tb_remainder = contract.handler_post_tb_remainder;
  m_sc2_update_handler_effect_ranges = contract.handler_effect_ranges;
  m_sc2_update_external_replay_index = 0;
  m_sc2_update_external_keyed_replay = true;
  m_sc2_update_external_replay_used.assign(contract.external_effects.size(), false);
  m_sc2_update_external_replay_reserved.assign(contract.external_effects.size(), false);
  for (const auto& [key, range] : contract.handler_effect_ranges)
  {
    static_cast<void>(key);
    const auto [begin, end] = range;
    if (begin > end || end > m_sc2_update_external_replay_reserved.size())
      return false;
    std::fill(m_sc2_update_external_replay_reserved.begin() + begin,
              m_sc2_update_external_replay_reserved.begin() + end, true);
  }
  m_sc2_update_external_valid = true;
  m_sc2_update_external_capture = false;
  m_sc2_update_external_replay = true;
  m_sc2_engine_external_profile_active = false;
  m_sc2_engine_replay_input_update_span = true;
  m_sc2_engine_speculative_active = true;
  m_sc2_transaction_replay_active = true;
  m_sc2_transaction_history_active = true;
  m_sc2_transaction_replay_current = transaction_id;

  u8* const ram = m_guest.ram;
  const u32 ram_size = m_guest.ram_size;
  u8* const mem2 = m_guest.mem2;
  const u32 mem2_size = m_guest.mem2_size;
  m_guest = entry_guest;
  m_guest.ram = ram;
  m_guest.ram_size = ram_size;
  m_guest.mem2 = mem2;
  m_guest.mem2_size = mem2_size;
  m_tb_cycle_remainder = entry_tb_remainder;
  m_sc2_engine_set_mem_journal(&StaticRecompCore::Sc2RollbackTransactionWriteTrampoline, this);
  std::fprintf(stderr,
               "[sc2-transaction-replay] pass=%u transaction=%llu polls=%zu effects=%zu "
               "handlers=%zu begin\n",
               m_sc2_transaction_replay_pass,
               static_cast<unsigned long long>(transaction_id), contract.input_polls.size(),
               contract.external_effects.size(), contract.handler_post_ram.size());
  return true;
}

bool StaticRecompCore::StartSc2TransactionReplayPass()
{
  if (!m_sc2_transaction_store || m_sc2_transaction_replay_contracts.empty())
    return false;
  const Sc2TransactionReplayContract& first = *m_sc2_transaction_replay_contracts.front();
  const Sc2TransactionReplayContract& last = *m_sc2_transaction_replay_contracts.back();
  const auto first_batch = std::find_if(first.input_polls.begin(), first.input_polls.end(),
                                        [](const auto& poll) { return poll.batch_id.has_value(); });
  const auto last_batch = std::find_if(last.input_polls.rbegin(), last.input_polls.rend(),
                                       [](const auto& poll) { return poll.batch_id.has_value(); });
  if (first_batch == first.input_polls.end() || last_batch == last.input_polls.rend())
    return false;
  const auto plan =
      m_sc2_transaction_store->PlanCorrection(*first_batch->batch_id, *last_batch->batch_id);
  std::vector<u8> auxiliary(sizeof(CPUState) + sizeof(u64));
  if (!m_sc2_transaction_store->Restore(
          plan, std::span<u8>{m_guest.ram, m_guest.ram_size}, auxiliary))
  {
    return false;
  }
  CPUState entry_guest{};
  u64 entry_tb_remainder = 0;
  std::memcpy(&entry_guest, auxiliary.data(), sizeof(CPUState));
  std::memcpy(&entry_tb_remainder, auxiliary.data() + sizeof(CPUState), sizeof(u64));
  ++m_sc2_transaction_replay_pass;
  return StartSc2TransactionReplayStep(m_sc2_transaction_replay_first, entry_guest,
                                       entry_tb_remainder);
}

bool StaticRecompCore::TryStartSc2TransactionReplayProbe()
{
  if (!m_sc2_transaction_replay_probe_enabled || m_sc2_transaction_replay_probe_attempted ||
      !m_sc2_transaction_store)
  {
    return false;
  }
  const std::optional<u64> latest =
      m_sc2_transaction_store->GetTimeline().GetLatestCompletedTransaction();
  if (!latest || *latest < 3)
    return false;
  const u64 first = *latest - 2;
  std::vector<std::unique_ptr<Sc2TransactionReplayContract>> contracts;
  contracts.reserve(3);
  for (u64 id = first; id <= *latest; ++id)
  {
    const auto* const transaction = m_sc2_transaction_store->GetTimeline().Find(id);
    const auto& source = m_sc2_transaction_contracts[id % m_sc2_transaction_contracts.size()];
    if (!transaction || transaction->consumed_batches.empty() || !source ||
        source->transaction_id != id || source->input_polls.empty() ||
        !std::ranges::any_of(source->input_polls, [](const auto& poll) {
          return poll.pad_num == 1 && poll.result;
        }))
    {
      return false;
    }
    contracts.push_back(std::make_unique<Sc2TransactionReplayContract>(*source));
  }

  m_sc2_transaction_replay_probe_attempted = true;
  m_sc2_transaction_replay_first = first;
  m_sc2_transaction_replay_through = *latest;
  m_sc2_transaction_replay_frontier = m_sc2_transaction_id;
  m_sc2_transaction_replay_contracts = std::move(contracts);
  m_sc2_transaction_replay_fifo_was_running =
      Core::GetState(m_system) == Core::State::Running;
  m_system.GetFifo().PauseAndLock();
  m_sc2_transaction_replay_fifo_locked = true;
  SyncOut();
  {
    const State::ScopedRollbackSnapshot rollback_snapshot_scope;
    m_sc2_transaction_replay_canonical_state_size =
        State::SaveToBuffer(m_system, m_sc2_transaction_replay_canonical_state);
  }
  if (m_sc2_transaction_replay_canonical_state_size == 0)
  {
    AbortSc2TransactionReplay("canonical-state-capture");
    return false;
  }
  m_sc2_transaction_replay_canonical_ram.assign(m_guest.ram,
                                                 m_guest.ram + m_guest.ram_size);
  m_sc2_transaction_replay_canonical_guest = m_guest;
  m_sc2_transaction_replay_canonical_tb_remainder = m_tb_cycle_remainder;
  m_sc2_transaction_replay_pass = 0;
  m_sc2_transaction_replay_perturbed_polls = 0;
  if (!StartSc2TransactionReplayPass())
  {
    AbortSc2TransactionReplay("first-pass-start");
    return false;
  }
  return true;
}

void StaticRecompCore::AbortSc2TransactionReplay(const char* const reason)
{
  if (m_sc2_engine_set_mem_journal)
    m_sc2_engine_set_mem_journal(nullptr, nullptr);
  NetPlay::EndSc2EngineInputReplay();
  bool state_restored = true;
  if (m_sc2_transaction_replay_canonical_state_size != 0)
  {
    const State::ScopedRollbackSnapshot rollback_snapshot_scope;
    state_restored = State::LoadFromBuffer(
        m_system,
        std::span<u8>{m_sc2_transaction_replay_canonical_state.data(),
                      m_sc2_transaction_replay_canonical_state_size});
  }
  if (m_guest.ram != nullptr &&
      m_sc2_transaction_replay_canonical_ram.size() == m_guest.ram_size)
  {
    std::memcpy(m_guest.ram, m_sc2_transaction_replay_canonical_ram.data(), m_guest.ram_size);
    u8* const ram = m_guest.ram;
    const u32 ram_size = m_guest.ram_size;
    u8* const mem2 = m_guest.mem2;
    const u32 mem2_size = m_guest.mem2_size;
    m_guest = m_sc2_transaction_replay_canonical_guest;
    m_guest.ram = ram;
    m_guest.ram_size = ram_size;
    m_guest.mem2 = mem2;
    m_guest.mem2_size = mem2_size;
    m_tb_cycle_remainder = m_sc2_transaction_replay_canonical_tb_remainder;
  }
  if (m_sc2_transaction_replay_fifo_locked)
  {
    m_system.GetFifo().RestoreState(m_sc2_transaction_replay_fifo_was_running);
    m_sc2_transaction_replay_fifo_locked = false;
  }
  m_sc2_transaction_replay_active = false;
  m_sc2_engine_speculative_active = false;
  m_sc2_update_external_replay = false;
  m_sc2_update_external_keyed_replay = false;
  m_sc2_transaction_history_active = false;
  m_sc2_transaction_history_enabled = false;
  std::fprintf(stderr,
               "[sc2-transaction-replay] stopped reason=%s canonical_state_bytes=%zu "
               "state_restored=%s\n",
               reason, m_sc2_transaction_replay_canonical_state_size,
               state_restored ? "yes" : "no");
}

void StaticRecompCore::FinishSc2TransactionReplayStep()
{
  m_sc2_engine_set_mem_journal(nullptr, nullptr);
  std::size_t perturbed_polls = 0;
  bool valid = NetPlay::FinishSc2EngineInputReplay(&perturbed_polls) &&
               m_sc2_update_external_valid;
  m_sc2_transaction_replay_perturbed_polls += perturbed_polls;

  const std::size_t index = static_cast<std::size_t>(
      m_sc2_transaction_replay_current - m_sc2_transaction_replay_first);
  const Sc2TransactionReplayContract& contract =
      *m_sc2_transaction_replay_contracts[index];
  std::optional<u64> last_batch;
  for (const auto& poll : contract.input_polls)
  {
    if (poll.batch_id && poll.batch_id != last_batch)
    {
      valid = m_sc2_transaction_store->RecordConsumedBatch(*poll.batch_id) && valid;
      last_batch = poll.batch_id;
    }
  }
  valid = m_sc2_transaction_store->CompleteTransaction(m_sc2_video_frame) && valid;
  m_sc2_transaction_history_active = false;
  m_sc2_update_external_replay = false;
  m_sc2_update_external_keyed_replay = false;
  std::fprintf(stderr,
               "[sc2-transaction-replay] pass=%u transaction=%llu perturbed_polls=%zu "
               "effects=%zu/%zu result=%s\n",
               m_sc2_transaction_replay_pass,
               static_cast<unsigned long long>(m_sc2_transaction_replay_current),
               perturbed_polls, m_sc2_update_external_replay_index,
               m_sc2_update_external_effects.size(), valid ? "ok" : "failed");
  if (!valid)
  {
    std::fprintf(stderr, "[sc2-transaction-replay] result=failed reason=contract\n");
    AbortSc2TransactionReplay("contract");
    return;
  }

  if (m_sc2_transaction_replay_current < m_sc2_transaction_replay_through)
  {
    const u64 next = m_sc2_transaction_replay_current + 1;
    const Sc2TransactionReplayContract& next_contract =
        *m_sc2_transaction_replay_contracts[static_cast<std::size_t>(
            next - m_sc2_transaction_replay_first)];
    std::vector<u8> auxiliary(sizeof(CPUState) + sizeof(u64));
    std::memcpy(auxiliary.data(), &next_contract.entry_guest, sizeof(CPUState));
    std::memcpy(auxiliary.data() + sizeof(CPUState), &next_contract.entry_tb_remainder,
                sizeof(u64));
    if (!m_sc2_transaction_store->BeginTransaction(
            next, m_sc2_video_frame, std::span<const u8>{m_guest.ram, m_guest.ram_size},
            auxiliary) ||
        !StartSc2TransactionReplayStep(next, next_contract.entry_guest,
                                       next_contract.entry_tb_remainder))
    {
      std::fprintf(stderr,
                   "[sc2-transaction-replay] result=failed reason=next-transaction\n");
      AbortSc2TransactionReplay("next-transaction");
    }
    return;
  }

  if (m_sc2_transaction_replay_pass == 1)
  {
    m_sc2_transaction_replay_reference_ram.assign(m_guest.ram,
                                                   m_guest.ram + m_guest.ram_size);
    m_sc2_transaction_replay_reference_guest = m_guest;
    m_sc2_transaction_replay_reference_tb_remainder = m_tb_cycle_remainder;
    std::vector<bool> owned(m_guest.ram_size, false);
    m_sc2_transaction_replay_owned_offsets.clear();
    for (u64 id = m_sc2_transaction_replay_first;
         id <= m_sc2_transaction_replay_through; ++id)
    {
      for (const u32 offset : m_sc2_transaction_store->GetUndoRing().GetWriteOffsets(id))
      {
        if (!owned[offset])
        {
          owned[offset] = true;
          m_sc2_transaction_replay_owned_offsets.push_back(offset);
        }
      }
    }
    std::ranges::sort(m_sc2_transaction_replay_owned_offsets);
    std::size_t corrected_bytes = 0;
    std::vector<u8> owned_digest_bytes;
    owned_digest_bytes.reserve(m_sc2_transaction_replay_owned_offsets.size() * 5);
    for (const u32 offset : m_sc2_transaction_replay_owned_offsets)
    {
      owned_digest_bytes.push_back(static_cast<u8>(offset >> 24));
      owned_digest_bytes.push_back(static_cast<u8>(offset >> 16));
      owned_digest_bytes.push_back(static_cast<u8>(offset >> 8));
      owned_digest_bytes.push_back(static_cast<u8>(offset));
      owned_digest_bytes.push_back(m_sc2_transaction_replay_reference_ram[offset]);
      if (m_sc2_transaction_replay_reference_ram[offset] !=
          m_sc2_transaction_replay_canonical_ram[offset])
      {
        ++corrected_bytes;
      }
    }
    const u32 owned_crc32 =
        Common::ComputeCRC32(owned_digest_bytes.data(), owned_digest_bytes.size());
    std::fprintf(stderr,
                 "[sc2-transaction-replay] corrected-reference transactions=3 "
                 "owned_bytes=%zu corrected_bytes=%zu owned_crc32=%08x perturbed_polls=%zu\n",
                 m_sc2_transaction_replay_owned_offsets.size(), corrected_bytes,
                 owned_crc32, m_sc2_transaction_replay_perturbed_polls);
    if (corrected_bytes == 0 || !StartSc2TransactionReplayPass())
    {
      std::fprintf(stderr,
                   "[sc2-transaction-replay] result=failed reason=reference-or-second-pass\n");
      AbortSc2TransactionReplay("reference-or-second-pass");
    }
    return;
  }

  std::size_t differing_bytes = 0;
  for (std::size_t offset = 0; offset < m_sc2_transaction_replay_reference_ram.size(); ++offset)
  {
    if (m_sc2_transaction_replay_reference_ram[offset] != m_guest.ram[offset])
      ++differing_bytes;
  }
  std::size_t owned_differing_bytes = 0;
  for (const u32 offset : m_sc2_transaction_replay_owned_offsets)
  {
    if (m_sc2_transaction_replay_reference_ram[offset] != m_guest.ram[offset])
      ++owned_differing_bytes;
  }
  const bool cpu_match =
      std::memcmp(&m_sc2_transaction_replay_reference_guest, &m_guest, sizeof(CPUState)) == 0;
  const bool tb_match =
      m_sc2_transaction_replay_reference_tb_remainder == m_tb_cycle_remainder;
  const bool exact = owned_differing_bytes == 0 && cpu_match && tb_match &&
                     m_sc2_transaction_replay_perturbed_polls >= 2;
  std::fprintf(stderr,
               "[sc2-transaction-replay] result=%s transactions=3 passes=2 "
               "game_ram_match=%s cpu_match=%s tb_remainder_match=%s owned_bytes=%zu "
               "owned_differing_bytes=%zu external_differing_bytes=%zu perturbed_polls=%zu\n",
               exact ? "ok" : "failed", owned_differing_bytes == 0 ? "yes" : "no",
               cpu_match ? "yes" : "no", tb_match ? "yes" : "no",
               m_sc2_transaction_replay_owned_offsets.size(), owned_differing_bytes,
               differing_bytes - owned_differing_bytes,
               m_sc2_transaction_replay_perturbed_polls);
  AbortSc2TransactionReplay(exact ? "complete" : "verification");
}

void StaticRecompCore::ObserveSc2RollbackTransaction(const u32 pc)
{
  if (!m_sc2_transaction_history_enabled || m_guest.ram == nullptr || m_guest.ram_size == 0)
  {
    return;
  }
  if (m_sc2_transaction_history_faulted)
  {
    if (m_sc2_transaction_history_active)
    {
      if (m_sc2_engine_set_mem_journal)
        m_sc2_engine_set_mem_journal(nullptr, nullptr);
      NetPlay::EndSc2EngineInputReplay();
      m_sc2_transaction_history_active = false;
    }
    m_sc2_transaction_store.reset();
    for (auto& contract : m_sc2_transaction_contracts)
      contract.reset();
    m_sc2_transaction_id = 1;
    if (m_sc2_transaction_epoch != std::numeric_limits<u64>::max())
      ++m_sc2_transaction_epoch;
    m_sc2_transaction_history_faulted = false;
    std::fprintf(stderr,
                 "[sc2-transaction-history] incomplete journal discarded; next_epoch=%llu\n",
                 static_cast<unsigned long long>(m_sc2_transaction_epoch));
    return;
  }

  constexpr u32 BEGIN_PC = 0x80011c80;
  constexpr u32 BEGIN_LR = 0x8001bbf8;
  constexpr u32 END_PC = 0x8001bcb0;
  if (m_sc2_transaction_replay_active)
  {
    InjectSc2TransactionReplayInput(pc);
    if (ReplaySc2UpdateSystemHandler(pc))
      return;
    if (pc == END_PC)
      FinishSc2TransactionReplayStep();
    return;
  }
  if (m_sc2_transaction_history_active)
  {
    ObserveSc2EngineIndirectCall(pc);
    ObserveSc2EngineDirectCall(pc);
  }
  if (pc == BEGIN_PC && m_guest.lr == BEGIN_LR)
  {
    if (TryStartSc2TransactionReplayProbe())
      return;
    if (m_sc2_transaction_history_active || m_lockstep_verifier->IsEnabled() || m_watch_armed)
    {
      m_sc2_transaction_history_faulted = true;
      return;
    }
    if (!m_sc2_transaction_store)
    {
      constexpr std::size_t CAPACITY = 10;
      constexpr std::size_t MAX_UNIQUE_BYTES = 1024 * 1024;
      m_sc2_transaction_store = std::make_unique<NetPlay::Sc2RollbackTransactionStore>(
          m_guest.ram_size, CAPACITY, MAX_UNIQUE_BYTES, sizeof(CPUState) + sizeof(u64));
      m_sc2_transaction_contracts.resize(CAPACITY);
      m_sc2_engine_set_mem_journal = reinterpret_cast<Sc2EngineSetMemJournalFn>(
          m_library.GetSymbolAddress("ppc_set_mem_write_journal"));
      if (m_sc2_transaction_store->GetConfigurationStatus() !=
              NetPlay::Sc2RollbackTransactionStore::ConfigurationStatus::Valid ||
          !m_sc2_engine_set_mem_journal)
      {
        m_sc2_transaction_history_faulted = true;
        return;
      }
    }

    m_sc2_update_external_effects.clear();
    m_sc2_update_original_write_blocks.clear();
    m_sc2_update_original_last_writer.clear();
    m_sc2_update_handler_post_ram.clear();
    m_sc2_update_handler_post_guest.clear();
    m_sc2_update_handler_post_tb_remainder.clear();
    m_sc2_update_handler_effect_ranges.clear();
    m_sc2_update_external_replay_index = 0;
    m_sc2_update_external_valid = true;
    m_sc2_update_external_capture = true;
    m_sc2_update_external_replay = false;
    m_sc2_engine_replay_input_update_span = true;
    m_sc2_engine_external_read_count = 0;
    m_sc2_engine_external_write_count = 0;
    m_sc2_engine_external_reads.clear();
    m_sc2_engine_external_writes.clear();
    m_sc2_engine_external_read_blocks.clear();
    m_sc2_engine_external_write_blocks.clear();
    m_sc2_engine_external_entry_fallback_count = m_hook_fallback_instructions;
    m_sc2_engine_external_profile_overflow = false;
    m_sc2_engine_external_profile_complete = false;
    m_sc2_engine_external_profile_active = true;
    m_sc2_engine_direct_calls.clear();
    m_sc2_engine_direct_call_target = BEGIN_PC;
    m_sc2_engine_direct_call_return = END_PC;
    m_sc2_engine_direct_call_entry_reads = 0;
    m_sc2_engine_direct_call_entry_writes = 0;
    m_sc2_engine_direct_call_entry_fallbacks = m_hook_fallback_instructions;
    m_sc2_engine_direct_call_completed = 0;
    m_sc2_engine_direct_call_overflow = false;
    m_sc2_engine_direct_call_active = true;
    static_cast<void>(
        m_sc2_engine_direct_calls[(static_cast<u64>(END_PC) << 32) | BEGIN_PC]);
    m_sc2_engine_indirect_calls.clear();
    m_sc2_engine_indirect_call_completed = 0;
    m_sc2_engine_indirect_call_active = false;
    m_sc2_engine_indirect_call_overflow = false;

    std::vector<u8> auxiliary(sizeof(CPUState) + sizeof(u64));
    std::memcpy(auxiliary.data(), &m_guest, sizeof(CPUState));
    std::memcpy(auxiliary.data() + sizeof(CPUState), &m_tb_cycle_remainder, sizeof(u64));
    if (!m_sc2_transaction_store->BeginTransaction(
            m_sc2_transaction_id, m_sc2_video_frame,
            std::span<const u8>{m_guest.ram, m_guest.ram_size}, auxiliary))
    {
      m_sc2_transaction_history_faulted = true;
      return;
    }
    auto contract = std::make_unique<Sc2TransactionReplayContract>();
    contract->transaction_id = m_sc2_transaction_id;
    contract->entry_guest = m_guest;
    contract->entry_tb_remainder = m_tb_cycle_remainder;
    m_sc2_transaction_contracts[m_sc2_transaction_id % m_sc2_transaction_contracts.size()] =
        std::move(contract);
    NetPlay::BeginSc2EngineInputCapture();
    m_sc2_engine_set_mem_journal(&StaticRecompCore::Sc2RollbackTransactionWriteTrampoline, this);
    m_sc2_transaction_video_frame = m_sc2_video_frame;
    m_sc2_transaction_entry_fallbacks = m_hook_fallback_instructions;
    m_sc2_transaction_begin_us = Common::Timer::NowUs();
    m_sc2_transaction_history_active = true;
    return;
  }

  if (pc != END_PC || !m_sc2_transaction_history_active)
    return;

  m_sc2_engine_set_mem_journal(nullptr, nullptr);
  std::size_t polls = 0;
  std::vector<u64> batches;
  std::vector<NetPlay::Sc2EngineInputPoll> input_polls;
  const bool input_valid =
      NetPlay::FinishSc2EngineInputCapture(&polls, &batches, &input_polls);
  // Boot/menu updates can legitimately run before NetPlay has assigned an SI
  // batch. Retain them as inputless transactions; the mapper will never select
  // them for an input correction, but later retained descendants still need a
  // contiguous sparse undo lineage.
  const u64 fallback_delta = m_hook_fallback_instructions - m_sc2_transaction_entry_fallbacks;
  bool valid = input_valid && fallback_delta == 0 && m_sc2_update_external_valid &&
               !m_sc2_engine_direct_call_active && !m_sc2_engine_direct_call_overflow &&
               !m_sc2_engine_indirect_call_active && !m_sc2_engine_indirect_call_overflow;
  for (const u64 batch : batches)
    valid = m_sc2_transaction_store->RecordConsumedBatch(batch) && valid;
  valid = m_sc2_transaction_store->CompleteTransaction(m_sc2_video_frame) && valid;
  const std::optional<std::size_t> unique_bytes =
      m_sc2_transaction_store->GetUndoRing().GetUniqueBytes(m_sc2_transaction_id);
  valid = unique_bytes.has_value() && valid;
  if (valid)
  {
    auto& contract =
        m_sc2_transaction_contracts[m_sc2_transaction_id % m_sc2_transaction_contracts.size()];
    valid = contract && contract->transaction_id == m_sc2_transaction_id;
    if (valid)
    {
      contract->input_polls = std::move(input_polls);
      contract->external_effects = m_sc2_update_external_effects;
      contract->handler_post_ram = m_sc2_update_handler_post_ram;
      contract->handler_post_guest = m_sc2_update_handler_post_guest;
      contract->handler_post_tb_remainder = m_sc2_update_handler_post_tb_remainder;
      contract->handler_effect_ranges = m_sc2_update_handler_effect_ranges;
    }
  }
  m_sc2_update_external_capture = false;
  m_sc2_engine_external_profile_active = false;
  const u64 elapsed_us = Common::Timer::NowUs() - m_sc2_transaction_begin_us;
  const u64 first_batch = batches.empty() ? 0 : batches.front();
  const u64 last_batch = batches.empty() ? 0 : batches.back();
  std::fprintf(stderr,
               "[sc2-transaction-history] epoch=%llu transaction=%llu "
               "video_frames=%llu..%llu polls=%zu batches=%zu unique_bytes=%zu "
               "first_batch=%llu last_batch=%llu effects=%zu handlers=%zu elapsed_us=%llu input_valid=%s "
               "fallback_instructions=%llu result=%s\n",
               static_cast<unsigned long long>(m_sc2_transaction_epoch),
               static_cast<unsigned long long>(m_sc2_transaction_id),
               static_cast<unsigned long long>(m_sc2_transaction_video_frame),
               static_cast<unsigned long long>(m_sc2_video_frame), polls, batches.size(),
               unique_bytes.value_or(0), static_cast<unsigned long long>(first_batch),
               static_cast<unsigned long long>(last_batch), m_sc2_update_external_effects.size(),
               m_sc2_update_handler_post_ram.size(),
               static_cast<unsigned long long>(elapsed_us),
               input_valid ? "yes" : "no", static_cast<unsigned long long>(fallback_delta),
               valid ? "ok" : "failed");
  m_sc2_transaction_history_active = false;
  if (!valid || m_sc2_transaction_id == std::numeric_limits<u64>::max())
  {
    m_sc2_transaction_history_faulted = true;
    return;
  }
  ++m_sc2_transaction_id;
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
  auto& block_sites =
      write ? m_sc2_engine_external_write_blocks : m_sc2_engine_external_read_blocks;
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

  constexpr std::size_t MAX_BLOCK_SITES = 2048;
  const u64 block_key = (static_cast<u64>(m_guest.pc) << 32) | address;
  const auto existing_block = block_sites.find(block_key);
  if (existing_block != block_sites.end())
  {
    ++existing_block->second;
  }
  else if (block_sites.size() < MAX_BLOCK_SITES)
  {
    block_sites.emplace(block_key, 1);
  }
  else
  {
    m_sc2_engine_external_profile_overflow = true;
  }
}

bool StaticRecompCore::ReplaySc2UpdateExternalRead(const u32 address, const u8 size, u64* value)
{
  if (!m_sc2_update_external_replay)
    return false;
  if (m_sc2_update_external_keyed_replay)
  {
    for (std::size_t index = 0; index < m_sc2_update_external_effects.size(); ++index)
    {
      const auto& effect = m_sc2_update_external_effects[index];
      if (!m_sc2_update_external_replay_used[index] &&
          !m_sc2_update_external_replay_reserved[index] && !effect.write &&
          effect.address == address && effect.size == size)
      {
        m_sc2_update_external_replay_used[index] = true;
        ++m_sc2_update_external_replay_index;
        *value = effect.value;
        return true;
      }
    }
    if (m_sc2_update_external_valid)
    {
      std::fprintf(stderr,
                   "[sc2-update-effects] keyed mismatch actual=read address=0x%08x "
                   "size=%u pc=0x%08x\n",
                   address, size, m_guest.pc);
    }
    m_sc2_update_external_valid = false;
    *value = 0;
    return true;
  }
  if (m_sc2_update_external_replay_index >= m_sc2_update_external_effects.size())
  {
    if (m_sc2_update_external_valid)
      std::fprintf(stderr,
                   "[sc2-update-effects] mismatch index=%zu actual=read address=0x%08x "
                   "size=%u reason=journal-exhausted pc=0x%08x\n",
                   m_sc2_update_external_replay_index, address, size, m_guest.pc);
    m_sc2_update_external_valid = false;
    *value = 0;
    return true;
  }
  const auto& effect = m_sc2_update_external_effects[m_sc2_update_external_replay_index++];
  if (effect.write || effect.address != address || effect.size != size)
  {
    if (m_sc2_update_external_valid)
      std::fprintf(stderr,
                   "[sc2-update-effects] mismatch index=%zu expected=%s address=0x%08x "
                   "size=%u actual=read address=0x%08x size=%u pc=0x%08x\n",
                   m_sc2_update_external_replay_index - 1, effect.write ? "write" : "read",
                   effect.address, effect.size, address, size, m_guest.pc);
    m_sc2_update_external_valid = false;
  }
  *value = effect.value;
  return true;
}

bool StaticRecompCore::ReplaySc2UpdateExternalWrite(const u32 address, const u8 size,
                                                     const u64 value)
{
  if (!m_sc2_update_external_replay)
    return false;
  if (m_sc2_update_external_keyed_replay)
  {
    for (std::size_t index = 0; index < m_sc2_update_external_effects.size(); ++index)
    {
      const auto& effect = m_sc2_update_external_effects[index];
      if (!m_sc2_update_external_replay_used[index] &&
          !m_sc2_update_external_replay_reserved[index] && effect.write &&
          effect.address == address && effect.size == size && effect.value == value)
      {
        m_sc2_update_external_replay_used[index] = true;
        ++m_sc2_update_external_replay_index;
        return true;
      }
    }
    if (m_sc2_update_external_valid)
    {
      std::fprintf(stderr,
                   "[sc2-update-effects] keyed mismatch actual=write address=0x%08x "
                   "size=%u value=0x%llx pc=0x%08x\n",
                   address, size, static_cast<unsigned long long>(value), m_guest.pc);
    }
    m_sc2_update_external_valid = false;
    return true;
  }
  if (m_sc2_update_external_replay_index >= m_sc2_update_external_effects.size())
  {
    if (m_sc2_update_external_valid)
      std::fprintf(stderr,
                   "[sc2-update-effects] mismatch index=%zu actual=write address=0x%08x "
                   "size=%u value=0x%llx reason=journal-exhausted pc=0x%08x\n",
                   m_sc2_update_external_replay_index, address, size,
                   static_cast<unsigned long long>(value), m_guest.pc);
    m_sc2_update_external_valid = false;
    return true;
  }
  const auto& effect = m_sc2_update_external_effects[m_sc2_update_external_replay_index++];
  if (!effect.write || effect.address != address || effect.size != size || effect.value != value)
  {
    if (m_sc2_update_external_valid)
      std::fprintf(stderr,
                   "[sc2-update-effects] mismatch index=%zu expected=%s address=0x%08x "
                   "size=%u value=0x%llx actual=write address=0x%08x size=%u value=0x%llx "
                   "pc=0x%08x\n",
                   m_sc2_update_external_replay_index - 1, effect.write ? "write" : "read",
                   effect.address, effect.size, static_cast<unsigned long long>(effect.value),
                   address, size, static_cast<unsigned long long>(value), m_guest.pc);
    m_sc2_update_external_valid = false;
  }
  return true;
}

void StaticRecompCore::RecordSc2UpdateExternalRead(const u32 address, const u8 size,
                                                    const u64 value)
{
  const bool capture_input_span =
      m_sc2_engine_replay_input_update_span && m_sc2_engine_direct_call_active;
  const bool capture_update_handler =
      m_sc2_engine_indirect_call_active &&
      (m_sc2_engine_indirect_call_target == 0x8001af10 ||
       m_sc2_engine_indirect_call_target == 0x8001f0c0);
  if (!m_sc2_update_external_capture || (!capture_input_span && !capture_update_handler))
    return;
  constexpr std::size_t MAX_EFFECTS = 256;
  if (m_sc2_update_external_effects.size() >= MAX_EFFECTS)
  {
    m_sc2_update_external_valid = false;
    return;
  }
  m_sc2_update_external_effects.push_back({address, value, size, false});
}

void StaticRecompCore::RecordSc2UpdateExternalWrite(const u32 address, const u8 size,
                                                     const u64 value)
{
  const bool capture_input_span =
      m_sc2_engine_replay_input_update_span && m_sc2_engine_direct_call_active;
  const bool capture_update_handler =
      m_sc2_engine_indirect_call_active &&
      (m_sc2_engine_indirect_call_target == 0x8001af10 ||
       m_sc2_engine_indirect_call_target == 0x8001f0c0);
  if (!m_sc2_update_external_capture || (!capture_input_span && !capture_update_handler))
    return;
  constexpr std::size_t MAX_EFFECTS = 256;
  if (m_sc2_update_external_effects.size() >= MAX_EFFECTS)
  {
    m_sc2_update_external_valid = false;
    return;
  }
  m_sc2_update_external_effects.push_back({address, value, size, true});
}

void StaticRecompCore::ReportSc2EngineExternalProfile()
{
  if (m_sc2_engine_external_profile_complete)
    return;

  const u64 fallback_delta =
      m_hook_fallback_instructions - m_sc2_engine_external_entry_fallback_count;
  std::fprintf(stderr,
               "[sc2-engine-external] result reads=%llu writes=%llu read_sites=%zu "
               "write_sites=%zu read_block_sites=%zu write_block_sites=%zu "
               "fallback_instructions=%llu overflow=%s complete=%s\n",
               static_cast<unsigned long long>(m_sc2_engine_external_read_count),
               static_cast<unsigned long long>(m_sc2_engine_external_write_count),
               m_sc2_engine_external_reads.size(), m_sc2_engine_external_writes.size(),
               m_sc2_engine_external_read_blocks.size(),
               m_sc2_engine_external_write_blocks.size(),
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
  const auto report_blocks = [](const char* const kind, const std::map<u64, u64>& sites) {
    for (const auto& [key, count] : sites)
    {
      std::fprintf(stderr,
                   "[sc2-engine-external] %s-block pc=0x%08x address=0x%08x count=%llu\n",
                   kind, static_cast<u32>(key >> 32), static_cast<u32>(key),
                   static_cast<unsigned long long>(count));
    }
  };
  report_blocks("read", m_sc2_engine_external_read_blocks);
  report_blocks("write", m_sc2_engine_external_write_blocks);
  std::fflush(stderr);
  m_sc2_engine_external_profile_complete = true;
}

void StaticRecompCore::ObserveSc2EngineDirectCall(const u32 pc)
{
  if (!m_sc2_engine_external_profile_active || m_guest.ram == nullptr || m_guest.ram_size == 0)
    return;

  constexpr std::size_t PAGE_BYTES = 4096;
  if (m_sc2_engine_direct_call_active)
  {
    if (pc != m_sc2_engine_direct_call_return)
      return;

    const u64 key = (static_cast<u64>(m_sc2_engine_direct_call_return) << 32) |
                    m_sc2_engine_direct_call_target;
    auto& profile = m_sc2_engine_direct_calls[key];
    ++profile.invocations;
    profile.external_reads +=
        m_sc2_engine_external_read_count - m_sc2_engine_direct_call_entry_reads;
    profile.external_writes +=
        m_sc2_engine_external_write_count - m_sc2_engine_direct_call_entry_writes;
    profile.fallback_instructions +=
        m_hook_fallback_instructions - m_sc2_engine_direct_call_entry_fallbacks;
    ++m_sc2_engine_direct_call_completed;
    m_sc2_engine_direct_call_active = false;
    return;
  }

  // These are the exact direct branch-and-link edges in the certified USA DOL
  // function at 0x8001ba3c. Requiring both the target and LR avoids mistaking
  // ordinary branches for calls after LR has retained an earlier return PC.
  constexpr std::array<std::pair<u32, u32>, 39> DIRECT_CALLS{{
      {0x8001f664, 0x8001ba60}, {0x8001f624, 0x8001ba9c}, {0x8002bde0, 0x8001baa8},
      {0x8001aef0, 0x8001bb64}, {0x8001aa7c, 0x8001bb70}, {0x8001fe10, 0x8001bb80},
      {0x80020458, 0x8001bb8c}, {0x8001ac90, 0x8001bb94}, {0x800dda30, 0x8001bbec},
      {0x80011c80, 0x8001bbf8}, {0x801270b8, 0x8001bbfc}, {0x8001aef0, 0x8001bc40},
      {0x8001aa7c, 0x8001bc4c}, {0x800200a8, 0x8001bc5c}, {0x800200a8, 0x8001bc6c},
      {0x8001ac90, 0x8001bc74}, {0x8001aa2c, 0x8001bc7c}, {0x8001aa2c, 0x8001bc9c},
      {0x8012faac, 0x8001bca0}, {0x80065b70, 0x8001bca8}, {0x800095c0, 0x8001bcb0},
      {0x80020348, 0x8001bcc0}, {0x80020348, 0x8001bcd0}, {0x80020348, 0x8001bcf0},
      {0x8001b4b8, 0x8001bcf4}, {0x8001aa58, 0x8001bd04}, {0x80016d14, 0x8001bd5c},
      {0x8002bc50, 0x8001bd68}, {0x8002be14, 0x8001bd74}, {0x80165738, 0x8001bdd4},
      {0x80011c80, 0x8001bdfc}, {0x8001aef0, 0x8001be74}, {0x8001aef0, 0x8001be9c},
      {0x8001aa7c, 0x8001bea8}, {0x8001aa2c, 0x8001bec8}, {0x800095c0, 0x8001bed0},
      {0x8001aa58, 0x8001bee8}, {0x802086e8, 0x8001bf4c}, {0x8000c1f4, 0x8001bf5c},
  }};
  if (std::find(DIRECT_CALLS.begin(), DIRECT_CALLS.end(),
                std::pair{pc, m_guest.lr}) == DIRECT_CALLS.end())
  {
    return;
  }

  m_sc2_engine_direct_call_target = pc;
  m_sc2_engine_direct_call_return = m_guest.lr;
  const u64 key = (static_cast<u64>(m_sc2_engine_direct_call_return) << 32) |
                  m_sc2_engine_direct_call_target;
  auto& profile = m_sc2_engine_direct_calls[key];
  if (profile.written_pages.empty())
  {
    profile.written_pages.assign((m_guest.ram_size + PAGE_BYTES - 1) / PAGE_BYTES, false);
    profile.written_bytes.assign(m_guest.ram_size, false);
  }
  m_sc2_engine_direct_call_entry_reads = m_sc2_engine_external_read_count;
  m_sc2_engine_direct_call_entry_writes = m_sc2_engine_external_write_count;
  m_sc2_engine_direct_call_entry_fallbacks = m_hook_fallback_instructions;
  m_sc2_engine_direct_call_active = true;
}

void StaticRecompCore::Sc2EngineDirectCallWriteTrampoline(const u32 offset, const u32 size,
                                                           void* const user)
{
  auto* const core = static_cast<StaticRecompCore*>(user);
  if ((!core->m_sc2_engine_direct_call_active && !core->m_sc2_engine_speculative_active) ||
      size == 0 ||
      offset >= core->m_guest.ram_size)
  {
    return;
  }
  if (core->m_sc2_engine_undo_ring &&
      !core->m_sc2_engine_undo_ring->RecordWrite(
          offset, size, std::span<const u8>{core->m_guest.ram, core->m_guest.ram_size}))
  {
    core->m_sc2_engine_direct_call_overflow = true;
    core->m_sc2_update_external_valid = false;
    return;
  }
  constexpr u32 PAGE_BYTES = 4096;
  auto& write_blocks = core->m_sc2_engine_speculative_active ?
                           core->m_sc2_update_replay_write_blocks :
                           core->m_sc2_update_original_write_blocks;
  const u64 write_key = (static_cast<u64>(core->m_guest.pc) << 32) |
                        ((offset / PAGE_BYTES) * PAGE_BYTES);
  ++write_blocks[write_key];
  const u32 last_offset = std::min<u32>(core->m_guest.ram_size - 1, offset + size - 1);
  if (core->m_sc2_engine_speculative_active)
  {
    for (u32 byte = offset; byte <= last_offset; ++byte)
    {
      if (!core->m_sc2_update_replay_written_bytes[byte])
      {
        core->m_sc2_update_replay_written_bytes[byte] = true;
        core->m_sc2_update_replay_written_offsets.push_back(byte);
      }
    }
  }
  if (!core->m_sc2_engine_direct_call_active)
    return;
  const u64 key = (static_cast<u64>(core->m_sc2_engine_direct_call_return) << 32) |
                  core->m_sc2_engine_direct_call_target;
  auto& direct_profile = core->m_sc2_engine_direct_calls[key];
  if (core->m_sc2_transaction_history_enabled && !core->m_sc2_engine_replay_probe_enabled)
  {
    // The continuous transaction route only needs exact sparse postimages for
    // the intercepted indirect system handlers below. Avoid tracking the root
    // transaction twice: its complete preimage already lives in the undo ring.
  }
  else
  {
    auto& pages = direct_profile.written_pages;
    auto& bytes = direct_profile.written_bytes;
    auto& offsets = direct_profile.written_offsets;
    for (u32 page = offset / PAGE_BYTES; page <= last_offset / PAGE_BYTES; ++page)
      pages[page] = true;
    for (u32 byte = offset; byte <= last_offset; ++byte)
    {
      if (!core->m_sc2_engine_speculative_active)
        core->m_sc2_update_original_last_writer[byte] = core->m_guest.pc;
      if (!bytes[byte])
      {
        bytes[byte] = true;
        offsets.push_back(byte);
      }
    }
  }

  if (core->m_sc2_engine_indirect_call_active)
  {
    const u64 indirect_key =
        (static_cast<u64>(core->m_sc2_engine_indirect_call_return) << 32) |
        core->m_sc2_engine_indirect_call_target;
    auto& indirect_profile = core->m_sc2_engine_indirect_calls[indirect_key];
    if (core->m_sc2_transaction_history_enabled && !core->m_sc2_engine_replay_probe_enabled)
    {
      for (u32 byte = offset; byte <= last_offset; ++byte)
      {
        if (indirect_profile.sparse_written_offsets.insert(byte).second)
          indirect_profile.written_offsets.push_back(byte);
      }
    }
    else
    {
      auto& indirect_pages = indirect_profile.written_pages;
      auto& indirect_bytes = indirect_profile.written_bytes;
      auto& indirect_offsets = indirect_profile.written_offsets;
      for (u32 page = offset / PAGE_BYTES; page <= last_offset / PAGE_BYTES; ++page)
        indirect_pages[page] = true;
      for (u32 byte = offset; byte <= last_offset; ++byte)
      {
        if (!indirect_bytes[byte])
        {
          indirect_bytes[byte] = true;
          indirect_offsets.push_back(byte);
        }
      }
    }
  }
}

void StaticRecompCore::ObserveSc2EngineIndirectCall(const u32 pc)
{
  if (!m_sc2_engine_external_profile_active || !m_sc2_engine_direct_call_active ||
      (m_sc2_engine_direct_call_target != 0x800095c0 &&
       m_sc2_engine_direct_call_target != 0x80011c80))
  {
    return;
  }

  if (m_sc2_engine_indirect_call_active)
  {
    if (pc != m_sc2_engine_indirect_call_return)
      return;
    const u64 key = (static_cast<u64>(m_sc2_engine_indirect_call_return) << 32) |
                    m_sc2_engine_indirect_call_target;
    auto& profile = m_sc2_engine_indirect_calls[key];
    ++profile.invocations;
    profile.external_reads +=
        m_sc2_engine_external_read_count - m_sc2_engine_indirect_call_entry_reads;
    profile.external_writes +=
        m_sc2_engine_external_write_count - m_sc2_engine_indirect_call_entry_writes;
    profile.fallback_instructions +=
        m_hook_fallback_instructions - m_sc2_engine_indirect_call_entry_fallbacks;
    if (m_sc2_update_external_capture &&
        (m_sc2_engine_indirect_call_target == 0x8001af10 ||
         m_sc2_engine_indirect_call_target == 0x8001f0c0))
    {
      auto& postimage = m_sc2_update_handler_post_ram[key];
      postimage.clear();
      postimage.reserve(profile.written_offsets.size());
      for (const u32 offset : profile.written_offsets)
        postimage.emplace_back(offset, m_guest.ram[offset]);
      m_sc2_update_handler_post_guest[key] = m_guest;
      m_sc2_update_handler_post_tb_remainder[key] = m_tb_cycle_remainder;
      m_sc2_update_handler_effect_ranges[key] = {
          m_sc2_engine_indirect_call_entry_effect, m_sc2_update_external_effects.size()};
    }
    ++m_sc2_engine_indirect_call_completed;
    m_sc2_engine_indirect_call_active = false;
    return;
  }

  constexpr std::array<u32, 7> INDIRECT_RETURNS{
      0x800097f4, 0x8000981c, 0x8000988c, 0x800098ac,
      0x800098cc, 0x80009920, 0x80009940,
  };
  if (std::find(INDIRECT_RETURNS.begin(), INDIRECT_RETURNS.end(), m_guest.lr) ==
          INDIRECT_RETURNS.end() ||
      (pc >= 0x800095c0 && pc < 0x8000a000))
  {
    return;
  }
  constexpr std::size_t MAX_INDIRECT_SITES = 2048;
  const u64 key = (static_cast<u64>(m_guest.lr) << 32) | pc;
  if (!m_sc2_engine_indirect_calls.contains(key) &&
      m_sc2_engine_indirect_calls.size() >= MAX_INDIRECT_SITES)
  {
    m_sc2_engine_indirect_call_overflow = true;
    return;
  }
  auto& profile = m_sc2_engine_indirect_calls[key];
  if (m_sc2_transaction_history_enabled && !m_sc2_engine_replay_probe_enabled)
  {
    profile.sparse_written_offsets.reserve(4096);
  }
  else if (profile.written_pages.empty())
  {
    profile.written_pages.assign((m_guest.ram_size + 4095) / 4096, false);
    profile.written_bytes.assign(m_guest.ram_size, false);
  }
  m_sc2_engine_indirect_call_target = pc;
  m_sc2_engine_indirect_call_return = m_guest.lr;
  m_sc2_engine_indirect_call_entry_reads = m_sc2_engine_external_read_count;
  m_sc2_engine_indirect_call_entry_writes = m_sc2_engine_external_write_count;
  m_sc2_engine_indirect_call_entry_fallbacks = m_hook_fallback_instructions;
  m_sc2_engine_indirect_call_entry_effect = m_sc2_update_external_effects.size();
  m_sc2_engine_indirect_call_active = true;
}

bool StaticRecompCore::ReplaySc2UpdateSystemHandler(const u32 pc)
{
  if (!m_sc2_engine_speculative_active ||
      (pc != 0x8001af10 && pc != 0x8001f0c0) || m_guest.lr != 0x8000988c)
  {
    return false;
  }
  const u64 key = (static_cast<u64>(m_guest.lr) << 32) | pc;
  const auto post_ram = m_sc2_update_handler_post_ram.find(key);
  const auto post_guest = m_sc2_update_handler_post_guest.find(key);
  const auto post_tb = m_sc2_update_handler_post_tb_remainder.find(key);
  const auto effect_range = m_sc2_update_handler_effect_ranges.find(key);
  if (post_ram == m_sc2_update_handler_post_ram.end() ||
      post_guest == m_sc2_update_handler_post_guest.end() ||
      post_tb == m_sc2_update_handler_post_tb_remainder.end() ||
      effect_range == m_sc2_update_handler_effect_ranges.end())
  {
    m_sc2_update_external_valid = false;
    std::fprintf(stderr,
                 "[sc2-update-handler] missing captured postimage target=0x%08x\n", pc);
    return false;
  }
  for (const auto [offset, value] : post_ram->second)
  {
    if (m_sc2_transaction_replay_active &&
        !m_sc2_transaction_store->RecordWrite(
            offset, 1, std::span<const u8>{m_guest.ram, m_guest.ram_size}))
    {
      m_sc2_update_external_valid = false;
      std::fprintf(stderr,
                   "[sc2-update-handler] failed to journal postimage target=0x%08x\n", pc);
      return false;
    }
    m_guest.ram[offset] = value;
  }
  const auto [effect_begin, effect_end] = effect_range->second;
  if (effect_begin > effect_end || effect_end > m_sc2_update_external_effects.size())
  {
    m_sc2_update_external_valid = false;
  }
  else if (m_sc2_update_external_keyed_replay)
  {
    for (std::size_t index = effect_begin; index < effect_end; ++index)
    {
      if (!m_sc2_update_external_replay_used[index])
      {
        m_sc2_update_external_replay_used[index] = true;
        ++m_sc2_update_external_replay_index;
      }
    }
  }
  else if (m_sc2_update_external_replay_index > effect_begin)
  {
    m_sc2_update_external_valid = false;
  }
  else
  {
    m_sc2_update_external_replay_index = effect_end;
  }
  u8* const ram = m_guest.ram;
  const u32 ram_size = m_guest.ram_size;
  u8* const mem2 = m_guest.mem2;
  const u32 mem2_size = m_guest.mem2_size;
  m_guest = post_guest->second;
  m_guest.ram = ram;
  m_guest.ram_size = ram_size;
  m_guest.mem2 = mem2;
  m_guest.mem2_size = mem2_size;
  m_tb_cycle_remainder = post_tb->second;
  std::fprintf(stderr,
               "[sc2-update-handler] replayed target=0x%08x bytes=%zu effects=%zu\n",
               pc, post_ram->second.size(), effect_end - effect_begin);
  return true;
}

void StaticRecompCore::ReportSc2EngineDirectCallProfile()
{
  if (m_sc2_engine_set_mem_journal)
    m_sc2_engine_set_mem_journal(nullptr, nullptr);
  if (StaticRecompLockstep::g_ram_write_journal_user == this)
  {
    StaticRecompLockstep::g_ram_write_journal = nullptr;
    StaticRecompLockstep::g_ram_write_journal_user = nullptr;
  }
  if (m_sc2_engine_direct_call_active)
  {
    m_sc2_engine_direct_call_overflow = true;
    m_sc2_engine_direct_call_active = false;
  }
  std::fprintf(stderr,
               "[sc2-engine-calls] result sites=%zu completed=%llu overflow=%s complete=%s\n",
               m_sc2_engine_direct_calls.size(),
               static_cast<unsigned long long>(m_sc2_engine_direct_call_completed),
               m_sc2_engine_direct_call_overflow ? "yes" : "no",
               m_sc2_engine_direct_call_overflow ? "no" : "yes");
  for (const auto& [key, profile] : m_sc2_engine_direct_calls)
  {
    const std::size_t written_pages =
        static_cast<std::size_t>(std::count(profile.written_pages.begin(),
                                           profile.written_pages.end(), true));
    std::fprintf(stderr,
                 "[sc2-engine-calls] callsite=0x%08x target=0x%08x invocations=%llu "
                 "written_pages=%zu external_reads=%llu external_writes=%llu "
                 "fallback_instructions=%llu\n",
                 static_cast<u32>(key >> 32) - 4, static_cast<u32>(key),
                 static_cast<unsigned long long>(profile.invocations), written_pages,
                 static_cast<unsigned long long>(profile.external_reads),
                 static_cast<unsigned long long>(profile.external_writes),
                 static_cast<unsigned long long>(profile.fallback_instructions));
  }
  std::fflush(stderr);
}

void StaticRecompCore::ReportSc2EngineIndirectCallProfile()
{
  if (m_sc2_engine_indirect_call_active)
  {
    m_sc2_engine_indirect_call_overflow = true;
    m_sc2_engine_indirect_call_active = false;
  }
  std::fprintf(stderr,
               "[sc2-engine-indirect] result sites=%zu completed=%llu overflow=%s complete=%s\n",
               m_sc2_engine_indirect_calls.size(),
               static_cast<unsigned long long>(m_sc2_engine_indirect_call_completed),
               m_sc2_engine_indirect_call_overflow ? "yes" : "no",
               m_sc2_engine_indirect_call_overflow ? "no" : "yes");
  for (const auto& [key, profile] : m_sc2_engine_indirect_calls)
  {
    const std::size_t written_pages =
        static_cast<std::size_t>(std::count(profile.written_pages.begin(),
                                           profile.written_pages.end(), true));
    std::fprintf(stderr,
                 "[sc2-engine-indirect] callsite=0x%08x target=0x%08x invocations=%llu "
                 "written_pages=%zu external_reads=%llu external_writes=%llu "
                 "fallback_instructions=%llu\n",
                 static_cast<u32>(key >> 32) - 4, static_cast<u32>(key),
                 static_cast<unsigned long long>(profile.invocations), written_pages,
                 static_cast<unsigned long long>(profile.external_reads),
                 static_cast<unsigned long long>(profile.external_writes),
                 static_cast<unsigned long long>(profile.fallback_instructions));
  }
  std::fflush(stderr);
}

void StaticRecompCore::ProbeSc2EngineReplay(const u32 pc)
{
  const u32 replay_begin_pc = m_sc2_engine_replay_input_update_span ? 0x80011c80 :
                              m_sc2_engine_replay_update_call ? 0x800095c0 : 0x8001ba3c;
  const u32 replay_return_pc = m_sc2_engine_replay_update_call ? 0x8001bcb0 : 0x8002d628;
  if (!m_sc2_engine_replay_probe_enabled || m_sc2_engine_replay_completed ||
      m_guest.ram == nullptr || m_guest.ram_size == 0)
  {
    return;
  }

  // The original root call copied each resolved SI result into these raw
  // slots. During selective replay the MMIO reads are transaction-replayed so
  // they cannot poll the rollback scheduler again. Consume the bounded input
  // journal here and replace the just-copied slot before SC2's own controller
  // conversion reads it.
  InjectSc2TransactionReplayInput(pc);

  if (ReplaySc2UpdateSystemHandler(pc))
    return;

  ObserveSc2EngineIndirectCall(pc);
  ObserveSc2EngineDirectCall(pc);

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
  if (pc == replay_begin_pc && !m_sc2_engine_replay_have_entry)
  {
    const u32 expected_entry_lr =
        m_sc2_engine_replay_input_update_span ? 0x8001bbf8 : replay_return_pc;
    if (m_sc2_engine_replay_update_call && m_guest.lr != expected_entry_lr)
      return;
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
      if (m_sc2_engine_replay_selective_update)
      {
        m_sc2_engine_replay_entry_ram.assign(m_guest.ram, m_guest.ram + m_guest.ram_size);
      }
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
      m_sc2_update_external_effects.clear();
      m_sc2_update_original_write_blocks.clear();
      m_sc2_update_original_last_writer.clear();
      m_sc2_update_replay_write_blocks.clear();
      m_sc2_update_replay_written_bytes.assign(m_guest.ram_size, false);
      m_sc2_update_replay_written_offsets.clear();
      m_sc2_update_handler_post_ram.clear();
      m_sc2_update_handler_post_guest.clear();
      m_sc2_update_handler_post_tb_remainder.clear();
      m_sc2_update_handler_effect_ranges.clear();
      m_sc2_input_handler_post_offsets.clear();
      m_sc2_input_handler_post_values.clear();
      m_sc2_input_handler_have_post = false;
      m_sc2_update_external_replay_index = 0;
      m_sc2_update_external_valid = true;
      m_sc2_update_external_capture = m_sc2_engine_replay_selective_update;
      m_sc2_update_external_replay = false;
      m_sc2_engine_external_reads.clear();
      m_sc2_engine_external_writes.clear();
      m_sc2_engine_external_read_blocks.clear();
      m_sc2_engine_external_write_blocks.clear();
      m_sc2_engine_direct_calls.clear();
      m_sc2_engine_direct_call_completed = 0;
      m_sc2_engine_direct_call_active = m_sc2_engine_replay_update_call;
      m_sc2_engine_direct_call_overflow = false;
      if (m_sc2_engine_replay_selective_update)
      {
        constexpr std::size_t MAX_SELECTIVE_UNIQUE_BYTES = 1024 * 1024;
        m_sc2_engine_undo_ring = std::make_unique<NetPlay::RollbackUndoSnapshotRing>(
            m_guest.ram_size, 1, MAX_SELECTIVE_UNIQUE_BYTES);
        if (m_sc2_engine_undo_ring->GetConfigurationStatus() !=
                NetPlay::RollbackUndoSnapshotRing::ConfigurationStatus::Valid ||
            !m_sc2_engine_undo_ring->BeginFrame(
                1, std::span<const u8>{m_guest.ram, m_guest.ram_size}))
        {
          m_sc2_engine_direct_call_overflow = true;
          m_sc2_update_external_valid = false;
        }
      }
      if (m_sc2_engine_replay_update_call)
      {
        m_sc2_engine_direct_call_target = replay_begin_pc;
        m_sc2_engine_direct_call_return = replay_return_pc;
        auto& update_profile = m_sc2_engine_direct_calls[
            (static_cast<u64>(replay_return_pc) << 32) | replay_begin_pc];
        update_profile.written_pages.assign((m_guest.ram_size + 4095) / 4096, false);
        update_profile.written_bytes.assign(m_guest.ram_size, false);
      }
      m_sc2_engine_indirect_calls.clear();
      m_sc2_engine_indirect_call_completed = 0;
      m_sc2_engine_indirect_call_active = false;
      m_sc2_engine_indirect_call_overflow = false;
      m_sc2_engine_set_mem_journal = reinterpret_cast<Sc2EngineSetMemJournalFn>(
          m_library.GetSymbolAddress("ppc_set_mem_write_journal"));
      if (!m_sc2_engine_set_mem_journal || m_lockstep_verifier->IsEnabled() || m_watch_armed)
      {
        m_sc2_engine_direct_call_overflow = true;
      }
      else
      {
        m_sc2_engine_set_mem_journal(&StaticRecompCore::Sc2EngineDirectCallWriteTrampoline,
                                     this);
        // The generated module journal is sufficient for this native-only SC2
        // update (the profiler separately rejects fallback instructions).
        // Dolphin-side writes can be asynchronous DSP/device DMA and must not
        // become part of the game-state rewind set.
        if (!m_sc2_engine_replay_selective_update)
        {
          StaticRecompLockstep::g_ram_write_journal =
              &StaticRecompCore::Sc2EngineDirectCallWriteTrampoline;
          StaticRecompLockstep::g_ram_write_journal_user = this;
        }
      }
      m_sc2_engine_external_read_count = 0;
      m_sc2_engine_external_write_count = 0;
      m_sc2_engine_external_entry_fallback_count = m_hook_fallback_instructions;
      if (m_sc2_engine_replay_update_call)
      {
        m_sc2_engine_direct_call_entry_reads = 0;
        m_sc2_engine_direct_call_entry_writes = 0;
        m_sc2_engine_direct_call_entry_fallbacks = m_hook_fallback_instructions;
      }
      m_sc2_engine_external_profile_overflow = false;
      m_sc2_engine_external_profile_active = true;
    }
    std::fprintf(stderr,
                 "[sc2-engine-replay] captured ram_bytes=%u l1_bytes=%zu state_bytes=%zu "
                 "lr=0x%08x\n",
                 m_guest.ram_size, l1_size, m_sc2_engine_replay_entry_state_size, m_guest.lr);
    return;
  }

  if (m_sc2_engine_replay_input_update_span && !m_sc2_engine_replay_replaying &&
      m_sc2_engine_replay_have_entry && !m_sc2_input_handler_have_post && pc == 0x8001bbf8)
  {
    for (u32 offset = 0; offset < m_guest.ram_size; ++offset)
    {
      if (m_sc2_engine_replay_entry_ram[offset] == m_guest.ram[offset])
        continue;
      m_sc2_input_handler_post_offsets.push_back(offset);
      m_sc2_input_handler_post_values.push_back(m_guest.ram[offset]);
    }
    m_sc2_input_handler_post_guest = m_guest;
    m_sc2_input_handler_post_tb_remainder = m_tb_cycle_remainder;
    m_sc2_input_handler_external_effect_count = m_sc2_update_external_effects.size();
    m_sc2_input_handler_have_post = true;
    std::fprintf(stderr,
                 "[sc2-input-handler] captured bytes=%zu return_pc=0x8001bbf8\n",
                 m_sc2_input_handler_post_offsets.size());
  }

  if (m_sc2_engine_replay_input_update_span && m_sc2_engine_replay_replaying &&
      pc == 0x8001bbf8)
  {
    std::size_t hardware_bytes = 0;
    for (std::size_t i = 0; i < m_sc2_input_handler_post_offsets.size(); ++i)
    {
      const u32 offset = m_sc2_input_handler_post_offsets[i];
      const auto writer = m_sc2_update_original_last_writer.find(offset);
      if (writer == m_sc2_update_original_last_writer.end() || writer->second < 0x801bd000 ||
          writer->second >= 0x801c1000)
      {
        continue;
      }
      m_guest.ram[offset] = m_sc2_input_handler_post_values[i];
      ++hardware_bytes;
    }
    if (m_sc2_update_external_replay_index <= m_sc2_input_handler_external_effect_count)
      m_sc2_update_external_replay_index = m_sc2_input_handler_external_effect_count;
    else
      m_sc2_update_external_valid = false;
    std::fprintf(stderr,
                 "[sc2-input-handler] re-anchored system_bytes=%zu effects=%zu/%zu "
                 "return_pc=0x8001bbf8\n",
                 hardware_bytes, m_sc2_update_external_replay_index,
                 m_sc2_update_external_effects.size());
  }

  if (pc != replay_return_pc || !m_sc2_engine_replay_have_entry)
    return;

  ++m_sc2_engine_replay_pass_returns;
  if (m_sc2_engine_replay_pass_returns < m_sc2_engine_replay_tick_span)
    return;
  m_sc2_engine_replay_pass_returns = 0;

  if (!m_sc2_engine_replay_replaying)
  {
    if (m_sc2_engine_replay_full_emulator)
    {
      if (m_sc2_engine_replay_corrected_input)
      {
        m_sc2_engine_replay_original_endpoint_guest = m_guest;
        m_sc2_engine_replay_original_endpoint_tb_remainder = m_tb_cycle_remainder;
        m_sc2_engine_replay_original_endpoint_ram.assign(
            m_guest.ram, m_guest.ram + m_guest.ram_size);
        if (!capture_full_emulator_state(m_sc2_engine_replay_original_endpoint_state,
                                         m_sc2_engine_replay_original_endpoint_state_size))
        {
          std::fprintf(stderr,
                       "[sc2-engine-replay] failed original endpoint capture\n");
          m_sc2_engine_replay_completed = true;
          return;
        }
        m_guest = m_sc2_engine_replay_original_endpoint_guest;
        m_tb_cycle_remainder = m_sc2_engine_replay_original_endpoint_tb_remainder;
      }
      m_sc2_update_external_capture = false;
      m_sc2_engine_external_profile_active = false;
      ReportSc2EngineExternalProfile();
      ReportSc2EngineDirectCallProfile();
      ReportSc2EngineIndirectCallProfile();
      // A load intentionally dirties renderer caches. Comparing the original
      // endpoint with a replayed endpoint would therefore compare a no-load
      // path with a post-load path. Discard this first pass and compare two
      // passes which both begin from the same restored entry snapshot.
      m_sc2_engine_replay_input_capture_valid =
          NetPlay::FinishSc2EngineInputCapture(&m_sc2_engine_replay_input_polls,
                                               &m_sc2_engine_replay_input_batches);
      std::fprintf(stderr, "[sc2-engine-replay] input-batches count=%zu",
                   m_sc2_engine_replay_input_batches.size());
      for (const u64 batch : m_sc2_engine_replay_input_batches)
        std::fprintf(stderr, " %llu", static_cast<unsigned long long>(batch));
      std::fprintf(stderr, "\n");
      if (m_sc2_engine_replay_selective_update)
      {
        m_sc2_engine_replay_endpoint_guest = m_guest;
        m_sc2_engine_replay_endpoint_tb_remainder = m_tb_cycle_remainder;
        m_sc2_engine_replay_endpoint_ram.assign(m_guest.ram,
                                                m_guest.ram + m_guest.ram_size);
        m_sc2_engine_replay_endpoint_l1.assign(l1, l1 + l1_size);
        if (!m_sc2_engine_replay_input_capture_valid || !m_sc2_update_external_valid ||
            !capture_full_emulator_state(m_sc2_engine_replay_endpoint_state,
                                         m_sc2_engine_replay_endpoint_state_size))
        {
          std::fprintf(stderr,
                       "[sc2-engine-replay] failed selective reference capture\n");
          m_sc2_engine_replay_completed = true;
          return;
        }
        const u64 update_key = (static_cast<u64>(replay_return_pc) << 32) | replay_begin_pc;
        const auto update_profile = m_sc2_engine_direct_calls.find(update_key);
        if (update_profile == m_sc2_engine_direct_calls.end())
        {
          std::fprintf(stderr, "[sc2-engine-replay] missing selective update profile\n");
          m_sc2_engine_replay_completed = true;
          return;
        }
        const std::optional<std::size_t> sparse_bytes =
            m_sc2_engine_undo_ring ? m_sc2_engine_undo_ring->GetUniqueBytes(1) : std::nullopt;
        if (!sparse_bytes ||
            !m_sc2_engine_undo_ring->RestoreFrame(
                1, std::span<u8>{m_guest.ram, m_guest.ram_size}))
        {
          std::fprintf(stderr, "[sc2-engine-replay] sparse entry restore failed\n");
          m_sc2_engine_replay_completed = true;
          return;
        }
        m_guest = m_sc2_engine_replay_entry_guest;
        if (m_sc2_engine_replay_input_update_span)
        {
          if (!NetPlay::BeginSc2EngineInputReplay(m_sc2_engine_replay_corrected_input))
          {
            std::fprintf(stderr,
                         "[sc2-engine-replay] failed selective input replay start\n");
            m_sc2_engine_replay_completed = true;
            return;
          }
          std::fprintf(stderr,
                       "[sc2-input-handler] replaying root with captured hardware effects\n");
        }
        m_sc2_update_external_replay_index = 0;
        m_sc2_update_external_replay = true;
        m_sc2_engine_speculative_active = true;
        if (m_sc2_engine_set_mem_journal)
        {
          m_sc2_engine_set_mem_journal(&StaticRecompCore::Sc2EngineDirectCallWriteTrampoline,
                                       this);
          if (!m_sc2_engine_replay_selective_update)
          {
            StaticRecompLockstep::g_ram_write_journal =
                &StaticRecompCore::Sc2EngineDirectCallWriteTrampoline;
            StaticRecompLockstep::g_ram_write_journal_user = this;
          }
        }
        m_sc2_engine_replay_have_reference = true;
        m_sc2_engine_replay_replaying = true;
        std::fprintf(stderr,
                     "[sc2-engine-replay] selective restore bytes=%zu journal_bytes=%zu "
                     "pages=%zu effects=%zu; "
                     "replaying update with hardware held at canonical frontier\n",
                     update_profile->second.written_offsets.size(),
                     *sparse_bytes,
                     static_cast<std::size_t>(std::count(
                         update_profile->second.written_pages.begin(),
                         update_profile->second.written_pages.end(), true)),
                     m_sc2_update_external_effects.size());
        return;
      }
      if (!m_sc2_engine_replay_input_capture_valid ||
          !restore_full_emulator_state(m_sc2_engine_replay_entry_state,
                                       m_sc2_engine_replay_entry_state_size) ||
          !NetPlay::BeginSc2EngineInputReplay(m_sc2_engine_replay_corrected_input))
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
    if (m_sc2_engine_replay_selective_update && m_sc2_engine_replay_have_reference)
    {
      m_guest.timebase = m_sc2_engine_replay_endpoint_guest.timebase;
      m_tb_cycle_remainder = m_sc2_engine_replay_endpoint_tb_remainder;
    }
    const CPUState replayed_guest = m_guest;
    const u64 replayed_tb_remainder = m_tb_cycle_remainder;
    if (m_sc2_engine_replay_selective_update && m_sc2_engine_replay_have_reference)
    {
      const u64 update_key = (static_cast<u64>(replay_return_pc) << 32) | replay_begin_pc;
      const auto update_profile = m_sc2_engine_direct_calls.find(update_key);
      if (update_profile == m_sc2_engine_direct_calls.end())
      {
        std::fprintf(stderr,
                     "[sc2-engine-replay] missing selective update profile at endpoint\n");
        m_sc2_engine_replay_completed = true;
        return;
      }

      std::vector<std::pair<u32, u8>> game_postimage;
      game_postimage.reserve(update_profile->second.written_offsets.size() +
                             m_sc2_update_replay_written_offsets.size());
      for (const u32 offset : update_profile->second.written_offsets)
      {
        const auto writer = m_sc2_update_original_last_writer.find(offset);
        const bool system_owned = writer != m_sc2_update_original_last_writer.end() &&
                                  writer->second >= 0x801bd000 &&
                                  writer->second < 0x801c1000;
        if (!system_owned)
          game_postimage.emplace_back(offset, m_guest.ram[offset]);
      }
      const std::size_t original_game_bytes = game_postimage.size();
      for (const u32 offset : m_sc2_update_replay_written_offsets)
      {
        if (!update_profile->second.written_bytes[offset])
          game_postimage.emplace_back(offset, m_guest.ram[offset]);
      }

      // Re-anchor every non-game subsystem at the canonical endpoint. This
      // discards DSP/device work which raced with speculative execution while
      // preserving all original and extra replay game writes for comparison.
      // Re-capture after the load so both serialized endpoints have identical
      // post-load renderer/cache normalization.
      if (!restore_full_emulator_state(m_sc2_engine_replay_endpoint_state,
                                       m_sc2_engine_replay_endpoint_state_size) ||
          !capture_full_emulator_state(m_sc2_engine_replay_endpoint_state,
                                       m_sc2_engine_replay_endpoint_state_size))
      {
        std::fprintf(stderr,
                     "[sc2-engine-replay] failed to re-anchor canonical hardware endpoint\n");
        m_sc2_engine_replay_completed = true;
        return;
      }
      for (const auto [offset, value] : game_postimage)
        m_guest.ram[offset] = value;
      u8* const ram = m_guest.ram;
      const u32 ram_size = m_guest.ram_size;
      u8* const mem2 = m_guest.mem2;
      const u32 mem2_size = m_guest.mem2_size;
      m_guest = replayed_guest;
      m_guest.ram = ram;
      m_guest.ram_size = ram_size;
      m_guest.mem2 = mem2;
      m_guest.mem2_size = mem2_size;
      m_tb_cycle_remainder = replayed_tb_remainder;
      std::fprintf(stderr,
                   "[sc2-engine-replay] selective transaction game_bytes=%zu "
                   "extra_replay_bytes=%zu; canonical hardware re-anchored\n",
                   original_game_bytes, game_postimage.size() - original_game_bytes);

      if (m_sc2_engine_replay_input_update_span && m_sc2_engine_replay_corrected_input &&
          !m_sc2_engine_replay_selective_corrected_reference)
      {
        m_sc2_engine_replay_corrected_state_bytes = 0;
        for (const u32 offset : update_profile->second.written_offsets)
        {
          const auto writer = m_sc2_update_original_last_writer.find(offset);
          const bool system_owned = writer != m_sc2_update_original_last_writer.end() &&
                                    writer->second >= 0x801bd000 &&
                                    writer->second < 0x801c1000;
          if (!system_owned && m_sc2_engine_replay_endpoint_ram[offset] != m_guest.ram[offset])
            ++m_sc2_engine_replay_corrected_state_bytes;
        }

        if (m_sc2_engine_replay_corrected_state_bytes == 0 ||
            !NetPlay::FinishSc2EngineInputReplay(
                &m_sc2_engine_replay_reference_perturbed_polls))
        {
          NetPlay::EndSc2EngineInputReplay();
          std::fprintf(stderr,
                       "[sc2-engine-replay] selective corrected input produced no game "
                       "change or invalid poll replay\n");
          m_sc2_engine_replay_completed = true;
          return;
        }

        m_sc2_engine_replay_endpoint_guest = m_guest;
        m_sc2_engine_replay_endpoint_tb_remainder = m_tb_cycle_remainder;
        m_sc2_engine_replay_endpoint_ram.assign(m_guest.ram, m_guest.ram + m_guest.ram_size);
        m_sc2_engine_replay_endpoint_l1.assign(l1, l1 + l1_size);
        if (!capture_full_emulator_state(m_sc2_engine_replay_endpoint_state,
                                         m_sc2_engine_replay_endpoint_state_size))
        {
          std::fprintf(stderr,
                       "[sc2-engine-replay] failed selective corrected reference capture\n");
          m_sc2_engine_replay_completed = true;
          return;
        }
        m_guest = m_sc2_engine_replay_endpoint_guest;
        m_tb_cycle_remainder = m_sc2_engine_replay_endpoint_tb_remainder;

        // The canonical endpoint load above intentionally replaced the RAM
        // lineage which the sparse undo log describes. Reset the oracle from
        // its retained entry postimage rather than applying an undo log across
        // that unrelated full-state load. The live selective driver keeps one
        // direct transaction lineage and therefore does not need this oracle-
        // only reset.
        for (const u32 offset : update_profile->second.written_offsets)
          m_guest.ram[offset] = m_sc2_engine_replay_entry_ram[offset];
        const std::size_t oracle_reset_bytes = update_profile->second.written_offsets.size();
        m_sc2_engine_undo_ring.reset();
        u8* const entry_ram = m_guest.ram;
        const u32 entry_ram_size = m_guest.ram_size;
        u8* const entry_mem2 = m_guest.mem2;
        const u32 entry_mem2_size = m_guest.mem2_size;
        m_guest = m_sc2_engine_replay_entry_guest;
        m_guest.ram = entry_ram;
        m_guest.ram_size = entry_ram_size;
        m_guest.mem2 = entry_mem2;
        m_guest.mem2_size = entry_mem2_size;
        m_tb_cycle_remainder = m_sc2_engine_replay_entry_tb_remainder;
        m_sc2_update_external_replay_index = 0;
        m_sc2_update_external_valid = true;
        m_sc2_update_replay_write_blocks.clear();
        std::fill(m_sc2_update_replay_written_bytes.begin(),
                  m_sc2_update_replay_written_bytes.end(), false);
        m_sc2_update_replay_written_offsets.clear();
        if (!NetPlay::BeginSc2EngineInputReplay(true))
        {
          std::fprintf(stderr,
                       "[sc2-engine-replay] failed selective corrected verification start\n");
          m_sc2_engine_replay_completed = true;
          return;
        }
        m_sc2_engine_replay_selective_corrected_reference = true;
        std::fprintf(stderr,
                     "[sc2-engine-replay] selective corrected reference game_bytes=%zu "
                     "oracle_reset_bytes=%zu perturbed_polls=%zu; restored entry for verification "
                     "replay\n",
                     m_sc2_engine_replay_corrected_state_bytes,
                     oracle_reset_bytes,
                     m_sc2_engine_replay_reference_perturbed_polls);
        return;
      }
    }
    if (!m_sc2_engine_replay_have_reference)
    {
      if (m_sc2_engine_replay_corrected_input)
      {
        std::vector<bool> compared(m_guest.ram_size, false);
        std::vector<u32> corrected_offsets;
        m_sc2_engine_replay_corrected_state_bytes = 0;
        for (const auto& [key, profile] : m_sc2_engine_direct_calls)
        {
          for (const u32 offset : profile.written_offsets)
          {
            if (compared[offset])
              continue;
            compared[offset] = true;
            if (m_sc2_engine_replay_original_endpoint_ram[offset] != m_guest.ram[offset])
            {
              ++m_sc2_engine_replay_corrected_state_bytes;
              if (corrected_offsets.size() < 64)
                corrected_offsets.push_back(offset);
            }
          }
        }
        std::fprintf(stderr,
                     "[sc2-engine-replay] corrected-input-effect game_bytes=%zu\n",
                     m_sc2_engine_replay_corrected_state_bytes);
        for (const u32 offset : corrected_offsets)
        {
          std::fprintf(stderr,
                       "[sc2-engine-replay] corrected-input-byte offset=0x%08x "
                       "original=0x%02x corrected=0x%02x\n",
                       offset, m_sc2_engine_replay_original_endpoint_ram[offset],
                       m_guest.ram[offset]);
          const auto writer = m_sc2_update_original_last_writer.find(offset);
          if (writer != m_sc2_update_original_last_writer.end())
          {
            std::fprintf(stderr,
                         "[sc2-engine-replay] corrected-input-writer offset=0x%08x "
                         "pc=0x%08x\n",
                         offset, writer->second);
          }
        }
        if (m_sc2_engine_replay_corrected_state_bytes == 0)
        {
          NetPlay::EndSc2EngineInputReplay();
          std::fprintf(stderr,
                       "[sc2-engine-replay] corrected input did not alter game-owned state\n");
          m_sc2_engine_replay_completed = true;
          return;
        }
      }
      m_sc2_engine_replay_endpoint_guest = replayed_guest;
      m_sc2_engine_replay_endpoint_tb_remainder = replayed_tb_remainder;
      m_sc2_engine_replay_reference_input_valid = NetPlay::FinishSc2EngineInputReplay(
          &m_sc2_engine_replay_reference_perturbed_polls);
      if (!m_sc2_engine_replay_reference_input_valid ||
          !capture_full_emulator_state(m_sc2_engine_replay_endpoint_state,
                                       m_sc2_engine_replay_endpoint_state_size) ||
          !restore_full_emulator_state(m_sc2_engine_replay_entry_state,
                                       m_sc2_engine_replay_entry_state_size) ||
          !NetPlay::BeginSc2EngineInputReplay(m_sc2_engine_replay_corrected_input))
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

    m_sc2_engine_speculative_active = false;
    if (m_sc2_engine_set_mem_journal)
      m_sc2_engine_set_mem_journal(nullptr, nullptr);
    if (StaticRecompLockstep::g_ram_write_journal_user == this)
    {
      StaticRecompLockstep::g_ram_write_journal = nullptr;
      StaticRecompLockstep::g_ram_write_journal_user = nullptr;
    }
    m_sc2_update_external_replay = false;
    const bool external_replay_valid =
        !m_sc2_engine_replay_selective_update ||
        (m_sc2_update_external_valid &&
         m_sc2_update_external_replay_index == m_sc2_update_external_effects.size());
    std::size_t replayed_state_size = 0;
    std::size_t verification_perturbed_polls = 0;
    const bool verification_input_valid =
        m_sc2_engine_replay_input_update_span ?
            NetPlay::FinishSc2EngineInputReplay(&verification_perturbed_polls) :
        m_sc2_engine_replay_selective_update ? true :
                                               NetPlay::FinishSc2EngineInputReplay(
                                                   &verification_perturbed_polls);
    const bool corrected_input_valid =
        !m_sc2_engine_replay_corrected_input ||
        (m_sc2_engine_replay_reference_perturbed_polls != 0 &&
         verification_perturbed_polls == m_sc2_engine_replay_reference_perturbed_polls);
    if (!verification_input_valid || !corrected_input_valid ||
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
    std::size_t differing_ram_bytes = 0;
    std::map<std::size_t, std::size_t> differing_ram_pages;
    std::vector<std::size_t> first_ram_differences;
    for (std::size_t offset = 0; offset < m_sc2_engine_replay_endpoint_ram.size(); ++offset)
    {
      if (m_sc2_engine_replay_endpoint_ram[offset] == m_guest.ram[offset])
        continue;
      ++differing_ram_bytes;
      ++differing_ram_pages[offset / 4096];
      if (first_ram_differences.size() < 64)
        first_ram_differences.push_back(offset);
    }
    std::size_t differing_l1_bytes = 0;
    for (std::size_t offset = 0; offset < m_sc2_engine_replay_endpoint_l1.size(); ++offset)
      differing_l1_bytes += m_sc2_engine_replay_endpoint_l1[offset] != l1[offset];
    if (m_sc2_engine_replay_selective_update)
    {
      std::fprintf(stderr,
                   "[sc2-engine-replay] selective-result ram_differences=%zu "
                   "l1_differences=%zu tb_entry=%llu tb_endpoint=%llu tb_replay=%llu\n",
                   differing_ram_bytes, differing_l1_bytes,
                   static_cast<unsigned long long>(m_sc2_engine_replay_entry_tb_remainder),
                   static_cast<unsigned long long>(m_sc2_engine_replay_endpoint_tb_remainder),
                   static_cast<unsigned long long>(replayed_tb_remainder));
      for (const auto& [page, differences] : differing_ram_pages)
      {
        std::fprintf(stderr,
                     "[sc2-engine-replay] selective-ram-page offset=0x%08zx "
                     "differences=%zu\n",
                     page * 4096, differences);
      }
      for (const auto& [key, count] : m_sc2_update_original_write_blocks)
      {
        if (!differing_ram_pages.contains(static_cast<u32>(key) / 4096))
          continue;
        std::fprintf(stderr,
                     "[sc2-engine-replay] selective-writer phase=original pc=0x%08x "
                     "page=0x%08x writes=%llu\n",
                     static_cast<u32>(key >> 32), static_cast<u32>(key),
                     static_cast<unsigned long long>(count));
      }
      for (const auto& [key, count] : m_sc2_update_replay_write_blocks)
      {
        if (!differing_ram_pages.contains(static_cast<u32>(key) / 4096))
          continue;
        std::fprintf(stderr,
                     "[sc2-engine-replay] selective-writer phase=replay pc=0x%08x "
                     "page=0x%08x writes=%llu\n",
                     static_cast<u32>(key >> 32), static_cast<u32>(key),
                     static_cast<unsigned long long>(count));
      }
      for (const std::size_t offset : first_ram_differences)
      {
        std::fprintf(stderr,
                     "[sc2-engine-replay] selective-ram-byte offset=0x%08zx "
                     "endpoint=0x%02x replay=0x%02x\n",
                     offset, m_sc2_engine_replay_endpoint_ram[offset], m_guest.ram[offset]);
      }
    }
    std::fprintf(stderr,
                 "[sc2-engine-replay] full-state-result state_match=%s cpu_match=%s "
                 "tb_remainder_match=%s input_replay_match=%s input_polls=%zu "
                 "external_profile_complete=%s external_replay_match=%s external_effects=%zu "
                 "corrected_input=%s perturbed_polls=%zu corrected_state_bytes=%zu "
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
                 external_replay_valid ? "yes" : "no", m_sc2_update_external_effects.size(),
                 corrected_input_valid && m_sc2_engine_replay_corrected_input ? "yes" : "no",
                 verification_perturbed_polls, m_sc2_engine_replay_corrected_state_bytes,
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
    if (m_sc2_engine_replay_corrected_input)
    {
      if (!restore_full_emulator_state(m_sc2_engine_replay_original_endpoint_state,
                                       m_sc2_engine_replay_original_endpoint_state_size))
      {
        std::fprintf(stderr,
                     "[sc2-engine-replay] failed corrected-input canonical recovery\n");
      }
      m_guest = m_sc2_engine_replay_original_endpoint_guest;
      m_tb_cycle_remainder = m_sc2_engine_replay_original_endpoint_tb_remainder;
      std::fprintf(stderr,
                   "[sc2-engine-replay] restored original endpoint after corrected-input gate\n");
    }
    else if (m_sc2_engine_replay_selective_update &&
        (!state_match || !cpu_match || !tb_remainder_match || !external_replay_valid))
    {
      if (!restore_full_emulator_state(m_sc2_engine_replay_endpoint_state,
                                       m_sc2_engine_replay_endpoint_state_size))
      {
        std::fprintf(stderr, "[sc2-engine-replay] failed canonical recovery\n");
      }
      m_guest = m_sc2_engine_replay_endpoint_guest;
      m_tb_cycle_remainder = m_sc2_engine_replay_endpoint_tb_remainder;
      std::fprintf(stderr, "[sc2-engine-replay] restored canonical endpoint after failed gate\n");
    }
    else
    {
      m_guest = replayed_guest;
      m_tb_cycle_remainder = replayed_tb_remainder;
    }
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
