#ifndef DOLRECOMP_EMITTER_H
#define DOLRECOMP_EMITTER_H

#include "../common/types.h"
#include "../frontend/decoder.h"
#include <stdio.h>

typedef enum {
    DOLRECOMP_CPU_GEKKO,
    DOLRECOMP_CPU_BROADWAY,
    DOLRECOMP_CPU_ESPRESSO,
} DolRecompCPU;

// Split C emitter used by the command-line recompiler.

// emit the boilerplate header (includes, typedefs, etc)
void emit_header(FILE* out);
void emit_header_for_cpu(FILE* out, DolRecompCPU cpu);

// Register chunk entries for the opt-in cross-chunk direct-call experiment.
// Direct calls bypass chassis dispatch checks and must not be enabled by a
// runtime that validates mutable guest code there. Call before worker emission;
// passing count == 0 restores the safe return-to-chassis form.
void emit_set_chunk_table(const u32* starts, u32 count);

// emit a single recompiled function as C code
void emit_function(FILE* out, const PPCInst* insts, u32 count, u32 func_addr);

/* Guest PC of an OS idle spin loop, if the host skips it (see --idle-pc).
 * Back-edges to it are emitted as dispatcher returns so the host still sees it. */
void emit_set_idle_pc(u32 pc);
// Select the LLVM object backend instead of emitting C. A setter rather than a
// parameter because that is how this fork already threads options to the
// emitter, and threading a new argument through emit_dol/rpx/rel_split and
// every caller would touch far more than the feature needs.
void emit_set_llvm_backend(int enabled);
int emit_llvm_backend_enabled(void);

/* Guest PC that must always be reached through the dispatcher (a run-loop hook
   target). Repeatable; see emitter.c. */
void emit_add_dispatch_pc(u32 pc);

/* Turn local `bl` into a native goto (--chain-calls). Unproven: see emitter.c. */
void emit_set_chain_calls(bool enable);

/* Chunk-entry switch at leaders only (--leader-cases). Incomplete: see emitter.c. */
void emit_set_leader_cases(bool enable);

/* XER[CA] dead-write analysis (--ca-liveness). REPORTING ONLY: it counts how
   many write sites a SOUND intraprocedural analysis can prove dead, which is
   the number that decides whether eliding them is worth the risk. Emitted code
   is identical either way. */
void emit_set_ca_liveness(bool enable);

/* Elide XER[CA] writes the liveness pass proves dead. SEPARATE from
   --ca-liveness, which only reports and must leave codegen alone. Off by
   default: 98.4% of CA writes are dead, but CA READS are load-bearing (a
   RECOMP_NO_CA build produces zero frames), so a wrong verdict here is a silent
   divergence rather than a crash. Gate any build on the frame hashes. */
void emit_set_ca_elide(bool enable);
void emit_report_ca_stats(void);

/* Writes the entry-point sidecar. No-op (and writes nothing) unless the entry
   set was reduced; its absence means "the whole chunk range is entrable". */
int emit_write_entry_points(const char* path);

/* Records one chunk's entry points. MAIN THREAD ONLY -- emit_function() runs on
   the -jN workers, where appending to the shared list is a data race. */
void emit_collect_entry_points(const PPCInst* insts, u32 count, u32 func_addr);

// emit a single instruction as C code
void emit_instruction(FILE* out, const PPCInst* inst);

// emit the boilerplate footer
void emit_footer(FILE* out);

#endif /* DOLRECOMP_EMITTER_H */
