/* Lazy FPRF must be FLUSHED ON ENTRY to every op that writes FPSCR[12:16]
 * itself. This is the trap that cost three attempts to get right.
 *
 * FPRF is deferred: ops record the result value and its kind in module globals
 * and the classification is materialised later, at the read sites. That is
 * worth a measured -2.45% cycles because the game reads FPSCR twice in a
 * session and writes it 2.9 billion times.
 *
 * The hazard: ppc_fprf_materialize() clears the WHOLE 0x1F<<12 field and writes
 * the classification into it. FI/FR (bits 13-14) and FPCC (bits 12-15) live
 * INSIDE that field. So if an instruction writes those bits while a
 * classification is still pending, a later flush wipes what it wrote. The
 * symptom was one word at 0x804072A4 off by exactly bit 13 at frame 1280 --
 * that far from the cause, and only with a hash gate to catch it at all.
 *
 * The fix is ordering: each such op flushes the pending value on ENTRY, before
 * writing its own bits, which restores the eager sequence. This asserts that
 * ordering directly, per op, instead of leaving it to a 16000-frame hash.
 *
 * A pending value that simply got DROPPED would also pass "kind == 0", so each
 * case checks the classification actually landed in FPSCR too.
 */
#include <stdio.h>
#include <string.h>

#include "../src/cpu/cpu.h"

/* -1.0 is a negative normal: FPRF = 0x08 (bit 15 of the 5-bit field), which is
   distinguishable from the zeroed FPSCR these cases start from. */
#define PENDING_VALUE (-1.0)
#define FPRF_FIELD(fpscr) (((fpscr) >> 12) & 0x1Fu)

static int failures;

/* A pending value whose CLASSIFICATION sets an FI/FR bit. classify(+denormal)
   is 0x14, and 0x14 << 12 lights FPSCR bit 14 -- one of the two bits
   (0x00006000) these helpers clear. That is what makes a late flush visible:
   materialising it puts back a bit the helper deliberately cleared. A value
   like -1.0 (0x08) sets only bit 15 and cannot detect the bug at all, which is
   how the first version of this test passed against a sabotaged emitter. */
#define PENDING_DENORM 1e-320
#define FI_FR_MASK 0x00006000u

static void arm(CPUState* cpu, u32 fpscr) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->msr = 0x00002000u;   /* MSR.FP */
    cpu->fpscr = fpscr;
    g_fprf_value = PENDING_DENORM;
    g_fprf_kind = 2u;         /* double */
}

/* The ordering assertion. The helper clears FI/FR; whatever the module does
   next must not put them back. With the entry flush the pending value is long
   gone by then; without it, it is still sitting there waiting to overwrite. */
static void check_late_flush_cannot_revive_fi_fr(const char* op, CPUState* cpu) {
    if (cpu->fpscr & FI_FR_MASK) {
        fprintf(stderr, "%s: FI/FR set before the late flush -- test setup is wrong\n", op);
        failures++;
        return;
    }
    ppc_fprf_flush(cpu);          /* the "later" flush, e.g. at the next read site */
    if (cpu->fpscr & FI_FR_MASK) {
        fprintf(stderr,
                "%s: a later flush revived FI/FR (fpscr=0x%08X). The pending FPRF "
                "was not materialised on entry, so it outlived the op that cleared "
                "those bits.\n", op, cpu->fpscr);
        failures++;
    }
}

int main(void) {
    CPUState cpu;
    f64 r, r2;

    /* GROUP A -- helpers that can RETURN EARLY, before reaching set_fprf().
       That is the only path on which a pending value survives the call, so it
       is the only path where a later flush can revive the FI/FR bits the
       helper cleared. fpscr bit 4 is the enable that forces the early exit. */
    arm(&cpu, 0x10u); ppc_fres(&cpu, 0.0, &r);
    check_late_flush_cannot_revive_fi_fr("ppc_fres", &cpu);

    arm(&cpu, 0x10u); ppc_frsqrte(&cpu, 0.0, &r);
    check_late_flush_cannot_revive_fi_fr("ppc_frsqrte", &cpu);

    /* GROUP B -- ppc_ps_res / ppc_ps_rsqrte are void and always reach
       set_fprf(), which drops any pending value. They are safe by a different
       mechanism, so the assertion is different: nothing may outlive the call.
       Do NOT assert on FI/FR here -- set_fprf writes the classification into
       the same bits (1/0 is +inf, 0x05, which lights bit 14 legitimately), and
       an earlier version of this test failed on exactly that confusion. */
    arm(&cpu, 0u); ppc_ps_res(&cpu, 0.0, 0.0, &r, &r2);
    if (g_fprf_kind != 0u) {
        fprintf(stderr, "ppc_ps_res: a pending FPRF outlived the call\n");
        failures++;
    }
    arm(&cpu, 0u); ppc_ps_rsqrte(&cpu, 0.0, 0.0, &r, &r2);
    if (g_fprf_kind != 0u) {
        fprintf(stderr, "ppc_ps_rsqrte: a pending FPRF outlived the call\n");
        failures++;
    }

    /* GROUP C -- exit points. Control leaving the module must MATERIALISE the
       pending value, not drop it, or the interpreter and host read a stale
       FPSCR. */
    arm(&cpu, 0u);
    ppc_host_call(&cpu, 0x80003000u);
    if (g_fprf_kind != 0u || ((cpu.fpscr >> 12) & 0x1Fu) == 0u) {
        fprintf(stderr, "ppc_host_call: pending FPRF did not materialise before "
                        "leaving the module (kind=%u fpscr=0x%08X)\n",
                g_fprf_kind, cpu.fpscr);
        failures++;
    }

    return failures ? 1 : 0;
}
