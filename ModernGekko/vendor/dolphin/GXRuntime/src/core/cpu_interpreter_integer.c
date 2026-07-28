#include "cpu_interpreter_private.h"

bool ppc_lwarx_op(CPUState* cpu, u8 d, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return false;
    }
    cpu->gpr[d] = mem_read32(cpu, ea);
    cpu->reserve_valid = true;
    cpu->reserve_addr = ea;
    return true;
}

void ppc_stwcx_op(CPUState* cpu, u8 s, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }
    u32 so_bit = (cpu->xer >> 31) & 1u; /* XER.SO -> CR0[SO] */
    if (cpu->reserve_valid && ea == cpu->reserve_addr) {
        mem_write32(cpu, ea, cpu->gpr[s]);
        cpu->reserve_valid = false;
        cpu->cr = (cpu->cr & 0x0FFFFFFFu) | ((2u | so_bit) << 28);
        return;
    }
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (so_bit << 28);
}

void ppc_stsw(CPUState* cpu, u32 ea, u32 n, u8 r, u32 cia) {
    if (cpu->msr & PPC_MSR_LE) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }
    if (n == 0)
        return;

    u32 misalignment_bytes = ea & 3u;
    u32 misalignment_bits = misalignment_bytes * 8u;
    u32 current_address = ea & ~3u;
    u64 current_value = 0;

    if (misalignment_bytes != 0) {
        current_value = mem_read32(cpu, current_address);
        current_value <<= misalignment_bits;
        current_value &= 0xFFFFFFFF00000000ull;
        n += misalignment_bytes;
    }

    while (n >= 4) {
        current_value |= cpu->gpr[r];
        mem_write32(cpu, current_address, (u32)(current_value >> misalignment_bits));
        current_value <<= 32;
        current_address += 4;
        n -= 4;
        r = (u8)((r + 1) & 0x1Fu);
    }

    if (n != 0) {
        if (n > misalignment_bytes) {
            current_value |= cpu->gpr[r];
            current_value <<= (n - misalignment_bytes) * 8u;
        } else {
            current_value >>= (misalignment_bytes - n) * 8u;
        }
        current_value &= 0xFFFFFFFF00000000ull;
        current_value |= ((u64)mem_read32(cpu, current_address) << (n * 8u)) & 0xFFFFFFFFull;
        mem_write32(cpu, current_address, (u32)(current_value >> (n * 8u)));
    }
}
