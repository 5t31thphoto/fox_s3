#!/usr/bin/env python3
"""Pre-flight checks that mirror what the ESP-IDF build ACTUALLY fails on, so we
catch build-breakers before a 20-minute CI run. From the real compile flags:
  -Werror=all is ON, with -Wno-error for: unused-function, unused-variable,
  unused-but-set-variable, deprecated-declarations, misleading-indentation(NO),
  ... (see build log). So these are FATAL and we check them:
    1. redefinition of a file-scope object/define (same name twice at col 0)
    2. duplicate #define of the same macro with different intent
    3. misleading-indentation: 'if(x) A; B;' where B looks guarded but isn't
    4. stray non-ascii inside "..." string literals (renders as garbage / can warn)
Warnings that CI tolerates (unused-function etc.) are reported as notes only.
Exit 1 only on fatal classes."""
import re, os, sys, glob

D = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "firmware", "main")
files = sorted(glob.glob(os.path.join(D, "*.cpp")) + glob.glob(os.path.join(D, "*.inc")) +
               glob.glob(os.path.join(D, "*.h")))

def nocomment(s):
    s = re.sub(r'/\*.*?\*/', lambda m: "\n"*m.group(0).count("\n"), s, flags=re.S)
    out = []
    for line in s.split("\n"):
        out.append(re.sub(r'//.*$', '', line))
    return "\n".join(out)

fatal, notes = [], []

# ---- 1 & 2: redefinitions across the TU (fox_main.cpp + its .inc includes) ----
def expand(name, seen):
    out = []
    for i, line in enumerate(open(os.path.join(D, name))):
        m = re.match(r'\s*#include\s+"([^"]+\.inc)"', line)
        if m and m.group(1) not in seen:
            seen.add(m.group(1)); out += expand(m.group(1), seen)
        else:
            out.append((name, i+1, line))
    return out

unit = expand("fox_main.cpp", set())
defs = {}       # macro -> (file,line,body)
objs = {}       # file-scope var/array name -> (file,line)
depth = 0
for fn, ln, raw in unit:
    line = re.sub(r'//.*$', '', raw)
    md = re.match(r'\s*#define\s+(\w+)\b(.*)$', line)
    if md:
        name, body = md.group(1), md.group(2).strip()
        if name in defs and defs[name][2] != body:
            fatal.append(f"{fn}:{ln}: redefinition of macro '{name}' (first at {defs[name][0]}:{defs[name][1]})")
        defs.setdefault(name, (fn, ln, body))
        continue
    # file-scope object definitions: a line at column 0 that declares a static array/var
    if line[:1] in (" ", "\t", "#", "}", "/") or not line.strip():
        depth += line.count("{") - line.count("}"); continue
    if depth == 0:
        m = re.match(r'(?:static\s+)?(?:const\s+)?[\w:]+(?:\s*[\*&])?\s+(\w+)\s*(\[|=|;)', line)
        if m and "(" not in line.split(m.group(1))[0]:
            nm = m.group(1)
            if nm in objs:
                fatal.append(f"{fn}:{ln}: redefinition of '{nm}' (first at {objs[nm][0]}:{objs[nm][1]})")
            else:
                objs[nm] = (fn, ln)
    depth += line.count("{") - line.count("}")

# ---- 3: misleading-indentation ----
# pattern: 'if (cond) STMT1; STMT2;' on one line where STMT2 is indented as a
# new statement — gcc -Wmisleading-indentation flags the multi-statement-after-if
for f in files:
    for ln, raw in enumerate(nocomment(open(f).read()).split("\n"), 1):
        # if (...) something;  more;   -> two ';' after a close-paren if, no braces
        # find 'if (...)' then check what follows is NOT a brace and has 2+ stmts
        mm = re.match(r'\s*if\s*\(', raw)
        if mm:
            # balance the if-condition parens to find what comes after
            i = raw.index('(', mm.start()); depthp = 0; j = i
            while j < len(raw):
                if raw[j] == '(': depthp += 1
                elif raw[j] == ')':
                    depthp -= 1
                    if depthp == 0: break
                j += 1
            after = raw[j+1:]
            # blank out string/char contents so ';' inside them isn't counted
            after = re.sub(r'"(?:\\.|[^"\\])*"', '""', after)
            after = re.sub(r"'(?:\\.|[^'\\])*'", "''", after).strip()
            # misleading only if: no brace body, a stmt ending ';', THEN another stmt
            if (after and after[0] != '{'
                    and not re.match(r'(for|while|do|if)\b', after)
                    and 'else' not in after):
                # count top-level statements (';' not in a string)
                semis = [k for k, ch in enumerate(after) if ch == ';']
                if len(semis) >= 1 and after[semis[0]+1:].strip() and not after[semis[0]+1:].strip().startswith('}'):
                    fatal.append(f"{os.path.basename(f)}:{ln}: misleading-indentation: statement after if();stmt; without braces")

# ---- 4: non-ascii inside string literals ----
for f in files:
    for ln, raw in enumerate(open(f, "rb").read().split(b"\n"), 1):
        try: raw.decode("ascii"); continue
        except UnicodeDecodeError: pass
        s = raw.decode("utf-8", "replace")
        # non-ascii only MATTERS in strings that reach the 128px display
        for lit in re.findall(r'"(?:\\.|[^"\\])*"', s):
            if any(ord(c) > 127 for c in lit):
                if any(k in s for k in ("speak(", "face_caption", "drawString", "show_msg")):
                    fatal.append(f"{os.path.basename(f)}:{ln}: non-ascii in DISPLAYED string {lit[:36]!r}")
                break

# ---- notes: unused statics (CI tolerates, but worth knowing) ----
for f in files:
    txt = nocomment(open(f).read())
    for m in re.finditer(r'static\s+[\w:<>\*&\s]+?\b(\w+)\s*\([^;]*\)\s*\{', txt):
        nm = m.group(1)
        if nm in ("if","for","while","switch"): continue
        if len(re.findall(r'\b'+re.escape(nm)+r'\b', txt)) == 1:
            notes.append(f"{os.path.basename(f)}: static '{nm}' defined but never used (CI warns, not fatal)")

for n in notes[:12]: print("note:", n)
for e in fatal: print("FATAL:", e)
print(f"\npreflight: {len(fatal)} fatal, {len(notes)} notes across {len(files)} files")
sys.exit(1 if fatal else 0)
