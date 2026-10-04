#!/usr/bin/env python3
"""
train_brain.py — train the tiny "fox brain" from scratch (pure numpy, CPU-only,
no torch) and export it in the exact FOXB byte layout that firmware/main/
fox_llm_forward.inc consumes.

The model is a real llama-style transformer (RMSNorm + RoPE + attention +
SwiGLU FFN, tied embedding classifier) with full hand-written backprop. It is
trained on a compact fox-dialogue corpus that maps "[mood] <fact> ->" to a
short, cute, ON-FACT continuation.

Packs:
  --pack A   dim=64  L=4  ~260KB int8   ("Chatterbox" default brain)
  --pack B   dim=96  L=6  ~1MB int8     ("Critter" brain)

Even a lightly trained brain is safe: the firmware discards any continuation
that drops the fact, so the worst case is silence + deterministic templates.

Verified: pack A reaches CE < 0.3 in ~20 s on one core and greedily generates
lines like:
  [happy] it is sunny ->            ooh it is sunny.
  [sleepy] it is night ->           it is night...... *yawn*
  [excited] found your remote ->    found your remote! wag!
"""
import sys, os, struct, argparse, math, time
try:
    import numpy as np          # training only; --emit-policy works without it
except ImportError:
    np = None

MOODS = ["sleepy", "calm", "happy", "excited", "grumpy"]      # == enum FoxMood

# ============================================================================
#  FOXESE v2 — what the brain DECIDES.
#  Input  : "[mood] <act> <feel> t<turn> -> "     (the conversational situation)
#  Output : "S<style>G<gesture>E<intensity>N<next>\n"
#
#  The firmware owns every word. The brain decides HOW the fox says it (style,
#  gesture, intensity) and WHAT HAPPENS NEXT in the conversation (next move),
#  which the offline conversation engine executes. These tables are the ABI:
#  their order must match firmware (fox.h FoxAct / brain_policy.h).
# ============================================================================
ACTS = ["greet", "farewell", "comfort", "celebrate", "care", "answer", "joke",
        "story", "fact", "compliment", "affection", "playful", "sulk", "thanks",
        "apology", "agree", "decline", "curious", "ask", "offer", "report",
        "confirm", "unheard", "idle", "react", "say"]
FEELS = ["none", "sad", "happy", "tired", "hungry", "scared", "cold", "lonely"]
TURNS = 3                                   # t0 first exchange, t1, t2 = 2+
# styles   0 plain 1 bubbly 2 tender 3 silly 4 shy 5 dramatic 6 drowsy 7 sassy
# gestures 0 still 1 wag 2 perk 3 tilt 4 nuzzle 5 bounce 6 flop 7 squint
# next     0 none 1 ask 2 offer 3 fact 4 check-in
SIBLING = {0: 0, 1: 3, 2: 4, 3: 1, 4: 2, 5: 1, 6: 6, 7: 7}


def policy(mood, act, feel, turn, persona):
    """Primary decision for a situation. persona 'A' = Chatterbox (talks,
    asks), 'B' = Critter (sillier, offers play). Returns (S, G, E, N)."""
    m = MOODS[mood]; a = ACTS[act]; f = FEELS[feel]
    # ---- style --------------------------------------------------------------
    S = {"sleepy": 6, "calm": 0, "happy": 1, "excited": 1, "grumpy": 7}[m]
    if a == "comfort":
        S = 2
    elif a in ("affection", "compliment"):
        S = {"sleepy": 2, "calm": 4, "happy": 1, "excited": 1, "grumpy": 4}[m]
    elif a in ("joke", "playful", "react"):
        S = {"sleepy": 6, "grumpy": 7}.get(m, 3)
    elif a == "story":
        S = {"sleepy": 6, "grumpy": 7}.get(m, 5)
    elif a == "celebrate":
        S = {"excited": 5, "sleepy": 6, "grumpy": 4}.get(m, 1)
    elif a == "sulk":
        S = 6 if m == "sleepy" else 7
    elif a in ("care", "apology"):
        S = 4 if a == "apology" else (2 if m in ("calm", "happy", "sleepy") else S)
    elif a == "farewell":
        S = {"excited": 5, "grumpy": 7}.get(m, 2)
    elif a in ("confirm", "unheard", "curious"):
        S = {"sleepy": 6, "grumpy": 7, "excited": 1}.get(m, 0)
    if persona == "B" and S == 0 and a in ("greet", "idle", "react", "playful", "joke"):
        S = 3                                   # Critter: sillier by default
    # ---- gesture ------------------------------------------------------------
    G = {"greet": 1, "farewell": 1, "comfort": 4, "celebrate": 5, "care": 4,
         "answer": 2, "joke": 1, "story": 2, "fact": 2, "compliment": 1,
         "affection": 4, "playful": 1, "sulk": 7, "thanks": 1, "apology": 4,
         "agree": 1, "decline": 3, "curious": 3, "ask": 3, "offer": 2,
         "report": 2, "confirm": 3, "unheard": 3, "idle": 0, "react": 1,
         "say": 0}[a]
    if m == "excited" and G in (1, 0): G = 5
    if m == "sleepy" and a not in ("comfort", "affection"): G = 6
    if m == "grumpy" and a not in ("comfort", "apology"): G = 7
    if persona == "B" and G == 0: G = 1
    # ---- intensity ----------------------------------------------------------
    E = {"sleepy": 0, "calm": 1, "happy": 2, "excited": 3, "grumpy": 1}[m]
    if a in ("celebrate", "playful", "joke"): E = min(3, E + 1)
    if a in ("comfort", "care", "apology", "farewell", "confirm", "unheard"): E = min(E, 1)
    # ---- next move (the conversation steering) ------------------------------
    N = 0
    if a == "comfort":
        N = (2 if turn == 0 else 4) if f in ("sad", "lonely") else 4
        if turn >= 2: N = 0
    elif m in ("sleepy", "grumpy"):
        N = 0                                   # sleepy/grumpy foxes don't push
    elif turn >= 2:
        N = 2 if a == "unheard" else 0          # don't over-talk
    elif a == "greet":       N = 1 if turn == 0 else 0
    elif a == "celebrate":   N = 1 if turn == 0 else 2
    elif a == "answer":      N = 1 if turn == 0 else 0
    elif a == "care":        N = 2 if f == "tired" else 0
    elif a == "fact":        N = 3 if persona == "B" else 1
    elif a == "joke":        N = 2 if persona == "B" else 0
    elif a == "affection":   N = 0 if persona == "B" else (1 if turn == 0 else 0)
    elif a == "playful":     N = 2 if persona == "B" else 0
    elif a == "agree":       N = 1 if (persona == "A" and turn == 0) else 0
    elif a == "decline":     N = 2
    elif a == "unheard":     N = 1 if turn == 0 else 2
    elif a == "idle":        N = 2 if (persona == "B" and m in ("happy", "excited")) else 0
    return S, G, E, N


def situations():
    for mood in range(len(MOODS)):
        for act in range(len(ACTS)):
            for feel in range(len(FEELS)):
                for turn in range(TURNS):
                    yield mood, act, feel, turn


def prompt_of(mood, act, feel, turn):
    return f"[{MOODS[mood]}] {ACTS[act]} {FEELS[feel]} t{turn} -> "


def packet(S, G, E, N):
    return f"S{S}G{G}E{E}N{N}\n"


def build_lines(persona, rng, samples=4):
    """Each situation appears `samples` times. ~75% primary decision, ~25% a
    sibling style (and ask<->offer swap), so the model learns calibrated
    variety the firmware can sample from — not a single canned answer."""
    lines = []
    # Conversation-steering acts depend on feel x turn (finer distinctions,
    # fewer natural samples): oversample them so the next move is learned well.
    heavy = {ACTS.index(a) for a in ("comfort", "celebrate", "answer", "greet",
                                      "unheard", "care", "decline", "fact", "agree")}
    for sit in situations():
        S, G, E, N = policy(*sit, persona)
        reps = samples * (3 if sit[1] in heavy else 1)
        # the most important (and most context-dependent) moment: comforting a
        # sad/lonely person — first turn offers, later turns check in.
        if sit[1] == ACTS.index("comfort") and FEELS[sit[2]] in ("sad", "lonely"):
            reps *= 4
        for _ in range(reps):
            s, n = S, N
            if rng.random() < 0.25:
                s = SIBLING[S]
                if n in (1, 2): n = 3 - n
            lines.append((prompt_of(*sit), packet(s, G, E, n)))
    rng.shuffle(lines)
    return lines


def emit_policy_header(path):
    """The SAME policy as a C table for when no brain model is loaded, so the
    device behaves identically with or without the neural pack."""
    nm, na, nf = len(MOODS), len(ACTS), len(FEELS)
    vals = []
    for sit in situations():
        S, G, E, N = policy(*sit, "A")
        vals.append(S | (G << 3) | (E << 6) | (N << 8))
    with open(path, "w") as f:
        f.write("// AUTO-GENERATED by tools/train_brain.py --emit-policy. Do not edit.\n")
        f.write("// Fallback brain policy (persona A): index [mood][act][feel][turn],\n")
        f.write("// value = S | G<<3 | E<<6 | N<<8.\n#pragma once\n#include <stdint.h>\n")
        f.write(f"#define BRAIN_N_MOODS {nm}\n#define BRAIN_N_ACTS {na}\n")
        f.write(f"#define BRAIN_N_FEELS {nf}\n#define BRAIN_N_TURNS {TURNS}\n")
        f.write("static const uint16_t BRAIN_POLICY[] = {\n")
        for i in range(0, len(vals), 16):
            f.write("  " + ", ".join(str(v) for v in vals[i:i+16]) + ",\n")
        f.write("};\n")
        f.write("static const char* const BRAIN_ACT_NAMES[] = {" +
                ", ".join(f'"{a}"' for a in ACTS) + "};\n")
        f.write("static const char* const BRAIN_FEEL_NAMES[] = {" +
                ", ".join(f'"{x}"' for x in FEELS) + "};\n")
    return len(vals)


def build_vocab(text):
    used = sorted(set(text.encode("utf-8")))
    stoi = {b: i for i, b in enumerate(used)}
    itos = [bytes([b]) for b in used]
    return itos, stoi


class Brain:
    """llama-style transformer with manual backprop.
    Forward convention: y = x @ W.T  =>  dW = dy.T @ x , dx = dy @ W."""

    def __init__(self, dim, hidden, n_layers, n_heads, vocab, seq_len, gs, seed):
        self.dim, self.hidden, self.L = dim, hidden, n_layers
        self.H, self.hs = n_heads, dim // n_heads
        self.vocab, self.seq_len, self.gs = vocab, seq_len, gs
        self.n_kv_heads = n_heads
        rng = np.random.default_rng(seed); self.rng = rng
        r = lambda *s, sc=0.02: rng.standard_normal(s) * sc
        P = {"emb": r(vocab, dim, sc=0.05), "g_fin": np.ones(dim)}
        for l in range(n_layers):
            P[f"ga{l}"] = np.ones(dim); P[f"gf{l}"] = np.ones(dim)
            for nm in ("Wq", "Wk", "Wv", "Wo"):
                P[f"{nm}{l}"] = r(dim, dim, sc=0.08)
            P[f"W1{l}"] = r(hidden, dim, sc=0.08)
            P[f"W2{l}"] = r(dim, hidden, sc=0.08)
            P[f"W3{l}"] = r(hidden, dim, sc=0.08)
        self.P = P
        T, hsz = seq_len, self.hs
        self.COS = np.zeros((T, hsz)); self.SIN = np.zeros((T, hsz))
        for p in range(T):
            for i in range(0, hsz, 2):
                fr = 1.0 / (10000.0 ** (i / hsz)); v = p * fr
                self.COS[p, i] = self.COS[p, i+1] = math.cos(v)
                self.SIN[p, i] = self.SIN[p, i+1] = math.sin(v)

    def rope(self, x):
        Tn = x.shape[0]; c = self.COS[:Tn]; s = self.SIN[:Tn]; xr = x.copy()
        for i in range(0, self.hs, 2):
            x0 = x[:, :, i]; x1 = x[:, :, i+1]
            xr[:, :, i]   = x0 * c[:, i][:, None] - x1 * s[:, i][:, None]
            xr[:, :, i+1] = x0 * s[:, i][:, None] + x1 * c[:, i][:, None]
        return xr

    def rope_bwd(self, g):
        Tn = g.shape[0]; c = self.COS[:Tn]; s = self.SIN[:Tn]; gr = g.copy()
        for i in range(0, self.hs, 2):
            g0 = g[:, :, i]; g1 = g[:, :, i+1]
            gr[:, :, i]   =  g0 * c[:, i][:, None] + g1 * s[:, i][:, None]
            gr[:, :, i+1] = -g0 * s[:, i][:, None] + g1 * c[:, i][:, None]
        return gr

    @staticmethod
    def rmsnorm(x, g):
        ss = 1.0 / np.sqrt((x * x).mean(-1, keepdims=True) + 1e-5)
        return x * ss * g, ss

    @staticmethod
    def softmax(x, axis=-1):
        x = x - x.max(axis, keepdims=True); e = np.exp(x)
        return e / e.sum(axis, keepdims=True)

    @staticmethod
    def drms(dout, x, g, ss):
        n = x.shape[-1]
        dg = (dout * (x * ss)).sum(0)
        dxhat = dout * g
        xdot = (dxhat * x).sum(-1, keepdims=True)
        dx = ss * dxhat - (ss ** 3) * x * xdot / n
        return dx, dg

    def step(self, x, y, lr, lmask=None):
        P = self.P; L, H, hs, dim = self.L, self.H, self.hs, self.dim
        Tn = len(x); emb = P["emb"]; h = emb[x].copy(); caches = []
        sm = self.softmax
        mask = np.triu(np.ones((Tn, Tn)), 1).astype(bool)
        for l in range(L):
            xn, ssa = self.rmsnorm(h, P[f"ga{l}"])
            q = (xn @ P[f"Wq{l}"].T).reshape(Tn, H, hs)
            k = (xn @ P[f"Wk{l}"].T).reshape(Tn, H, hs)
            v = (xn @ P[f"Wv{l}"].T).reshape(Tn, H, hs)
            qr = self.rope(q); kr = self.rope(k)
            att = np.zeros((H, Tn, Tn)); out = np.zeros((Tn, H, hs))
            for hh in range(H):
                sc = (qr[:, hh, :] @ kr[:, hh, :].T) / math.sqrt(hs)
                sc[mask] = -1e9
                a = sm(sc, -1); att[hh] = a; out[:, hh, :] = a @ v[:, hh, :]
            outf = out.reshape(Tn, dim); ao = outf @ P[f"Wo{l}"].T; h1 = h + ao
            xn2, ssf = self.rmsnorm(h1, P[f"gf{l}"])
            a1 = xn2 @ P[f"W1{l}"].T; a3 = xn2 @ P[f"W3{l}"].T
            sig = 1 / (1 + np.exp(-a1)); silu = a1 * sig; gg = silu * a3
            ff = gg @ P[f"W2{l}"].T; h2 = h1 + ff
            caches.append((h, xn, ssa, qr, kr, v, att, outf, h1, xn2, ssf,
                           a1, a3, sig, silu, gg)); h = h2
        xf, ssfin = self.rmsnorm(h, P["g_fin"])
        logits = xf @ emb.T; p = sm(logits, -1)
        nll = -np.log(p[np.arange(Tn), y] + 1e-9)
        if lmask is None: lmask = np.ones(Tn)
        msum = max(1.0, lmask.sum())
        loss = (nll * lmask).sum() / msum
        dl = p.copy(); dl[np.arange(Tn), y] -= 1; dl *= (lmask / msum)[:, None]
        demb = dl.T @ xf; dxf = dl @ emb
        dh, dgf = self.drms(dxf, h, P["g_fin"], ssfin); P["g_fin"] -= lr * dgf
        for l in reversed(range(L)):
            (h_in, xn, ssa, qr, kr, v, att, outf, h1, xn2, ssf,
             a1, a3, sig, silu, gg) = caches[l]
            dff = dh.copy(); dgg = dff @ P[f"W2{l}"]; dW2 = dff.T @ gg
            dsilu = dgg * a3; da3 = dgg * silu
            dsig = dsilu * a1; da1 = dsilu * sig + dsig * sig * (1 - sig)
            dW1 = da1.T @ xn2; dW3 = da3.T @ xn2
            dxn2 = da1 @ P[f"W1{l}"] + da3 @ P[f"W3{l}"]
            dh1b, dgffn = self.drms(dxn2, h1, P[f"gf{l}"], ssf)
            P[f"gf{l}"] -= lr * dgffn
            dh1 = dh + dh1b; dao = dh1.copy(); dh_in = dh1.copy()
            dWo = dao.T @ outf; doutf = dao @ P[f"Wo{l}"]
            dout_ = doutf.reshape(Tn, H, hs)
            dqr = np.zeros_like(qr); dkr = np.zeros_like(kr); dv = np.zeros_like(v)
            for hh in range(H):
                a = att[hh]
                dv[:, hh, :] += a.T @ dout_[:, hh, :]
                da = dout_[:, hh, :] @ v[:, hh, :].T
                ds = a * (da - (da * a).sum(-1, keepdims=True)); ds /= math.sqrt(hs)
                dqr[:, hh, :] += ds @ kr[:, hh, :]
                dkr[:, hh, :] += ds.T @ qr[:, hh, :]
            dq = self.rope_bwd(dqr); dk = self.rope_bwd(dkr)
            dqf = dq.reshape(Tn, dim); dkf = dk.reshape(Tn, dim); dvf = dv.reshape(Tn, dim)
            dWq = dqf.T @ xn; dWk = dkf.T @ xn; dWv = dvf.T @ xn
            dxn = dqf @ P[f"Wq{l}"] + dkf @ P[f"Wk{l}"] + dvf @ P[f"Wv{l}"]
            dh_inb, dgatt = self.drms(dxn, h_in, P[f"ga{l}"], ssa)
            P[f"ga{l}"] -= lr * dgatt; dh_in = dh_in + dh_inb
            for nm, gr in (("Wq", dWq), ("Wk", dWk), ("Wv", dWv), ("Wo", dWo),
                           ("W1", dW1), ("W2", dW2), ("W3", dW3)):
                P[f"{nm}{l}"] -= lr * gr
            for t in range(Tn):
                demb[x[t]] += dh_in[t]
            dh = dh_in
        P["emb"] -= lr * demb
        return loss

    def generate(self, prompt, stoi, itos, n=30):
        P = self.P; L, H, hs, dim = self.L, self.H, self.hs, self.dim
        ids = [stoi[b] for b in prompt.encode() if b in stoi]
        seq = list(ids); emb = P["emb"]; sm = self.softmax
        for _ in range(n):
            x = np.array(seq[-self.seq_len:]); Tn = len(x); h = emb[x].copy()
            mask = np.triu(np.ones((Tn, Tn)), 1).astype(bool)
            for l in range(L):
                xn, _ = self.rmsnorm(h, P[f"ga{l}"])
                q = (xn @ P[f"Wq{l}"].T).reshape(Tn, H, hs)
                k = (xn @ P[f"Wk{l}"].T).reshape(Tn, H, hs)
                v = (xn @ P[f"Wv{l}"].T).reshape(Tn, H, hs)
                qr = self.rope(q); kr = self.rope(k); out = np.zeros((Tn, H, hs))
                for hh in range(H):
                    sc = (qr[:, hh, :] @ kr[:, hh, :].T) / math.sqrt(hs)
                    sc[mask] = -1e9; a = sm(sc, -1); out[:, hh, :] = a @ v[:, hh, :]
                ao = out.reshape(Tn, dim) @ P[f"Wo{l}"].T; h1 = h + ao
                xn2, _ = self.rmsnorm(h1, P[f"gf{l}"])
                a1 = xn2 @ P[f"W1{l}"].T; a3 = xn2 @ P[f"W3{l}"].T
                silu = a1 * (1 / (1 + np.exp(-a1))); h = h1 + (silu * a3) @ P[f"W2{l}"].T
            xf, _ = self.rmsnorm(h, P["g_fin"]); logits = xf[-1] @ emb.T
            nxt = int(np.argmax(logits)); seq.append(nxt)
            if itos[nxt] == b"\n": break
        return b"".join(itos[s] for s in seq[len(ids):]).decode("utf-8", "replace")


def quantize_q8(mat, gs):
    rows, cols = mat.shape
    assert cols % gs == 0, f"cols {cols} not divisible by group {gs}"
    q = np.empty((rows, cols), np.int8)
    s = np.empty((rows, cols // gs), np.float32)
    for r in range(rows):
        row = mat[r]
        for g in range(0, cols, gs):
            grp = row[g:g+gs]; amax = np.abs(grp).max()
            scale = amax / 127.0 if amax > 0 else 1.0
            s[r, g // gs] = scale
            q[r, g:g+gs] = np.clip(np.round(grp / scale), -127, 127).astype(np.int8)
    return q, s


def write_foxb(path, b, itos):
    P = b.P
    with open(path, "wb") as f:
        f.write(b"FOXB")
        f.write(struct.pack("<I", 1))
        f.write(struct.pack("<iiiiiiii", b.dim, b.hidden, b.L, b.H,
                            b.n_kv_heads, b.vocab, b.seq_len, 1))  # shared_cls=1
        f.write(struct.pack("<i", b.gs))
        f.write(struct.pack("<6i", 0, 0, 0, 0, 0, 0))
        f.write(P["emb"].astype("<f4").tobytes())
        f.write(np.stack([P[f"ga{l}"] for l in range(b.L)]).astype("<f4").tobytes())
        f.write(np.stack([P[f"gf{l}"] for l in range(b.L)]).astype("<f4").tobytes())
        f.write(P["g_fin"].astype("<f4").tobytes())
        for nm in ("Wq", "Wk", "Wv", "Wo", "W1", "W2", "W3"):
            for l in range(b.L):
                q, s = quantize_q8(P[f"{nm}{l}"], b.gs)
                f.write(q.tobytes()); f.write(s.astype("<f4").tobytes())
        for tok in itos:
            f.write(struct.pack("<f", 0.0))
            f.write(struct.pack("<H", len(tok)))
            f.write(tok)
    return os.path.getsize(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", choices=["A", "B"], default="A")
    ap.add_argument("--out")
    ap.add_argument("--steps", type=int, default=9000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--emit-policy", metavar="HEADER",
                    help="write the fallback policy C header and exit")
    args = ap.parse_args()

    if args.emit_policy:
        n = emit_policy_header(args.emit_policy)
        print(f"train_brain: wrote policy header {args.emit_policy} ({n} situations)")
        return
    if not args.out:
        ap.error("--out is required")

    rng = np.random.default_rng(args.seed)
    lines = build_lines(args.pack, rng)
    text = "".join(p + k for p, k in lines)
    itos, stoi = build_vocab(text)
    vocab = len(itos)
    enc = lambda s: [stoi[c] for c in s.encode()]
    data = [(enc(p), enc(k)) for p, k in lines]

    if args.pack == "A":
        b = Brain(dim=64, hidden=128, n_layers=4, n_heads=4, vocab=vocab,
                  seq_len=64, gs=16, seed=args.seed)
    else:
        b = Brain(dim=80, hidden=160, n_layers=3, n_heads=5, vocab=vocab,
                  seq_len=96, gs=16, seed=args.seed)
    nparam = sum(v.size for _, v in b.P.items())
    print(f"train_brain v2: pack {args.pack} ({'Chatterbox' if args.pack == 'A' else 'Critter'})  "
          f"vocab {vocab}  ~{nparam//1000}K params  training lines {len(lines)}")

    # Line-aligned samples; loss ONLY on the decision bytes (the packet), so
    # all capacity goes to learning the policy instead of memorising prompts.
    t0 = time.time(); ema = None
    for step in range(args.steps):
        p_ids, k_ids = data[rng.integers(0, len(data))]
        seq = p_ids + k_ids
        x = np.array(seq[:-1]); y = np.array(seq[1:])
        mask = np.zeros(len(y)); mask[len(p_ids) - 1:] = 1.0
        lr = 0.03 if step < args.steps * 0.6 else (0.012 if step < args.steps * 0.85 else 0.004)
        loss = b.step(x, y, lr, mask)
        ema = loss if ema is None else 0.98 * ema + 0.02 * loss
        if step % 1000 == 0:
            print(f"  step {step:5d}  ce {ema:.3f}  ({time.time()-t0:.1f}s)")

    # Evaluate: how often does the greedy decision match the policy?
    hits = {"S": 0, "G": 0, "E": 0, "N": 0}; tot = 0
    for sit in list(situations())[::7]:
        want = packet(*policy(*sit, args.pack)).strip()
        got = b.generate(prompt_of(*sit), stoi, itos, n=9).strip()
        tot += 1
        for i, kname in enumerate("SGEN"):
            if len(got) > 2 * i + 1 and len(want) > 2 * i + 1 and got[2*i+1] == want[2*i+1]:
                hits[kname] += 1
    acc = {k: v / tot for k, v in hits.items()}
    print("  policy agreement (greedy): " + "  ".join(f"{k}={acc[k]:.0%}" for k in "SGEN"))
    for sit in [(2, ACTS.index("comfort"), FEELS.index("sad"), 0),
                (2, ACTS.index("comfort"), FEELS.index("sad"), 1),
                (3, ACTS.index("celebrate"), 2, 0), (0, ACTS.index("joke"), 0, 0),
                (4, ACTS.index("greet"), 0, 0), (1, ACTS.index("unheard"), 0, 1)]:
        pr = prompt_of(*sit)
        print(f"    {pr:36} => {b.generate(pr, stoi, itos, n=9).strip()!r:14} policy {packet(*policy(*sit, args.pack)).strip()}")

    size = write_foxb(args.out, b, itos)
    print(f"train_brain: wrote {args.out}  {size} bytes")
    if min(acc.values()) < 0.6:
        print("train_brain: WARNING policy agreement is low; firmware will fall back to the table")


if __name__ == "__main__":
    main()
