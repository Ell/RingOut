// Out-of-line float helpers for the LLVM object backend.
//
// The C backend emits this arithmetic INLINE, so this fork never needed these as
// functions and does not define them. The LLVM backend emits CALLS by name, so
// without this translation unit the module does not link: ppc_fp_available (420
// call sites), ppc_fsubs (356), ppc_fmuls (310), ppc_fcmp (284), ppc_fadds (271)
// and the rest.
//
// EVERY BODY HERE MIRRORS WHAT emitter.c EMITS, NOT WHAT UPSTREAM'S cpu.c DOES.
// Upstream has all of these already, and taking them would have been quicker and
// wrong -- they differ from this fork in ways the game can observe:
//
//   * single-precision ops here broadcast the result to BOTH paired-single lanes
//     (Gekko: ps[FD].Fill), which upstream's binary_single does not. That fix is
//     why geometry stopped warping.
//   * ps_mul rounds each operand to f32 and multiplies; upstream multiplies in
//     f64 with force_25_bit on c. Different results.
//   * FPCC on compares is cleared then set per the architecture. Upstream, and
//     Dolphin's interpreter, OR into the old value -- see the note in
//     emitter.c's emit_fcompare. The game reads FPSCR at two mffs sites.
//
// So a module built through either backend must produce identical guest state;
// the determinism harness is what proves it, by comparing frame hashes against a
// C-backend module.

// ppc_fp_available is a static inline fast path in cpu.h, which produces no
// linkable symbol. Rename the inline out of the way, then export a real function
// that calls it, so both backends run the same code rather than two copies of it.
// generated.h, not cpu.h: dolrecomp_fprf_s / dolrecomp_fprf_d /
// dolrecomp_ps_round are static inlines the EMITTER writes per generation, so
// they exist only there. generated.h includes cpu.h itself. This file is
// therefore part of the MODULE build, where GENERATED_DIR is on the include
// path -- it is not built into the recompiler.
#define ppc_fp_available dolrecomp_fp_available_inline
#include "generated.h"
#undef ppc_fp_available

bool ppc_fp_available(CPUState* cpu, u32 cia) {
    return dolrecomp_fp_available_inline(cpu, cia);
}

// --- single precision: result is rounded to f32 and filled into both lanes ---
#define DOLRECOMP_BINARY_SINGLE(name, expr)                                     \
    void name(CPUState* cpu, u8 d, u8 a, u8 b) {                                \
        cpu->fpr[d] = cpu->ps1[d] = (f64)(f32)(expr);                           \
        dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);                                \
    }

DOLRECOMP_BINARY_SINGLE(ppc_fadds, cpu->fpr[a] + cpu->fpr[b])
DOLRECOMP_BINARY_SINGLE(ppc_fsubs, cpu->fpr[a] - cpu->fpr[b])
// fmuls takes rC where the others take rB; the backend passes c in this slot.
DOLRECOMP_BINARY_SINGLE(ppc_fmuls, cpu->fpr[a] * cpu->fpr[b])
DOLRECOMP_BINARY_SINGLE(ppc_fdivs, cpu->fpr[a] / cpu->fpr[b])

// --- double precision: no lane fill, and FPRF is classified on the f64 ---
#define DOLRECOMP_BINARY_DOUBLE(name, expr)                                     \
    void name(CPUState* cpu, u8 d, u8 a, u8 b) {                                \
        cpu->fpr[d] = (expr);                                                   \
        dolrecomp_fprf_d(cpu, cpu->fpr[d]);                                     \
    }

DOLRECOMP_BINARY_DOUBLE(ppc_fadd, cpu->fpr[a] + cpu->fpr[b])
DOLRECOMP_BINARY_DOUBLE(ppc_fsub, cpu->fpr[a] - cpu->fpr[b])
DOLRECOMP_BINARY_DOUBLE(ppc_fmul, cpu->fpr[a] * cpu->fpr[b])
DOLRECOMP_BINARY_DOUBLE(ppc_fdiv, cpu->fpr[a] / cpu->fpr[b])

// frsp fills both lanes as well (Dolphin: ps[FD].Fill(rounded)).
void ppc_frsp(CPUState* cpu, u8 d, u8 b) {
    cpu->fpr[d] = cpu->ps1[d] = (f64)(f32)cpu->fpr[b];
    dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);
}

void ppc_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c) {
    cpu->fpr[d] = dolrecomp_ps_round((f32)cpu->fpr[a] * (f32)cpu->fpr[c]);
    cpu->ps1[d] = dolrecomp_ps_round((f32)cpu->ps1[a] * (f32)cpu->ps1[c]);
    dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);
}

// Sets the CR field and FPCC. `ordered` distinguishes fcmpo from fcmpu; this
// fork's emitter treats them identically, so it is accepted and unused rather
// than silently changing behaviour between backends.
void ppc_fcmp(CPUState* cpu, u8 crfd, f64 val_a, f64 val_b, bool ordered) {
    (void)ordered;
    const u32 shift = (7u - (crfd & 7u)) * 4u;
    u32 cr_bits;
    if (val_a < val_b)       cr_bits = 0x8u;
    else if (val_a > val_b)  cr_bits = 0x4u;
    else if (val_a == val_b) cr_bits = 0x2u;
    else                     cr_bits = 0x1u;
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (cr_bits << shift);
    cpu->fpscr = (cpu->fpscr & ~(0xFu << 12)) | (cr_bits << 12);
}

// --- paired single -------------------------------------------------------
// Every lane is computed as (f32)x OP (f32)y and then run through
// dolrecomp_ps_round, matching emitter.c. Upstream instead multiplies in f64
// with force_25_bit; that is a different number, so none of these bodies were
// taken from it. FPRF is classified on lane 0 only, as the emitter does.
#define DOLRECOMP_PS_BINARY(name, OP)                                          \
    void name(CPUState* cpu, u8 d, u8 a, u8 b) {                               \
        f64 lo = dolrecomp_ps_round((f32)cpu->fpr[a] OP (f32)cpu->fpr[b]);     \
        f64 hi = dolrecomp_ps_round((f32)cpu->ps1[a] OP (f32)cpu->ps1[b]);     \
        cpu->fpr[d] = lo;                                                      \
        cpu->ps1[d] = hi;                                                      \
        dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);                               \
    }

DOLRECOMP_PS_BINARY(ppc_ps_add_op, +)
DOLRECOMP_PS_BINARY(ppc_ps_sub_op, -)
DOLRECOMP_PS_BINARY(ppc_ps_div_op, /)

// ps_madd/msub/nmadd/nmsub share one body; the backend passes the two flags
// rather than emitting four names. Note the product is formed in f32 BEFORE the
// addend, exactly as the emitter does -- rounding once at the end would give a
// different result.
void ppc_ps_madd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract,
                    bool negative) {
    f32 lo = (f32)cpu->fpr[a] * (f32)cpu->fpr[c];
    f32 hi = (f32)cpu->ps1[a] * (f32)cpu->ps1[c];
    if (subtract) {
        lo -= (f32)cpu->fpr[b];
        hi -= (f32)cpu->ps1[b];
    } else {
        lo += (f32)cpu->fpr[b];
        hi += (f32)cpu->ps1[b];
    }
    if (negative) {
        lo = -lo;
        hi = -hi;
    }
    cpu->fpr[d] = dolrecomp_ps_round(lo);
    cpu->ps1[d] = dolrecomp_ps_round(hi);
    dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);
}

// The s0/s1 forms broadcast ONE lane of c to both multiplies: s0 takes c's lane
// 0 (fpr[c]) and s1 takes lane 1 (ps1[c]).
void ppc_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 lo = dolrecomp_ps_round((f32)cpu->fpr[a] * (f32)cpu->fpr[c] + (f32)cpu->fpr[b]);
    f64 hi = dolrecomp_ps_round((f32)cpu->ps1[a] * (f32)cpu->fpr[c] + (f32)cpu->ps1[b]);
    cpu->fpr[d] = lo;
    cpu->ps1[d] = hi;
    dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);
}

void ppc_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 lo = dolrecomp_ps_round((f32)cpu->fpr[a] * (f32)cpu->ps1[c] + (f32)cpu->fpr[b]);
    f64 hi = dolrecomp_ps_round((f32)cpu->ps1[a] * (f32)cpu->ps1[c] + (f32)cpu->ps1[b]);
    cpu->fpr[d] = lo;
    cpu->ps1[d] = hi;
    dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);
}

void ppc_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 lo = dolrecomp_ps_round((f32)cpu->fpr[a] * (f32)cpu->fpr[c]);
    f64 hi = dolrecomp_ps_round((f32)cpu->ps1[a] * (f32)cpu->fpr[c]);
    cpu->fpr[d] = lo;
    cpu->ps1[d] = hi;
    dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);
}

void ppc_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 lo = dolrecomp_ps_round((f32)cpu->fpr[a] * (f32)cpu->ps1[c]);
    f64 hi = dolrecomp_ps_round((f32)cpu->ps1[a] * (f32)cpu->ps1[c]);
    cpu->fpr[d] = lo;
    cpu->ps1[d] = hi;
    dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);
}

// sum0 crosses the lanes: lane 0 is a's lane 0 + b's lane 1, and lane 1 is
// simply c's lane 1 passed through. sum1 is the mirror image.
void ppc_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 lo = dolrecomp_ps_round((f32)cpu->fpr[a] + (f32)cpu->ps1[b]);
    f64 hi = dolrecomp_ps_round(cpu->ps1[c]);
    cpu->fpr[d] = lo;
    cpu->ps1[d] = hi;
    dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);
}

void ppc_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 lo = dolrecomp_ps_round(cpu->fpr[c]);
    f64 hi = dolrecomp_ps_round((f32)cpu->fpr[a] + (f32)cpu->ps1[b]);
    cpu->fpr[d] = lo;
    cpu->ps1[d] = hi;
    dolrecomp_fprf_s(cpu, (f32)cpu->fpr[d]);
}

void ppc_ps_rsqrte_op(CPUState* cpu, u8 d, u8 b) {
    f64 lo, hi;
    ppc_ps_rsqrte(cpu, cpu->fpr[b], cpu->ps1[b], &lo, &hi);
    cpu->fpr[d] = dolrecomp_ps_round(lo);
    cpu->ps1[d] = dolrecomp_ps_round(hi);
}

// --- control -------------------------------------------------------------
// mtfsb0/mtfsb1 refuse to touch FPSCR bits 1 and 2, which are read-only
// summaries, exactly as the emitter does.
void ppc_mtfsb1_op(CPUState* cpu, u8 bit) {
    if (bit != 1u && bit != 2u)
        cpu->fpscr |= 0x80000000u >> bit;
}

void ppc_mtfsb0_op(CPUState* cpu, u8 bit) {
    if (bit != 1u && bit != 2u)
        cpu->fpscr &= ~(0x80000000u >> bit);
}

// Re-arms host rounding/flush state after a control write. cpu.c already
// exposes this; the LLVM backend just calls it under a different name.
void ppc_fpscr_control_updated(CPUState* cpu) {
    ppc_fpscr_updated(cpu);
}

// The cache-op selector is upstream's enum, which this fork's cpu.h does not
// have. The backend emits these as raw immediates, so the ORDER is the contract
// -- it must match upstream's PPCCacheControl enum, not be renumbered here.
enum {
    DOLRECOMP_CACHE_DCBST = 0,
    DOLRECOMP_CACHE_DCBF  = 1,
    DOLRECOMP_CACHE_DCBI  = 2,
    DOLRECOMP_CACHE_ICBI  = 3,
};

// dcbst/dcbf/dcbi are no-ops in a flat memory model -- stores go straight to
// RAM and there is no data cache to flush. icbi is NOT: the emitter routes it to
// the interpreter so the chassis can retire the affected chunk, and that has to
// hold here too or self-modifying code would silently keep running stale
// translations.
void ppc_cache_control(CPUState* cpu, u8 operation, u32 ea, u32 cia) {
    (void)ea;
    if (operation == DOLRECOMP_CACHE_ICBI)
        ppc_fallback_instruction(cpu, 0x7C0007ACu, cia);
}
