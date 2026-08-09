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
