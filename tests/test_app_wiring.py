#!/usr/bin/env python3
"""
tests/test_app_wiring.py — static checks that the app suite is actually wired,
without an ESP32 toolchain. These catch the class of bug that bites at link/run
time: a menu item or voice command with no dispatch path, a duplicate command
id, or a tool the LLM is told about but can't run.

Run: python tests/test_app_wiring.py
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, "firmware", "main")

FAILED = []
def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail and not cond else ""))
    if not cond: FAILED.append(name)

def read(f):
    return open(os.path.join(MAIN, f)).read()


def test_command_ids_unique():
    print("Voice command grammar:")
    s = read("fox_main.cpp")
    cmds = re.findall(r"\{\s*(\d+),\s*\"([^\"]*)\",\s*\"([^\"]*)\"\}", s)
    ids = [int(c[0]) for c in cmds]
    check("commands parsed", len(cmds) >= 20, f"{len(cmds)}")
    check("ids unique", len(set(ids)) == len(ids))
    check("ids 1-based contiguous", sorted(ids) == list(range(1, len(ids) + 1)),
          f"{sorted(ids)}")
    # every action resolves: it's handled explicitly in do_action OR falls to launch()
    launch_ids = set(re.findall(r'strcmp\(id,\s*"([^"]+)"\)', s))
    action_ids = set(re.findall(r'strcmp\(action,\s*"([^"]+)"\)', s))
    handled = launch_ids | action_ids
    # do_action has an else -> launch(action); launch handles the tool/game ids
    for _, _, action in cmds:
        ok = action in handled or action in launch_ids
        check(f"voice action '{action}' has a path", ok)


def test_menu_dispatch_coverage():
    print("Menu wiring:")
    inp = read("fox_input.inc")
    main = read("fox_main.cpp")
    menu_ids = re.findall(r'\{"[^"]+",\s*"([^"]+)"\}', inp)
    check("menu items parsed", len(menu_ids) >= 15, f"{len(menu_ids)}")
    handled = set(re.findall(r'strcmp\(id,\s*"([^"]+)"\)', main))
    launch = set(re.findall(r'strcmp\(id,\s*"([^"]+)"\)', main))
    # 'close' is handled inside open_menu(); everything else must be in
    # menu_dispatch (handled) or launch()
    for mid in menu_ids:
        if mid in ("close", "nop"):
            continue
        ok = mid in handled
        check(f"menu id '{mid}' dispatched", ok)


def test_tools_have_runners():
    print("LLM tool-calling:")
    main = read("fox_main.cpp")
    # tools advertised via fn("name", ...) must be runnable in run_tool()
    advertised = set(re.findall(r'fn\("([^"]+)"', main))
    runnable = set(re.findall(r'name == "([^"]+)"', main))
    check("tools advertised", len(advertised) >= 3, f"{advertised}")
    for t in advertised:
        check(f"tool '{t}' has a runner", t in runnable)


def test_no_orphan_entrypoints():
    print("No conflicting entry points / orphans:")
    files = os.listdir(MAIN)
    check("no main.c orphan", "main.c" not in files)
    check("single setup() defined",
          sum(read(f).count("void setup()") for f in files if f.endswith(".cpp")) == 1)
    check("single loop() defined",
          sum(read(f).count("void loop()") for f in files if f.endswith(".cpp")) == 1)
    # the fabricated deps must be gone from the manifest
    dep = read("idf_component.yml")
    check("nimble-cpp dep removed", "h2zero/esp-nimble-cpp" not in dep
          or dep.strip().startswith("#") or "# " in dep.split("esp-nimble-cpp")[0].split("\n")[-1])
    check("picotts not a versioned registry dep",
          not re.search(r'^\s*jmattsson/esp-picotts:\s*"\^', dep, re.M))


def test_bayes_kb():
    print("Bayesian 20Q knowledge base:")
    import math
    s = read("fox_bayes.inc")
    feats = re.search(r"enum \{([^}]+)NFEAT", s).group(1)
    fnames = [x.strip() for x in feats.split(",") if x.strip() and "NFEAT" not in x]
    rows = re.findall(r'\{"([^"]+)",\s*\{([0-9,\s]+)\}\}', s)
    nf = len(fnames)
    check("features parsed", nf >= 6, f"{nf}")
    check("entities parsed", len(rows) >= 6, f"{len(rows)}")
    ent = {}
    lens_ok = True
    for name, vals in rows:
        nums = [int(x) for x in vals.split(",") if x.strip()]
        ent[name] = nums
        if len(nums) != nf:
            lens_ok = False
    check("every entity row has NFEAT probs", lens_ok)
    # probabilities are 0..100
    check("probs in 0..100", all(0 <= v <= 100 for v in sum(ent.values(), [])))
    # run the guesser: honest answers must identify every entity
    def entropy(b): return -sum(p*math.log(p+1e-12) for p in b.values() if p > 0)
    def infogain(b, f):
        py = sum(b[e]*ent[e][f]/100 for e in b); pn = 1-py
        if py < 1e-3 or pn < 1e-3: return -1
        by = {e: b[e]*ent[e][f]/100 for e in b}; sy = sum(by.values())
        bn = {e: b[e]*(1-ent[e][f]/100) for e in b}; sn = sum(bn.values())
        hy = -sum((v/sy)*math.log(v/sy+1e-12) for v in by.values() if v > 0) if sy else 0
        hn = -sum((v/sn)*math.log(v/sn+1e-12) for v in bn.values() if v > 0) if sn else 0
        return entropy(b) - (py*hy + pn*hn)
    def upd(b, f, yes):
        nb = {e: b[e]*((ent[e][f]/100) if yes else (1-ent[e][f]/100)) for e in b}
        s = sum(nb.values()) or 1
        return {e: v/s for e, v in nb.items()}
    def play(secret):
        b = {e: 1/len(ent) for e in ent}; asked = set()
        for _ in range(12):
            t = max(b, key=b.get)
            if b[t] > 0.8:
                if t == secret: return t
                b[t] = 0; s = sum(b.values())
                if s: b = {e: v/s for e, v in b.items()}
            best, bg = None, -1
            for f in range(nf):
                if f in asked: continue
                g = infogain(b, f)
                if g > bg: bg, best = g, f
            if best is None: break
            asked.add(best); b = upd(b, best, ent[secret][best] > 50)
        return max(b, key=b.get)
    correct = sum(play(e) == e for e in ent)
    check("guesser identifies all entities (honest answers)", correct == len(ent),
          f"{correct}/{len(ent)}")


if __name__ == "__main__":
    print("=== Fox app-wiring tests ===")
    test_command_ids_unique()
    test_menu_dispatch_coverage()
    test_tools_have_runners()
    test_no_orphan_entrypoints()
    test_bayes_kb()
    print()
    if FAILED:
        print(f"FAILED: {len(FAILED)}: {FAILED}"); sys.exit(1)
    print("All wiring checks passed.")
