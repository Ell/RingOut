#!/usr/bin/env python3
"""Join a perf srcline extract against the emitter's per-instruction mnemonics.

Split from the recording half on purpose: `perf report` must run where the .so
sits at the path perf recorded (the Deck), while the generated chunks that carry
the `// <addr>: <mnem>` comments live on the build machine. So the Deck produces
a small text file of "<pct>% chunk_x.c:<line>" and this does the mapping.

    opcode-join.py <lines.txt> <generated-dir> [top-N]
"""
import bisect
import collections
import re
import sys

LABEL = re.compile(r"label_([0-9A-F]{8}):")
MNEM = re.compile(r"^\s*//\s*([0-9A-Fa-f]{8}):\s+(\S+)")
ROW = re.compile(r"\s*([0-9.]+)%\s+(chunk_[^\s]+\.c):(\d+)")

# Guest opcodes grouped into the classes a codegen change would actually target.
CLASS = {}
for m in "lwz lwzu lwzx lhz lhzu lhzx lha lhax lbz lbzu lbzx lmw".split():
    CLASS[m] = "int load"
for m in "stw stwu stwx sth sthu sthx stb stbu stbx stmw".split():
    CLASS[m] = "int store"
for m in "lfs lfsu lfsx lfd lfdu lfdx".split():
    CLASS[m] = "fp load"
for m in "stfs stfsu stfsx stfd stfdu stfdx stfiwx".split():
    CLASS[m] = "fp store"
for m in "psq_l psq_lu psq_lx psq_st psq_stu psq_stx".split():
    CLASS[m] = "paired-single ld/st"
for m in ("cmpw cmpwi cmplw cmplwi cmpi cmp cmpl cmpli fcmpo fcmpu").split():
    CLASS[m] = "compare"
for m in "bc bcctr bcctrl bclr bclrl b bl blr bctr bctrl".split():
    CLASS[m] = "branch"
for m in ("add addi addis addc adde addic addme addze subf subfc subfe "
          "subfic subfme subfze neg mullw mulli mulhw mulhwu divw divwu").split():
    CLASS[m] = "int arith"
for m in ("and andi. andis. or ori oris xor xori xoris nand nor eqv andc orc "
          "extsb extsh cntlzw rlwinm rlwinm. rlwimi rlwnm slw srw sraw srawi "
          "srawi. slwi srwi").split():
    CLASS[m] = "logic/shift"
for m in ("fadd fadds fsub fsubs fmul fmuls fdiv fdivs fmadd fmadds fmsub "
          "fmsubs fnmadd fnmadds fnmsub fnmsubs fabs fneg fmr fnabs frsp "
          "fctiw fctiwz fres frsqrte fsel").split():
    CLASS[m] = "fp arith"
for m in ("ps_add ps_sub ps_mul ps_div ps_madd ps_msub ps_nmadd ps_nmsub "
          "ps_sum0 ps_sum1 ps_muls0 ps_muls1 ps_madds0 ps_madds1 ps_merge00 "
          "ps_merge01 ps_merge10 ps_merge11 ps_mr ps_neg ps_abs ps_res "
          "ps_rsqrte ps_sel ps_cmpo0 ps_cmpu0").split():
    CLASS[m] = "paired-single arith"
for m in ("mflr mtlr mfctr mtctr mfcr mtcrf mfmsr mtmsr mfspr mtspr mffs "
          "mtfsf mtfsb0 mtfsb1 mcrf mcrfs crand cror crxor crnand crnor "
          "creqv crandc crorc isync sync eieio icbi dcbf dcbi dcbst dcbt "
          "dcbtst dcbz").split():
    CLASS[m] = "system/spr"


def index(gen, chunk, cache={}):
    key = (gen, chunk)
    if key not in cache:
        nums, info = [], []
        try:
            with open(f"{gen}/chunks/{chunk}") as fh:
                for n, line in enumerate(fh, 1):
                    m = LABEL.match(line)
                    if m:
                        nums.append(n)
                        info.append([int(m.group(1), 16), None])
                        continue
                    m = MNEM.match(line)
                    if m and info:
                        info[-1][1] = m.group(2).lower()
        except OSError:
            pass
        cache[key] = (nums, info)
    return cache[key]


def main():
    lines_file, gen = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 30

    by_mnem = collections.Counter()
    by_class = collections.Counter()
    switch = 0.0
    unmapped = 0.0

    for raw in open(lines_file):
        m = ROW.match(raw)
        if not m:
            continue
        pct, chunk, num = float(m.group(1)), m.group(2), int(m.group(3))
        nums, info = index(gen, chunk)
        if not nums:
            unmapped += pct
            continue
        i = bisect.bisect_right(nums, num) - 1
        if i < 0:
            switch += pct
            continue
        mnem = info[i][1]
        if mnem is None:
            unmapped += pct
            continue
        by_mnem[mnem] += pct
        by_class[CLASS.get(mnem, "other")] += pct

    total = sum(by_mnem.values()) + switch
    if total <= 0:
        sys.exit("nothing mapped")

    print("BY CLASS")
    print(f"{'class':<24}{'% of CPU thread':>17}{'% of mapped':>14}")
    print("-" * 55)
    print(f"{'(chunk entry switch)':<24}{switch:>16.2f}%{100*switch/total:>13.2f}%")
    for cls, pct in by_class.most_common():
        print(f"{cls:<24}{pct:>16.2f}%{100*pct/total:>13.2f}%")
    print()
    print("BY MNEMONIC")
    print(f"{'mnemonic':<24}{'% of CPU thread':>17}{'% of mapped':>14}")
    print("-" * 55)
    for mnem, pct in by_mnem.most_common(top):
        print(f"{mnem:<24}{pct:>16.2f}%{100*pct/total:>13.2f}%")
    print("-" * 55)
    print(f"{'mapped total':<24}{total:>16.2f}%")
    if unmapped:
        print(f"{'unmapped':<24}{unmapped:>16.2f}%")


if __name__ == "__main__":
    main()
