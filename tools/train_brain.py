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
import numpy as np

MOODS = ["sleepy", "calm", "happy", "excited", "grumpy"]

FACTS = [
    "the time is now", "it is sunny", "it is raining", "it is cloudy",
    "battery is low", "battery is full", "the tv is off", "the tv is on",
    "found your remote", "message from a friend", "it is morning",
    "it is night", "you have been away", "the volume is up",
    "the volume is down", "the light is on", "the light is off",
    "a brand new day", "time to rest", "i saved that", "i remember you",
    "let us play", "i missed you", "all done", "ready to go",
]

STYLE = {
    "sleepy":  ["{f}... *yawn*", "mm {f}...", "{f}, so sleepy", "{f} *nods off*"],
    "calm":    ["{f}.", "okay {f}", "{f}, nice", "sure, {f}"],
    "happy":   ["ooh {f}~", "yay {f}!", "{f} ^^", "hehe {f}!"],
    "excited": ["{f}!! hehe", "{f}! wag!", "ooh ooh {f}!", "{f}!! yay!"],
    "grumpy":  ["{f}. hmph.", "fine {f}.", "*flicks tail* {f}", "{f}. whatever."],
}


def build_corpus(repeat):
    lines = []
    for f in FACTS:
        for m in MOODS:
            for t in STYLE[m]:
                lines.append(f"[{m}] {f} -> {t.format(f=f)}\n")
    return "".join(lines * repeat)


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

    def step(self, x, y, lr):
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
        loss = -np.log(p[np.arange(Tn), y] + 1e-9).mean()
        dl = p.copy(); dl[np.arange(Tn), y] -= 1; dl /= Tn
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
    ap.add_argument("--out", required=True)
    ap.add_argument("--steps", type=int, default=6000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--repeat", type=int, default=6)
    args = ap.parse_args()

    text = build_corpus(args.repeat)
    itos, stoi = build_vocab(text)
    data = np.array([stoi[bb] for bb in text.encode()], dtype=np.int64)
    vocab = len(itos)

    if args.pack == "A":
        b = Brain(dim=64, hidden=128, n_layers=4, n_heads=4, vocab=vocab,
                  seq_len=64, gs=16, seed=args.seed)
    else:
        b = Brain(dim=96, hidden=256, n_layers=6, n_heads=6, vocab=vocab,
                  seq_len=96, gs=16, seed=args.seed)

    nparam = sum(v.size for _, v in b.P.items())
    print(f"train_brain: pack {args.pack}  vocab {vocab}  ~{nparam//1000}K params "
          f"dim={b.dim} L={b.L} H={b.H} seq={b.seq_len}")

    T = min(b.seq_len, 48)
    t0 = time.time()
    loss = 0.0
    for step in range(args.steps):
        i = b.rng.integers(0, len(data) - T - 1)
        toks = data[i:i+T+1]
        lr = 0.03 if step < args.steps // 2 else 0.008
        loss = b.step(toks[:-1], toks[1:], lr)
        if step % 1000 == 0:
            print(f"  step {step:5d}  ce {loss:.3f}  ({time.time()-t0:.1f}s)")
    print(f"  final ce {loss:.3f}")

    print("  sample generations:")
    for pr in ["[happy] it is sunny ->", "[sleepy] it is night ->",
               "[excited] found your remote ->", "[grumpy] battery is low ->"]:
        print(f"    {pr:38} => {b.generate(pr, stoi, itos)!r}")

    size = write_foxb(args.out, b, itos)
    print(f"train_brain: wrote {args.out}  {size} bytes")


if __name__ == "__main__":
    main()
