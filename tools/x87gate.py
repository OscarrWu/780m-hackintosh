#!/usr/bin/env python3
"""x87gate.py - post-link V2 gate: no reachable x87 in the kext.

Two-stage check (Main adjudication 2026-11-02):
  stage 1  linear llvm-objdump scan for x87 mnemonics.  Cheap, but has a known
           false positive: switch jump tables inside __text are bytes that the
           linear sweep misreads as code (e.g. d8 f0 = fdiv).
  stage 2  control-flow reachability.  An x87 mnemonic only fails the build when
           it is reachable from its function's entry point.  Bytes that are not
           reachable are data (jump table) and are reported, not failed.

Usage:
  x87gate.py [--objdump PATH] [--selftest] <MachO-binary>

Exit codes: 0 pass, 1 reachable x87 found / selftest failure, 2 usage error.
"""

import argparse
import re
import subprocess
import sys

# x87 FPU mnemonics (objdump spelling).  SSE is handled by a separate, wider
# policy: va_start register saves are store-only and recorded as a deviation.
X87 = {
    "f2xm1", "fabs", "fadd", "faddp", "fbld", "fbstp", "fchs", "fclex",
    "fcmovbe", "fcmovb", "fcmove", "fcmovnbe", "fcmovnb", "fcmovne", "fcmovnu",
    "fcmovu", "fcom", "fcomi", "fcomip", "fcomp", "fcompp", "fcos", "fdecstp",
    "fdiv", "fdivp", "fdivr", "fdivrp", "ffree", "ffreep", "fiadd", "ficom",
    "ficomp", "fidiv", "fidivr", "fild", "fimul", "fincstp", "finit", "fist",
    "fistp", "fisttp", "fisub", "fisubr", "fld", "fld1", "fldcw", "fldenv",
    "fldl2e", "fldl2t", "fldlg2", "fldln2", "fldpi", "fldz", "fmul", "fmulp",
    "fnclex", "fninit", "fnop", "fnstcw", "fnstenv", "fnstsw", "fpatan",
    "fprem", "fprem1", "fptan", "frndint", "frstor", "fsave", "fscale",
    "fsin", "fsincos", "fsqrt", "fst", "fstcw", "fstenv", "fstp", "fstsw",
    "fsub", "fsubp", "fsubr", "fsubrp", "ftst", "fucom", "fucomi", "fucomip",
    "fucomp", "fucompp", "fxam", "fxch", "fxrstor", "fxsave", "fxtract",
    "fyl2x", "fyl2xp1",
}

FUNC = re.compile(r"^([0-9a-fA-F]+)\s+<(.+)>:$")
INSN = re.compile(r"^\s*([0-9a-fA-F]+):(?:\s+[0-9a-fA-F ]+)?\s+(\S+)(.*)$")
TARGET = re.compile(r"0x([0-9a-fA-F]+)")
TERMINATORS = {"ret", "retq", "retf", "iret", "iretq", "hlt", "ud2"}


def parse(text):
    """Parse objdump -d output -> [(func_addr, func_name, [insn])]."""
    funcs = []
    cur = None
    for line in text.splitlines():
        m = FUNC.match(line)
        if m:
            cur = (int(m.group(1), 16), m.group(2), [])
            funcs.append(cur)
            continue
        m = INSN.match(line)
        if m and cur is not None:
            cur[2].append((int(m.group(1), 16), m.group(2).lower(),
                           m.group(3).strip()))
    return funcs


def reachable(func):
    """Addresses reachable from the function entry (intra-function CFG)."""
    addr, _name, insns = func
    if not insns:
        return set()
    order = [i[0] for i in insns]
    next_addr = dict(zip(order, order[1:]))
    seen = set()
    work = [insns[0][0]]
    while work:
        a = work.pop()
        if a in seen or a not in next_addr and a != order[-1]:
            continue
        seen.add(a)
        idx = order.index(a)
        mn, ops = insns[idx][1], insns[idx][2]
        if mn.startswith("<") or mn == "<unknown>":
            continue  # data: linear sweep failed, stop this path
        m = TARGET.search(ops)
        target = int(m.group(1), 16) if (m and "*" not in ops) else None
        if mn.startswith("j") or mn.startswith("loop"):
            if target is not None and addr <= target < order[-1] + 1:
                work.append(target)
            if mn == "jmp" or mn.startswith("loop"):
                continue  # unconditional: no fall-through
        elif mn in TERMINATORS:
            continue
        nxt = next_addr.get(a)
        if nxt is not None:
            work.append(nxt)
    return seen


def check(text):
    funcs = parse(text)
    candidates, failures, data = [], [], []
    for fa, name, insns in funcs:
        reach = reachable((fa, name, insns))
        for ia, mn, ops in insns:
            if mn in X87:
                candidates.append((fa, name, ia, mn, ops))
                if ia in reach:
                    failures.append((fa, name, ia, mn, ops))
                else:
                    data.append((fa, name, ia))
    return candidates, failures, data


def selftest():
    # Case A: jump-table bytes inside a function are unreachable data -> pass.
    table = """0000000000001380c <mes_substep>:
   15f2b: e9 16 f8 ff ff                jmpq	0x15746 <mes_substep+0x1f3a>
   15f30: 02 ee ff ff                   addb	%dh, %ch
   15f32: ff ff                         <unknown>
   15f50: d8 f0                         fdiv	%st(0), %st
"""
    _c, fail, data = check(table)
    if fail or len(data) != 1:
        print("selftest A FAIL (jump table must be data, not reachable x87)")
        return 1
    # Case B: a reachable x87 instruction must fail.
    real = """0000000000001000 <uses_fpu>:
   1000: 55                            pushq	%rbp
   1001: 48 89 e5                      movq	%rsp, %rbp
   1004: d9 ee                         fldz
   1006: 5d                            popq	%rbp
   1007: c3                            retq
"""
    _c, fail, _d = check(real)
    if not fail:
        print("selftest B FAIL (reachable x87 must fail)")
        return 1
    print("selftest ok (jump table = data; reachable x87 = failure)")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", default="llvm-objdump")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("binary", nargs="?")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.binary:
        ap.error("need a MachO binary (or --selftest)")
    out = subprocess.run([args.objdump, "-d", "--arch=x86_64", args.binary],
                         capture_output=True, text=True, check=True).stdout
    candidates, failures, data = check(out)
    for fa, name, ia, mn, ops in failures:
        print(f"X87 GATE FAIL: 0x{ia:x} {mn} {ops} reachable in "
              f"0x{fa:x} <{name}>")
    print(f"x87 gate: candidates={len(candidates)} reachable={len(failures)} "
          f"data={len(data)}"
          + (f" (data at: {', '.join(f'0x{ia:x}' for _f, _n, ia in data)})"
             if data else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
