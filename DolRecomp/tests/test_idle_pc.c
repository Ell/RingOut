/* --idle-pc is mandatory for this game and fails SILENTLY when it breaks.
 *
 * Loop back-edges compile to native gotos, which is worth a measured -11.5% CPU
 * (52.8% of all dispatches were back-edges). But the OS idle spin loop at
 * 0x80185DEC must keep returning to the chassis, or the host never sees the
 * guest go idle and idle-skip stops working -- the symptom is FMV and gameplay
 * running at roughly half speed, with no error anywhere.
 *
 * Nothing checked this. It was verified by hand, by grepping a generated chunk
 * for `label_80185DEC` and eyeballing its back-edge, which is exactly the kind
 * of check that stops happening. So: assert the emitter's two branches
 * directly.
 *
 * ORDER IS LOAD-BEARING. emit_set_idle_pc() and emit_add_dispatch_pc() write to
 * static emitter state and there is no way to clear it, so the unprotected case
 * must run FIRST.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/emitter.h"
#include "../src/common/types.h"
#include "../src/frontend/decoder.h"

#define BASE 0x80003000u

/* nop, then an unconditional branch back to BASE -- a two-instruction spin
   loop, the same shape as the scheduler's idle loop. */
static const u32 loop_raws[] = {
    0x60000000u,  /* ori r0, r0, 0   (nop) */
    0x4BFFFFFCu,  /* b   -4          (back-edge to BASE) */
};

static char* emit_loop_to_string(void) {
    PPCInst insts[2];
    for (unsigned i = 0; i < 2u; i++) {
        insts[i] = ppc_decode(loop_raws[i], BASE + i * 4u);
        if (insts[i].op == PPC_OP_UNKNOWN) {
            fprintf(stderr, "raw 0x%08X decoded as unknown\n", loop_raws[i]);
            return NULL;
        }
    }

    FILE* f = tmpfile();
    if (!f) { perror("tmpfile"); return NULL; }
    emit_function(f, insts, 2u, BASE);

    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)calloc((size_t)n + 1u, 1u);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1u, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    return buf;
}

/* Assert on the BACK-EDGE ONLY. Every chunk opens with an entry switch --
   `case 0x80003000u: goto label_80003000;` -- which is how the dispatcher jumps
   in, and which contains the very string the naive check looks for. Searching
   the whole function matches the prologue and reports a failure that is not
   there; scope to the text after the branch's own label. */
static const char* back_edge(const char* text) {
    const char* p = strstr(text, "label_80003004:");
    return p ? p : text;
}

int main(void) {
    const char* const goto_form = "goto label_80003000";
    const char* const dispatch_form = "ctx->pc = 0x80003000u;";

    /* 1. Unprotected: the back-edge stays native, behind the loop budget. */
    char* plain = emit_loop_to_string();
    if (!plain) return 1;
    if (!strstr(back_edge(plain), goto_form)) {
        fprintf(stderr, "back-edge is not a native goto without --idle-pc:\n%s\n", plain);
        free(plain);
        return 2;
    }
    if (!strstr(back_edge(plain), "ctx->downcount <= -")) {
        fprintf(stderr, "native loop has no budget check -- it could spin unbounded:\n%s\n", plain);
        free(plain);
        return 3;
    }
    free(plain);

    /* 2. Protected: the same back-edge must become a dispatcher return, and the
          goto must be GONE. Asserting the absence is the point -- emitting both
          would leave the goto reachable and idle-skip just as dead. */
    emit_set_idle_pc(BASE);
    char* idle = emit_loop_to_string();
    if (!idle) return 4;
    if (!strstr(back_edge(idle), dispatch_form)) {
        fprintf(stderr, "back-edge does not return to the dispatcher with --idle-pc:\n%s\n", idle);
        free(idle);
        return 5;
    }
    if (strstr(back_edge(idle), goto_form)) {
        fprintf(stderr, "back-edge still has a native goto with --idle-pc set:\n%s\n", idle);
        free(idle);
        return 6;
    }
    free(idle);

    return 0;
}
