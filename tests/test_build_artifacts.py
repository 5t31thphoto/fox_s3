#!/usr/bin/env python3
"""
tests/test_build_artifacts.py — validate the build-time artifacts the firmware
depends on, without needing an ESP32 toolchain. Run: python tests/test_build_artifacts.py

These are the checks that actually caught bugs during development:
  1. IR converter produces a well-formed FOXI blob whose every offset is in
     bounds and whose decoded protocol headers match known-good timings.
  2. The brain trainer produces a FOXB blob whose byte layout is consumed
     EXACTLY (to the last byte) by a reader that mirrors fox_llm_forward.inc —
     the trainer and firmware must agree or inference reads garbage.
  3. The MultiNet command grammar has unique 1-based IDs and distinct phrases.
"""
import os, sys, struct, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

FAILED = []
def check(name, cond, detail=""):
    tag = "PASS" if cond else "FAIL"
    print(f"  [{tag}] {name}" + (f" — {detail}" if detail and not cond else ""))
    if not cond:
        FAILED.append(name)


# ---------------------------------------------------------------------------
def test_ir():
    print("IR converter (FOXI):")
    irdir = os.path.join(ROOT, "assets", "ir")
    if not os.path.isdir(irdir):
        check("ir assets present", False, "assets/ir missing"); return
    out = os.path.join(tempfile.gettempdir(), "fox_ir_test.bin")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "build_ir.py"),
                        "--out", out,
                        "--tv", os.path.join(irdir, "tv.ir"),
                        "--audio", os.path.join(irdir, "audio.ir"),
                        "--projector", os.path.join(irdir, "projector.ir"),
                        "--ac", os.path.join(irdir, "ac.ir")],
                       capture_output=True, text=True)
    check("build_ir runs", r.returncode == 0, r.stderr)
    if r.returncode != 0:
        return
    blob = open(out, "rb").read()
    check("FOXI magic", blob[:4] == b"FOXI")
    ver, n = struct.unpack_from("<HH", blob, 4)
    strtab_off, strtab_len = struct.unpack_from("<II", blob, 8)
    check("version 2", ver == 2)
    check("has codes", n > 100, f"only {n}")

    # every entry: offsets in-bounds, freq sane
    ok_off = True; ok_freq = True; power = 0
    headers = {}
    for i in range(n):
        e = blob[16 + i*16: 16 + i*16 + 16]
        name_off, cat, btn, fdiv, flags, doff, npair, pad = struct.unpack("<IBBBBIHH", e)
        if name_off >= len(blob) or doff + npair*4 > len(blob):
            ok_off = False
        if not (28 <= fdiv*250 <= 60000):
            ok_freq = False
        if flags & 1: power += 1
        if i < 400:  # sample first-pair header of some codes
            first = struct.unpack_from("<HH", blob, doff)
            headers.setdefault(first, 0)
            headers[first] += 1
    check("all offsets in-bounds", ok_off)
    check("all carriers 28–60kHz", ok_freq)
    check("power pool populated", power > 50, f"{power} power codes")
    # known protocol leader pairs should appear among decoded headers
    known = {(9000,4500):"NEC", (2400,600):"SIRC", (4500,4500):"Samsung", (2666,889):"RC6"}
    seen = [v for k,v in known.items() if k in headers]
    check("known protocol headers present", len(seen) >= 3, f"saw {seen}")


# ---------------------------------------------------------------------------
def test_brain():
    print("Brain trainer (FOXB) byte contract:")
    out = os.path.join(tempfile.gettempdir(), "foxbrain_test.bin")
    # tiny fast run just to validate the byte layout, not quality
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "train_brain.py"),
                        "--pack", "A", "--out", out, "--steps", "60", "--repeat", "2"],
                       capture_output=True, text=True)
    check("train_brain runs", r.returncode == 0, r.stderr[-400:] if r.stderr else "")
    if r.returncode != 0:
        return
    blob = open(out, "rb").read()
    off = 0
    check("FOXB magic", blob[:4] == b"FOXB"); off = 4
    ver, = struct.unpack_from("<I", blob, off); off += 4
    dim, hidden, L, H, KV, vocab, seq, shared = struct.unpack_from("<iiiiiiii", blob, off); off += 32
    gs, = struct.unpack_from("<i", blob, off); off += 4; off += 24
    check("version 1", ver == 1)
    check("shared classifier", shared == 1)
    check("dims sane", 0 < dim <= 512 and 0 < L <= 12, f"dim={dim} L={L}")
    kvdim = (dim * KV) // H

    def skip_f32(n):
        nonlocal off; off += n*4
    def skip_q8(rows, cols):
        nonlocal off; off += rows*cols + (rows*cols//gs)*4

    skip_f32(vocab*dim)                 # tok_emb
    skip_f32(L*dim); skip_f32(L*dim)    # rms_att, rms_ffn
    skip_f32(dim)                       # rms_final
    for _ in range(L): skip_q8(dim, dim)      # wq
    for _ in range(L): skip_q8(kvdim, dim)    # wk
    for _ in range(L): skip_q8(kvdim, dim)    # wv
    for _ in range(L): skip_q8(dim, dim)      # wo
    for _ in range(L): skip_q8(hidden, dim)   # w1
    for _ in range(L): skip_q8(dim, hidden)   # w2
    for _ in range(L): skip_q8(hidden, dim)   # w3
    # vocab blob
    vocab_ok = True
    for i in range(vocab):
        if off + 6 > len(blob): vocab_ok = False; break
        off += 4
        ln, = struct.unpack_from("<H", blob, off); off += 2; off += ln
    check("vocab blob well-formed", vocab_ok)
    check("reader consumes EXACTLY all bytes", off == len(blob),
          f"consumed {off} of {len(blob)}")


# ---------------------------------------------------------------------------
def test_foxese():
    print("FOXESE semantic IR contract:")
    src = open(os.path.join(ROOT, "firmware", "main", "foxese.cpp")).read()
    trainer = open(os.path.join(ROOT, "tools", "train_brain.py")).read()
    check("FOXESE packet shape is fixed", "s.length() != 13" in src)
    check("FOXESE has version/mood/fact/style/gesture/intensity",
          all(x in src for x in ["version", "mood", "fact_id", "style", "gesture", "intensity"]))
    check("trainer emits semantic tail", "S{style:X}G{gesture:X}E{intensity:X}" in trainer)
    check("firmware constructs immutable packet", "foxese_encode((uint8_t)mood, fid" in open(os.path.join(ROOT, "firmware", "main", "fox_llm.cpp")).read())
    check("model output is constrained", "llm_foxese_tail" in open(os.path.join(ROOT, "firmware", "main", "fox_llm_forward.inc")).read())


# ---------------------------------------------------------------------------
def test_commands():
    print("MultiNet command registry:")
    main = open(os.path.join(ROOT, "firmware", "main", "fox_main.cpp")).read()
    import re
    # extract the COMMANDS[] table lines like {1, "...", "..."}
    ids = re.findall(r"\{\s*(\d+),\s*\"([^\"]*)\",\s*\"([^\"]*)\"\}", main)
    check("commands found", len(ids) >= 8, f"{len(ids)}")
    id_nums = [int(i[0]) for i in ids]
    check("ids are 1-based & unique", len(set(id_nums)) == len(id_nums) and min(id_nums) >= 1)
    # phrases distinct
    phrases = [i[1] for i in ids]
    check("phrases distinct", len(set(phrases)) == len(phrases))
    # actions all dispatched in do_action
    for _, _, action in ids:
        check(f"action '{action}' dispatched", f'"{action}"' in main)


if __name__ == "__main__":
    print("=== Fox build-artifact tests ===")
    test_ir()
    test_brain()
    test_foxese()
    test_commands()
    print()
    if FAILED:
        print(f"FAILED: {len(FAILED)} check(s): {FAILED}")
        sys.exit(1)
    print("All checks passed.")
