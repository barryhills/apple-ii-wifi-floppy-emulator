#!/usr/bin/env python3
"""Check that the firmware's core-1 code never touches flash.

Core 1 runs the drive while core 0 writes flash, when flash cannot be read
(drive.c). So every instruction core 1 can reach must be in RAM, and it must
not load flash addresses (constant tables, strings) either.

Walks the call graph from core1_main in `arm-none-eabi-objdump -d` output:
every direct branch/call target must be a RAM address (0x2000_0000+) and is
followed; an indirect call (blx/bx through a register other than lr) fails
the check, since its target is unknown; every literal-pool word in a reached
function that holds a flash address (0x1000_0000-0x11ff_ffff) is reported.

usage: SW/tools/check_core1_ram.py [SW/firmware/build/a2drive.elf [start-function]]
(start-function other than core1_main: only to check the checker)
"""
import re
import subprocess
import sys

elf = sys.argv[1] if len(sys.argv) > 1 else "SW/firmware/build/a2drive.elf"
dis = subprocess.run(["arm-none-eabi-objdump", "-d", "--no-show-raw-insn", elf],
                     capture_output=True, text=True, check=True).stdout

funcs = {}          # name -> (addr, [lines])
addr_of = {}
cur = None
for line in dis.splitlines():
    m = re.match(r"^([0-9a-f]{8}) <(.+)>:$", line)
    if m:
        cur = m.group(2)
        funcs[cur] = (int(m.group(1), 16), [])
        addr_of[int(m.group(1), 16)] = cur
        continue
    if cur and re.match(r"^\s*[0-9a-f]+:\s", line):
        funcs[cur][1].append(line)

def is_ram(a):
    return 0x20000000 <= a < 0x20082000

def is_flash(a):
    return 0x10000000 <= a < 0x12000000

start = sys.argv[2] if len(sys.argv) > 2 else "core1_main"
if start not in funcs:
    sys.exit(f"{start} not found in {elf}")

seen, todo, problems = set(), [start], []
while todo:
    f = todo.pop()
    if f in seen:
        continue
    seen.add(f)
    addr, lines = funcs[f]
    if not is_ram(addr):
        problems.append(f"{f} is at {addr:#010x} (flash)")
        continue
    for line in lines:
        ins = line.split(":", 1)[1].strip()
        # direct branches and calls: "bl 20001234 <name>", "b.w ...", "beq.n ..."
        m = re.match(r"^(b[a-z]*(?:\.[nw])?)\s+([0-9a-f]+)\s+<([^>+]+)(\+0x[0-9a-f]+)?>", ins)
        if m:
            tgt, name, off = int(m.group(2), 16), m.group(3), m.group(4)
            if not is_ram(tgt):
                problems.append(f"{f}: {ins}")
            elif not off and name != f:
                todo.append(name)
            continue
        m = re.match(r"^(blx|bx)\s+(r\d+|ip|sl|fp)\b", ins)
        if m:
            problems.append(f"{f}: indirect call: {ins}")
            continue
        m = re.match(r"^\.word\s+0x([0-9a-f]+)", ins)
        if m and is_flash(int(m.group(1), 16)):
            problems.append(f"{f}: literal {ins} points into flash")

print("core 1 reaches:", ", ".join(sorted(seen)))
if problems:
    print("PROBLEMS:")
    for p in problems:
        print("  " + p)
    sys.exit(1)
print("OK: all core-1 code is in RAM and loads no flash addresses")
