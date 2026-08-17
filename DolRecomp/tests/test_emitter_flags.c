/* The off-by-default emitter experiments. None of them ships enabled, so a
 * break here is inert rather than shipped -- but they are also the flags most
 * likely to be switched on by someone measuring, and two of them carry
 * correctness invariants that are not obvious from the flag name.
 *
 * --chain-calls turns a local `bl` into a native goto. The invariant that makes
 * it safe is the negative one: a call whose TARGET the run loop recognises by
 * PC must never be chained, because a call that does not come back through the
 * dispatcher is a hook that never fires. There are seven such FMV-HLE
 * addresses, and --idle-pc registers an eighth. Chaining one of those would not
 * crash -- the movie would simply stop being intercepted.
 *
 * --ca-liveness claims in its own --help text that codegen is UNCHANGED; it
 * only counts provably-dead XER[CA] writes. That claim is worth pinning,
 * because the day someone wires the liveness result into emission is the day it
 * silently stops being true.
 *
 * ORDER IS LOAD-BEARING: emit_add_dispatch_pc() accumulates into static state
 * with no way to remove an entry, so every case that needs an UNprotected
 * target must run before the one that registers it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/emitter.h"
#include "../src/common/types.h"
#include "../src/frontend/decoder.h"

#define BASE   0x80003000u
#define TARGET 0x80003008u

/* bl +8 ; nop ; nop (the call target) ; blr */
static const u32 raws[] = {
    0x48000009u,  /* bl  0x80003008  -- forward, local, linked */
    0x60000000u,  /* nop                                        */
    0x60000000u,  /* nop   <- TARGET                            */
    0x4E800020u,  /* blr                                        */
};
#define NRAW ((u32)(sizeof(raws) / sizeof(raws[0])))

static int failures;

static char* emit_to_string(void) {
    PPCInst insts[NRAW];
    for (u32 i = 0; i < NRAW; i++) {
        insts[i] = ppc_decode(raws[i], BASE + i * 4u);
        if (insts[i].op == PPC_OP_UNKNOWN) {
            fprintf(stderr, "raw 0x%08X decoded as unknown\n", raws[i]);
            return NULL;
        }
    }
    FILE* f = tmpfile();
    if (!f) return NULL;
    emit_function(f, insts, NRAW, BASE);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)calloc((size_t)n + 1u, 1u);
    if (buf && fread(buf, 1u, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

/* Scope to the CALL SITE. The chunk's entry switch lists `case 0x80003008u:
   goto label_80003008;` for every leader, so a whole-text search for that goto
   matches the prologue and reports a chained call that is not there. This cost
   a false failure once already, in test_idle_pc. */
static char* call_site(const char* text) {
    static char site[4096];
    const char* a = strstr(text, "label_80003000:");
    if (!a) return NULL;
    const char* b = strstr(a, "label_80003004:");
    size_t n = (b ? (size_t)(b - a) : strlen(a));
    if (n >= sizeof(site)) n = sizeof(site) - 1u;
    memcpy(site, a, n);
    site[n] = '\0';
    return site;
}

static void expect(const char* what, int cond, const char* text) {
    if (!cond) {
        fprintf(stderr, "%s\n--- call site was ---\n%s\n", what, text ? text : "(null)");
        failures++;
    }
}

int main(void) {
    char *off, *on, *hooked, *ca_off, *ca_on;
    char site[4096];

    /* 1. --ca-liveness must not move a single byte of output. Run first, while
          no dispatch PC is registered, so the two arms are otherwise identical. */
    ca_off = emit_to_string();
    if (!ca_off) return 1;
    emit_set_ca_liveness(true);
    ca_on = emit_to_string();
    if (!ca_on) { free(ca_off); return 1; }
    expect("--ca-liveness changed the emitted code; its --help says codegen is unchanged",
           strcmp(ca_off, ca_on) == 0, NULL);
    emit_set_ca_liveness(false);
    free(ca_off);
    free(ca_on);

    /* 2. Chaining off: a local `bl` costs a dispatcher round trip. */
    off = emit_to_string();
    if (!off) return 1;
    snprintf(site, sizeof(site), "%s", call_site(off) ? call_site(off) : "");
    expect("chaining off: local bl should return to the dispatcher",
           strstr(site, "ctx->pc = 0x80003008u;") != NULL, site);
    expect("chaining off: local bl must NOT be a goto",
           strstr(site, "goto label_80003008") == NULL, site);
    free(off);

    /* 3. Chaining on: the same call becomes a native goto. */
    emit_set_chain_calls(true);
    on = emit_to_string();
    if (!on) return 1;
    snprintf(site, sizeof(site), "%s", call_site(on) ? call_site(on) : "");
    expect("chaining on: local bl should become a native goto",
           strstr(site, "goto label_80003008") != NULL, site);
    free(on);

    /* 4. THE INVARIANT. Same flag, same call -- but the target is now a PC the
          run loop hooks. It must go back to dispatching, or the hook is dead. */
    emit_add_dispatch_pc(TARGET);
    hooked = emit_to_string();
    if (!hooked) return 1;
    snprintf(site, sizeof(site), "%s", call_site(hooked) ? call_site(hooked) : "");
    expect("chained a call to a hooked PC -- that hook can never fire again",
           strstr(site, "goto label_80003008") == NULL, site);
    expect("hooked call target should return to the dispatcher",
           strstr(site, "ctx->pc = 0x80003008u;") != NULL, site);
    free(hooked);

    /* 5. --leader-cases: the entry switch should list only block LEADERS, not
          every instruction. That is what shrank the module 19.5% -- and also
          what made it a disaster at runtime, because indirect targets land on
          non-leaders and fall out to the interpreter. Pin the structural claim;
          the runtime consequence is what keeps the flag off by default. */
    {
        char* plain = emit_to_string();
        if (!plain) return 1;
        emit_set_leader_cases(true);
        char* leaders = emit_to_string();
        if (!leaders) { free(plain); return 1; }

        unsigned n_plain = 0, n_leaders = 0;
        for (const char* p = plain; (p = strstr(p, "    case 0x")) != NULL; p += 5) n_plain++;
        for (const char* p = leaders; (p = strstr(p, "    case 0x")) != NULL; p += 5) n_leaders++;

        if (!(n_leaders < n_plain)) {
            fprintf(stderr, "--leader-cases did not reduce the entry switch "
                            "(%u cases -> %u); every instruction is still a "
                            "switch-reachable join point\n", n_plain, n_leaders);
            failures++;
        }
        if (n_leaders == 0u) {
            fprintf(stderr, "--leader-cases emitted NO entry cases; the chunk "
                            "cannot be entered at all\n");
            failures++;
        }
        emit_set_leader_cases(false);
        free(plain);
        free(leaders);
    }

    return failures ? 1 : 0;
}
