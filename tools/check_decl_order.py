#!/usr/bin/env python3
"""Flag file-scope statics used before their declaration in fox_main.cpp's
translation unit (fox_main.cpp with its #include "*.inc" expanded in order).
Catches the 'X was not declared in this scope' class of build break."""
import re, os, sys
D = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "firmware", "main")

def expand(name, seen):
    out = []
    for line in open(os.path.join(D, name)):
        m = re.match(r'\s*#include\s+"([^"]+\.inc)"', line)
        if m and m.group(1) not in seen:
            seen.add(m.group(1)); out += expand(m.group(1), seen)
        else:
            out.append(line)
    return out

def strip(s):  # drop comments and string/char literals, keep line structure
    s = re.sub(r'/\*.*?\*/', lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
    s = re.sub(r"'(\\.|[^'\\\n])'", "''", s)          # char literals FIRST ('"' must not open a string)
    s = re.sub(r'"(\\.|[^"\\\n])*"', '""', s)          # strings never span lines
    s = re.sub(r'//[^\n]*', '', s)
    return s

lines = strip("".join(expand("fox_main.cpp", set()))).split("\n")
decl = {}   # name -> first line where declared (definition or prototype)
pat = re.compile(r'^(?:static\s+)?(?:inline\s+)?(?:const\s+)?[A-Za-z_][\w:<>,\s\*&]*?[\s\*&]([A-Za-z_]\w*)\s*(\(|=|;|\[)')
depth = 0
decl_depth = {}
for i, l in enumerate(lines):
    # track brace depth so we only consider TRUE file-scope (depth 0) decls
    stripped_depth = depth
    if not l or l[0] in " \t#}" or l.startswith(("struct", "enum", "using", "typedef", "template", "class", "return", "namespace")):
        depth += l.count("{") - l.count("}")
        continue
    if stripped_depth != 0:
        depth += l.count("{") - l.count("}")
        continue
    m = pat.match(l)
    if m and m.group(1) not in decl and m.group(1) not in ("if", "for", "while", "switch"):
        decl[m.group(1)] = i
    depth += l.count("{") - l.count("}")
# names prototyped in the project headers are declared before any use
hdr = set()
for h in ("fox.h", "fox_decls.h", "foxese.h", "fox_audio.h"):
    p = os.path.join(D, h)
    if os.path.exists(p):
        hdr |= set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', strip(open(p).read())))
        hdr |= set(re.findall(r'\bextern\s+[\w:<>\s\*&]+?\b([A-Za-z_]\w*)\s*;', strip(open(p).read())))
bad = []
for name, d in decl.items():
    if name in hdr: continue
    rx = re.compile(r'(?<![.>])\b' + re.escape(name) + r'\b')   # skip obj.member / ptr->member
    for i in range(d):
        if rx.search(lines[i]):
            bad.append((name, i + 1, d + 1)); break
for n, u, d in sorted(bad, key=lambda x: x[1]):
    print(f"USED BEFORE DECLARED: {n} (used line {u}, declared line {d} of the expanded unit)")
print(f"checked {len(decl)} file-scope declarations: {len(bad)} problem(s)")
sys.exit(1 if bad else 0)
