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

// emit a single recompiled function as C code
void emit_function(FILE* out, const PPCInst* insts, u32 count, u32 func_addr);

/* Guest PC of an OS idle spin loop, if the host skips it (see --idle-pc).
 * Back-edges to it are emitted as dispatcher returns so the host still sees it. */
void emit_set_idle_pc(u32 pc);

/* Guest PC that must always be reached through the dispatcher (a run-loop hook
   target). Repeatable; see emitter.c. */
void emit_add_dispatch_pc(u32 pc);

/* Turn local `bl` into a native goto (--chain-calls). Unproven: see emitter.c. */
void emit_set_chain_calls(bool enable);

// emit a single instruction as C code
void emit_instruction(FILE* out, const PPCInst* inst);

// emit the boilerplate footer
void emit_footer(FILE* out);

#endif /* DOLRECOMP_EMITTER_H */
