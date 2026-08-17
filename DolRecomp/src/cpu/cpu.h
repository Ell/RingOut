#ifndef DOLRECOMP_CPU_H
#define DOLRECOMP_CPU_H

#include "common/types.h"

// New-ABI CPUState (spr[1024] + mem2, no external_pointer). Kept in sync with
// the chassis runtime header (GXRuntime core/cpu.h) for the module ABI check.
#define GXRUNTIME_CPU_ABI_VERSION 3u

#define GC_MAIN_RAM_SIZE    (24 * 1024 * 1024)
#define GC_RAM_BASE         0x80000000u
#define GC_RAM_UNCACHED     0xC0000000u

#define WII_MEM2_SIZE       (64 * 1024 * 1024)
#define WII_MEM2_BASE       0x90000000u
#define WII_MEM2_UNCACHED   0xD0000000u

#define PPC_EXC_PROGRAM       0x00000001u
#define PPC_EXC_DSI           0x00000002u
#define PPC_EXC_ALIGNMENT     0x00000004u
#define PPC_EXC_SYSTEM_CALL   0x00000008u
#define PPC_EXC_MACHINE_CHECK 0x00000010u
#define PPC_EXC_FP_UNAVAILABLE 0x00000020u

#define PPC_PROGRAM_FP        0x00100000u
#define PPC_PROGRAM_ILLEGAL   0x00080000u
#define PPC_PROGRAM_PRIV      0x00040000u
#define PPC_PROGRAM_TRAP      0x00020000u

#define PPC_DSI_EAR_DISABLED  0x00100000u

#define PPC_VECTOR_MACHINE_CHECK 0x00200u
#define PPC_VECTOR_DSI           0x00300u
#define PPC_VECTOR_ALIGNMENT     0x00600u
#define PPC_VECTOR_PROGRAM       0x00700u
#define PPC_VECTOR_FP_UNAVAILABLE 0x00800u
#define PPC_VECTOR_SYSTEM_CALL   0x00C00u

#define PPC_HID2_LSQE   0x80000000u
#define PPC_HID2_PSE    0x20000000u
#define PPC_HID2_LCE    0x10000000u
#define PPC_HID2_DCHERR 0x00800000u
#define PPC_HID2_DCHEE  0x00080000u

#define PPC_GEKKO_PVR 0x00083214u

typedef struct CPUState CPUState;
typedef u64 (*PPCExternalRead)(CPUState* cpu, u32 ea, u8 size);
typedef void (*PPCExternalWrite)(CPUState* cpu, u32 ea, u64 value, u8 size);
typedef u32 (*PPCExternalRead32)(CPUState* cpu, u32 ea, u8 rid);
typedef void (*PPCExternalWrite32)(CPUState* cpu, u32 ea, u32 value, u8 rid);
typedef void (*PPCInstructionFallback)(CPUState* cpu, u32 raw, u32 cia);
typedef bool (*PPCHostCall)(CPUState* cpu, u32 address);

struct CPUState {
    u32 gpr[32];
    f64 fpr[32];
    f64 ps1[32];
    u32 pc;
    u32 lr;
    u32 ctr;
    u32 cr;
    u32 xer;
    u32 fpscr;
    u32 msr;
    u32 srr0;
    u32 srr1;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u64 timebase;
    u32 sr[16];
    u32 gqr[8];
    u32 spr[1024];
    u32 exception;
    u32 program_exception;
    u32 tlb_last_vps;
    u32 tlb_last_index;
    u32 tlb_invalidate_count;
    u32 external_addr;
    u32 external_value;
    u8 external_rid;
    u8 external_read_count;
    u8 external_write_count;
    u32 reserve_addr;
    bool reserve_valid;
    u32 locked_cache_tag[512];
    bool locked_cache_valid[512];
    PPCExternalRead external_read;
    PPCExternalWrite external_write;
    PPCExternalRead32 external_read32;
    PPCExternalWrite32 external_write32;
    PPCInstructionFallback instruction_fallback;
    PPCHostCall host_call;
    void* external_user_data;

    u8* ram;
    u32 ram_size;
    u8* mem2;
    u32 mem2_size;
    s64 downcount;
};

bool cpu_init(CPUState* cpu);
bool cpu_alloc_mem2(CPUState* cpu, u32 size); //mem 2 only exists after first aloc
void cpu_free(CPUState* cpu);
void cpu_reset(CPUState* cpu);

// Slow-path memory access (MMIO / external / unmapped). The fast inline
// wrappers at the bottom of this header service the common cached/uncached
// MEM1+MEM2 hit inline, so the generated code never pays a call for RAM.
u64  mem_read64_slow(CPUState* cpu, u32 addr);
void mem_write64_slow(CPUState* cpu, u32 addr, u64 value);
u32  mem_read32_slow(CPUState* cpu, u32 addr);
void mem_write32_slow(CPUState* cpu, u32 addr, u32 value);
u16  mem_read16_slow(CPUState* cpu, u32 addr);
void mem_write16_slow(CPUState* cpu, u32 addr, u16 value);
u8   mem_read8_slow(CPUState* cpu, u32 addr);
void mem_write8_slow(CPUState* cpu, u32 addr, u8 value);

// Memory write journal (lockstep pre-image capture). Declared here so the
// inline write fast path can reference the globals and fold the null check.
typedef void (*PPCMemWriteJournal)(u32 offset, u32 size, void* user);
extern PPCMemWriteJournal g_mem_write_journal;
extern void* g_mem_write_journal_user;
void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user);

f64 ppc_approx_reciprocal(f64 value);
f64 ppc_approx_rsqrt(f64 value);
bool ppc_fres(CPUState* cpu, f64 value, f64* result);
bool ppc_frsqrte(CPUState* cpu, f64 value, f64* result);
void ppc_ps_res(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
void ppc_ps_rsqrte(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
             bool subtract, bool negative, f64* output);
bool ppc_fctiw(CPUState* cpu, f64 value, bool toward_zero, u64* result);
bool ppc_add_overflowed(u32 a, u32 b, u32 result);
bool ppc_trap_condition(u8 to, u32 a, u32 b);
void ppc_set_xer_ov(CPUState* cpu, bool ov);
void ppc_take_exception(CPUState* cpu, u32 exception, u32 vector, u32 srr0, u32 srr1_info);
void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia);
void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia);
bool ppc_host_call(CPUState* cpu, u32 address);
void ppc_system_call_exception(CPUState* cpu, u32 cia);
void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr);
void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia);
u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia);
u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia);
void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia);
void ppc_rfi(CPUState* cpu, u32 cia);
void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia);
// Returns false when the access raised an exception, true otherwise. The C
// backend calls these as statements and ignores it; the LLVM backend BRANCHES on
// the result, and while these returned void it branched on a garbage register,
// took the failure path and returned without advancing pc -- an infinite
// re-dispatch of the same address.
bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia);
void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia);
void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia);
void ppc_fpscr_updated(CPUState* cpu);
void ppc_memory_fence(void);

// ---------------------------------------------------------------------------
// Hot inline fast paths. These were previously out-of-line functions in cpu.c,
// so every generated load/store/FP-guard emitted a real call (88k mem_read32,
// 87k ppc_fp_available, 70k mem_write32 sites) — the dominant CPU cost during
// heavy workloads (FMV decode). Inlining the common cases here collapses them
// to a couple of compares + a byteswap; only genuine misses call the slow path.

// Plain static inline. Measured: force-inlining the mem wrappers (always_inline)
// only bought ~2% (50%→52% FMV) while ballooning the module 20MB→36MB, so it is
// not worth it — the big win was inlining ppc_fp_available (the FP guard), which
// this FP-heavy path hit on every op. clang keeps the branchier mem wrappers as
// direct local calls (still far cheaper than the former PLT round-trip).
#define DOLRECOMP_AI static inline

// Cached-MEM1 host pointer for [addr, addr+size) or NULL. This is the
// overwhelmingly common access (0x80xxxxxx); uncached (0xC0…), MEM2 and MMIO
// fall through to the *_slow path, which resolve_addr() handles identically.
// Kept to a single compare so the wrappers below inline naturally at -O2 (no
// always_inline needed → no compile-time / code-size blowup at 80k+ sites).
// Unsigned wrap makes any out-of-region address exceed the bound and fail.
DOLRECOMP_AI u8* dolrecomp_cached_ram(const CPUState* cpu, u32 addr, u32 size) {
    u32 off = addr - GC_RAM_BASE;
    return off <= cpu->ram_size - size ? cpu->ram + off : (u8*)0;
}

// host is always inside cpu->ram here (cached MEM1), so the journal offset is
// direct and needs no range check.
DOLRECOMP_AI void dolrecomp_journal_cached(CPUState* cpu, const u8* host, u32 size) {
    if (g_mem_write_journal)
        g_mem_write_journal((u32)(host - cpu->ram), size, g_mem_write_journal_user);
}
DOLRECOMP_AI void dolrecomp_clear_reservation(CPUState* cpu, u32 addr) {
    if (cpu->reserve_valid && ((cpu->reserve_addr ^ addr) & ~31u) == 0)
        cpu->reserve_valid = false;
}

DOLRECOMP_AI u8 mem_read8(CPUState* cpu, u32 addr) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 1u);
    return h ? *h : mem_read8_slow(cpu, addr);
}
DOLRECOMP_AI u16 mem_read16(CPUState* cpu, u32 addr) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 2u);
    return h ? read_be16(h) : mem_read16_slow(cpu, addr);
}
DOLRECOMP_AI u32 mem_read32(CPUState* cpu, u32 addr) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 4u);
    return h ? read_be32(h) : mem_read32_slow(cpu, addr);
}
DOLRECOMP_AI u64 mem_read64(CPUState* cpu, u32 addr) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 8u);
    return h ? read_be64(h) : mem_read64_slow(cpu, addr);
}

DOLRECOMP_AI void mem_write8(CPUState* cpu, u32 addr, u8 value) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 1u);
    if (h) { dolrecomp_clear_reservation(cpu, addr); dolrecomp_journal_cached(cpu, h, 1u); *h = value; }
    else mem_write8_slow(cpu, addr, value);
}
DOLRECOMP_AI void mem_write16(CPUState* cpu, u32 addr, u16 value) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 2u);
    if (h) { dolrecomp_clear_reservation(cpu, addr); dolrecomp_journal_cached(cpu, h, 2u); write_be16(h, value); }
    else mem_write16_slow(cpu, addr, value);
}
DOLRECOMP_AI void mem_write32(CPUState* cpu, u32 addr, u32 value) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 4u);
    if (h) { dolrecomp_clear_reservation(cpu, addr); dolrecomp_journal_cached(cpu, h, 4u); write_be32(h, value); }
    else mem_write32_slow(cpu, addr, value);
}
DOLRECOMP_AI void mem_write64(CPUState* cpu, u32 addr, u64 value) {
    u8* h = dolrecomp_cached_ram(cpu, addr, 8u);
    if (h) { dolrecomp_clear_reservation(cpu, addr); dolrecomp_journal_cached(cpu, h, 8u); write_be64(h, value); }
    else mem_write64_slow(cpu, addr, value);
}

/* LAZY FPRF.
 *
 * FPRF (FPSCR bits 12-16) classifies every FP result. Measured on gameplay it
 * is written 3.073 G times and read 15,885 -- 100% dead -- and costs ~20 host
 * instructions a time, 3.1% of all cycles. So do not compute it eagerly:
 * remember the value, and classify only when someone can actually see FPSCR.
 *
 * The pending value lives in module globals rather than CPUState on purpose.
 * A CPUState field would be the tidier home, but the chassis exports guest
 * state to Dolphin through SyncOut() -- the path savestates, rollback and
 * netplay all run on -- and hooking that needs a new module export and an ABI
 * bump, which couples module and runtime deployment. Instead this flushes at
 * every point control LEAVES the module, so the chassis can never observe a
 * stale FPSCR and none of that machinery has to change:
 *
 *   chassis_dispatch        (module_export.c) every return to the run loop
 *   ppc_fallback_instruction  before the interpreter sees the state
 *   ppc_host_call             before any HLE hook runs
 *   mffs / mcrfs              the guest reading it itself
 *
 * and it is DROPPED (not flushed) wherever the guest writes FPSCR explicitly
 * (mtfsf/mtfsfi/mtfsb0/mtfsb1), because that write supersedes the pending
 * classification of an earlier op.
 *
 * Correctness is exactly checkable: guest state must stay bit-identical, so
 * the gameplay benchmark's frame hash must be unchanged.
 */
/* RECOMP_FPRF_TRACE: name the instruction that last wrote FPRF before each
 * mffs, so an eager and a lazy build can be diffed to find which op the two
 * disagree about. Off by default and expanding to nothing. */
#ifdef RECOMP_FPRF_TRACE
extern u32 g_fprf_pc;
void ppc_fprf_trace_mffs(CPUState* cpu);
#define PPC_FPRF_TAG(addr)      (g_fprf_pc = (u32)(addr))
#define PPC_FPRF_TRACE_MFFS(c)  ppc_fprf_trace_mffs(c)
#else
#define PPC_FPRF_TAG(addr)      ((void)0)
#define PPC_FPRF_TRACE_MFFS(c)  ((void)0)
#endif

extern f64 g_fprf_value;
extern u8  g_fprf_kind;   /* 0 none, 1 single, 2 double */
void ppc_fprf_materialize(CPUState* cpu);

static inline void ppc_fprf_flush(CPUState* cpu) {
    if (g_fprf_kind) ppc_fprf_materialize(cpu);
}
static inline void ppc_fprf_drop(void) { g_fprf_kind = 0u; }

/* Condition-register liveness instrumentation.
 *
 * Every `.`-form instruction and every compare eagerly computes a CR field --
 * about six host instructions each, into a read-modify-write of ctx->cr. If
 * most of those fields are overwritten before anything reads them, that is
 * pure emitted waste and eliding it is a codegen win. Nothing has ever
 * measured which it is, and in this codebase static structure has predicted
 * execution badly every single time (static psq sites said 92.8% GQR0; the
 * measured truth for stores was 15.3%), so this counts at runtime.
 *
 * OFF BY DEFAULT AND EXPANDING TO NOTHING, so one generation serves both the
 * shipped module and the probe: build with -DRECOMP_CR_STATS to arm it.
 */
#ifdef RECOMP_CR_STATS
void ppc_cr_note_write(unsigned field);
void ppc_cr_note_read(unsigned field_mask);
void ppc_ca_note_write(void);
void ppc_ca_note_read(void);
void ppc_fprf_note_write(void);
void ppc_fprf_note_read(void);
void ppc_ps_note_lane(void);
#define PPC_CR_WRITE(field) ppc_cr_note_write((field))
#define PPC_CR_READ(mask)   ppc_cr_note_read((mask))
#define PPC_CA_WRITE()      ppc_ca_note_write()
#define PPC_CA_READ()       ppc_ca_note_read()
#define PPC_FPRF_WRITE()    ppc_fprf_note_write()
#define PPC_FPRF_READ()     ppc_fprf_note_read()
#define PPC_PS_LANE()       ppc_ps_note_lane()
#else
#define PPC_CR_WRITE(field) ((void)0)
#define PPC_CR_READ(mask)   ((void)0)
#define PPC_CA_WRITE()      ((void)0)
#define PPC_CA_READ()       ((void)0)
#define PPC_FPRF_WRITE()    ((void)0)
#define PPC_FPRF_READ()     ((void)0)
#define PPC_PS_LANE()       ((void)0)
#endif

// MSR.FP (PPC_BIT(18) = 0x2000). Common case (FP enabled) is a single bit test.
static inline bool ppc_fp_available(CPUState* cpu, u32 cia) {
    if (cpu->msr & 0x00002000u)
        return true;
    ppc_take_exception(cpu, PPC_EXC_FP_UNAVAILABLE, PPC_VECTOR_FP_UNAVAILABLE, cia, 0);
    return false;
}

#endif /* DOLRECOMP_CPU_H */
