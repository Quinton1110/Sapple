#!/usr/bin/env python3
"""Make OpenTV's generated data image (tools/gen_data.py in Engine/opentv-upstream) assemble for Apple targets.

gen_data.py writes ELF-flavoured GNU assembly: a `.section .data` line Apple's assembler rejects, and labels
without the leading underscore Mach-O gives every C symbol (`g_tab_5418:` does not satisfy `_g_tab_5418`).
This rewrites the file in place: `.section .data` -> `.data`, and every global label / `.globl` without a leading
underscore gains an underscored alias at the same place. No byte moves: the image is laid out exactly as gen_data
laid it out (the engine reads past the end of some tables into the next one, so order and offsets matter).
Idempotent. (The same fix as devinprater/iTruVoice's tools/macho_asm.py, written for this repo.)
"""
import re
import sys

path = sys.argv[1]
lines = open(path).read().splitlines(keepends=True)
labels = {m.group(1) for m in (re.match(r"^([A-Za-z_.][\w$.]*):", l) for l in lines) if m}
globls = {m.group(1) for m in (re.match(r"^\s*\.globl\s+(\S+)\s*$", l) for l in lines) if m}
out = []
for line in lines:
    if line.strip() == ".section .data":
        out.append(line.replace(".section .data", ".data"))
        continue
    out.append(line)
    m = re.match(r"^(\s*)\.globl\s+([A-Za-z][\w$.]*)\s*$", line)
    if m and "_" + m.group(2) not in globls:
        out.append("%s.globl _%s\n" % (m.group(1), m.group(2)))
        globls.add("_" + m.group(2))
        continue
    m = re.match(r"^([A-Za-z][\w$.]*):(.*)$", line.rstrip("\n"))
    if m and "_" + m.group(1) not in labels:
        out.append("_%s:%s\n" % (m.group(1), m.group(2)))
        labels.add("_" + m.group(1))
open(path, "w").writelines(out)
