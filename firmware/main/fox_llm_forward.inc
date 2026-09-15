// fox_llm_forward.inc — transformer forward pass + sampler for the tiny fox
// brain. Included inside the anonymous namespace of fox_llm.cpp.
//
// This is a compact, faithful port of Andrej Karpathy's llama2.c inference,
// specialised to Q8_0-style group quantisation and reading weights straight
// from the mmapped `foxbrain` partition. The exact byte layout it consumes is
// produced by tools/train_brain.py (see WEIGHT LAYOUT there and mirrored here).
//
// WEIGHT LAYOUT (after Header), all little-endian:
//   fp32 tok_emb[vocab * dim]                         (embedding, also classifier if shared)
//   fp32 rms_att[n_layers * dim]
//   fp32 rms_ffn[n_layers * dim]
//   fp32 rms_final[dim]
//   Q8   wq[n_layers]  each (dim x dim)
//   Q8   wk[n_layers]  each (dim x kvdim)
//   Q8   wv[n_layers]  each (dim x kvdim)
//   Q8   wo[n_layers]  each (dim x dim)
//   Q8   w1[n_layers]  each (hidden x dim)
//   Q8   w2[n_layers]  each (dim x hidden)
//   Q8   w3[n_layers]  each (hidden x dim)
//   Q8   wcls          (vocab x dim)  [present only if shared_cls == 0]
//   vocab blob: for i in vocab: fp32 score, u16 len, bytes[len]
//
// A Q8 matrix of shape (rows x cols) is stored as:
//   int8 q[rows * cols]     row-major
//   fp32 s[rows * cols/gs]  one scale per group of gs along the row
//
// Everything is pointer arithmetic into the mmap; no weight copies to RAM.

static inline float* rt_x() { return M.x; }

// cursor helper to walk the mmap sequentially during load-time slicing
struct Cursor {
    const uint8_t* p;
    explicit Cursor(const uint8_t* base) : p(base) {}
    const float* f32(size_t n) { const float* r = (const float*)p; p += n * 4; return r; }
    QTensor q8(size_t rows, size_t cols, int gs) {
        QTensor t;
        t.q = (const int8_t*)p; p += rows * cols;
        t.s = (const float*)p;  p += (rows * cols / gs) * 4;
        return t;
    }
};

// Resolved weight views (filled on first forward()).
struct Weights {
    const float* tok_emb;
    const float* rms_att;   // [layers*dim]
    const float* rms_ffn;   // [layers*dim]
    const float* rms_final; // [dim]
    QTensor* wq; QTensor* wk; QTensor* wv; QTensor* wo;
    QTensor* w1; QTensor* w2; QTensor* w3;
    QTensor  wcls;
    bool resolved = false;
} W;

static void resolve_weights() {
    if (W.resolved) return;
    const Header& h = M.h;
    int dim = h.dim, hidden = h.hidden, L = h.n_layers, gs = h.group_size;
    int kvdim = (dim * h.n_kv_heads) / h.n_heads;
    Cursor c(M.base + sizeof(Header));
    W.tok_emb   = c.f32((size_t)h.vocab * dim);
    W.rms_att   = c.f32((size_t)L * dim);
    W.rms_ffn   = c.f32((size_t)L * dim);
    W.rms_final = c.f32(dim);
    auto arr = [&](int n) { return (QTensor*)heap_caps_malloc(n * sizeof(QTensor),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); };
    W.wq = arr(L); W.wk = arr(L); W.wv = arr(L); W.wo = arr(L);
    W.w1 = arr(L); W.w2 = arr(L); W.w3 = arr(L);
    for (int l = 0; l < L; ++l) W.wq[l] = c.q8(dim, dim, gs);
    for (int l = 0; l < L; ++l) W.wk[l] = c.q8(dim, kvdim, gs);
    for (int l = 0; l < L; ++l) W.wv[l] = c.q8(dim, kvdim, gs);
    for (int l = 0; l < L; ++l) W.wo[l] = c.q8(dim, dim, gs);
    for (int l = 0; l < L; ++l) W.w1[l] = c.q8(hidden, dim, gs);
    for (int l = 0; l < L; ++l) W.w2[l] = c.q8(dim, hidden, gs);
    for (int l = 0; l < L; ++l) W.w3[l] = c.q8(hidden, dim, gs);
    if (!h.shared_cls) W.wcls = c.q8(h.vocab, dim, gs);
    else { W.wcls.q = nullptr; W.wcls.s = nullptr; }
    // vocab blob follows; parse into V
    const uint8_t* vp = c.p;
    static const char* toks[512];
    static float scores[512];
    int vs = h.vocab > 512 ? 512 : h.vocab;
    for (int i = 0; i < vs; ++i) {
        scores[i] = *(const float*)vp; vp += 4;
        uint16_t len = *(const uint16_t*)vp; vp += 2;
        // store as heap C-strings (tiny)
        char* s = (char*)malloc(len + 1);
        memcpy(s, vp, len); s[len] = 0; vp += len;
        toks[i] = s;
    }
    V.size = vs; V.tok = toks; V.score = scores;
    W.resolved = true;
}

// ---- math kernels -----------------------------------------------------------
static void rmsnorm(float* o, const float* x, const float* w, int n) {
    float ss = 0.f;
    for (int i = 0; i < n; ++i) ss += x[i] * x[i];
    ss = 1.0f / sqrtf(ss / n + 1e-5f);
    for (int i = 0; i < n; ++i) o[i] = w[i] * (ss * x[i]);
}

static void softmax(float* x, int n) {
    float m = x[0];
    for (int i = 1; i < n; ++i) if (x[i] > m) m = x[i];
    float s = 0.f;
    for (int i = 0; i < n; ++i) { x[i] = expf(x[i] - m); s += x[i]; }
    for (int i = 0; i < n; ++i) x[i] /= s;
}

// out[rows] = W(rows x cols, Q8 grouped) * x[cols]  (x is fp32)
static void qmatmul(float* out, const float* x, const QTensor& w,
                    int rows, int cols, int gs) {
    for (int r = 0; r < rows; ++r) {
        float acc = 0.f;
        const int8_t* wq = w.q + (size_t)r * cols;
        const float* ws = w.s + (size_t)r * (cols / gs);
        for (int g = 0; g < cols; g += gs) {
            float scale = ws[g / gs];
            float part = 0.f;
            for (int j = 0; j < gs; ++j) part += (float)wq[g + j] * x[g + j];
            acc += part * scale;
        }
        out[r] = acc;
    }
}

static const float* forward(int token, int pos) {
    resolve_weights();
    const Header& h = M.h;
    int dim = h.dim, hidden = h.hidden, gs = h.group_size;
    int n_heads = h.n_heads, head_size = dim / n_heads;
    int kv_heads = h.n_kv_heads, kvdim = (dim * kv_heads) / n_heads;
    int kv_mul = n_heads / kv_heads;

    memcpy(M.x, W.tok_emb + (size_t)token * dim, dim * sizeof(float));

    for (int l = 0; l < h.n_layers; ++l) {
        rmsnorm(M.xb, M.x, W.rms_att + (size_t)l * dim, dim);

        float* krow = M.key_cache + ((size_t)l * h.seq_len + pos) * kvdim;
        float* vrow = M.val_cache + ((size_t)l * h.seq_len + pos) * kvdim;
        qmatmul(M.q,  M.xb, W.wq[l], dim,   dim, gs);
        qmatmul(krow, M.xb, W.wk[l], kvdim, dim, gs);
        qmatmul(vrow, M.xb, W.wv[l], kvdim, dim, gs);

        // RoPE on q and k
        for (int i = 0; i < dim; i += 2) {
            int hd = i % head_size;
            float freq = 1.0f / powf(10000.0f, (float)hd / head_size);
            float val = pos * freq, fcr = cosf(val), fci = sinf(val);
            float q0 = M.q[i], q1 = M.q[i + 1];
            M.q[i] = q0 * fcr - q1 * fci;
            M.q[i + 1] = q0 * fci + q1 * fcr;
            if (i < kvdim) {
                float k0 = krow[i], k1 = krow[i + 1];
                krow[i] = k0 * fcr - k1 * fci;
                krow[i + 1] = k0 * fci + k1 * fcr;
            }
        }

        // attention per head
        for (int hh = 0; hh < n_heads; ++hh) {
            const float* qh = M.q + hh * head_size;
            float* atth = M.att + hh * h.seq_len;
            int kvh = hh / kv_mul;
            for (int t = 0; t <= pos; ++t) {
                const float* kh = M.key_cache +
                    ((size_t)l * h.seq_len + t) * kvdim + kvh * head_size;
                float sc = 0.f;
                for (int i = 0; i < head_size; ++i) sc += qh[i] * kh[i];
                atth[t] = sc / sqrtf((float)head_size);
            }
            softmax(atth, pos + 1);
            float* xbh = M.xb + hh * head_size;
            for (int i = 0; i < head_size; ++i) xbh[i] = 0.f;
            for (int t = 0; t <= pos; ++t) {
                const float* vh = M.val_cache +
                    ((size_t)l * h.seq_len + t) * kvdim + kvh * head_size;
                float a = atth[t];
                for (int i = 0; i < head_size; ++i) xbh[i] += a * vh[i];
            }
        }

        qmatmul(M.xb2, M.xb, W.wo[l], dim, dim, gs);
        for (int i = 0; i < dim; ++i) M.x[i] += M.xb2[i];

        // FFN (SwiGLU)
        rmsnorm(M.xb, M.x, W.rms_ffn + (size_t)l * dim, dim);
        qmatmul(M.hb,  M.xb, W.w1[l], hidden, dim, gs);
        qmatmul(M.hb2, M.xb, W.w3[l], hidden, dim, gs);
        for (int i = 0; i < hidden; ++i) {
            float v = M.hb[i];
            v *= (1.0f / (1.0f + expf(-v)));   // SiLU
            M.hb[i] = v * M.hb2[i];
        }
        qmatmul(M.xb, M.hb, W.w2[l], dim, hidden, gs);
        for (int i = 0; i < dim; ++i) M.x[i] += M.xb[i];
    }

    rmsnorm(M.x, M.x, W.rms_final, dim);
    if (h.shared_cls) {
        // logits = tok_emb (as fp32) dot x — treat embedding as classifier
        for (int r = 0; r < h.vocab; ++r) {
            const float* row = W.tok_emb + (size_t)r * dim;
            float acc = 0.f;
            for (int i = 0; i < dim; ++i) acc += row[i] * M.x[i];
            M.logits[r] = acc;
        }
    } else {
        qmatmul(M.logits, M.x, W.wcls, h.vocab, dim, gs);
    }
    return M.logits;
}

// ---- byte-level tokenizer (greedy longest-match over vocab) ------------------
static int str_lookup(const char* s, int len) {
    for (int i = 0; i < V.size; ++i) {
        if ((int)strlen(V.tok[i]) == len && memcmp(V.tok[i], s, len) == 0)
            return i;
    }
    return -1;
}

static int encode(const String& text, int* toks, int max) {
    int n = 0;
    size_t i = 0;
    while (i < text.length() && n < max) {
        // try longest match up to 6 bytes
        int best = -1, blen = 0;
        for (int l = 6; l >= 1; --l) {
            if (i + l > text.length()) continue;
            int id = str_lookup(text.c_str() + i, l);
            if (id >= 0) { best = id; blen = l; break; }
        }
        if (best < 0) {  // fall back to raw byte token if present
            best = str_lookup(text.c_str() + i, 1);
            blen = 1;
        }
        if (best < 0) { i++; continue; }
        toks[n++] = best; i += blen;
    }
    return n;
}

static int sample_argmax(const float* logits, int n) {
    int best = 0; float bv = logits[0];
    for (int i = 1; i < n; ++i) if (logits[i] > bv) { bv = logits[i]; best = i; }
    return best;
}

static int sample_temp(float* logits, int n, float temp) {
    if (temp <= 0.f) return sample_argmax(logits, n);
    for (int i = 0; i < n; ++i) logits[i] /= temp;
    softmax(logits, n);
    float r = (float)(esp_random() & 0xFFFFFF) / (float)0x1000000;
    float c = 0.f;
    for (int i = 0; i < n; ++i) { c += logits[i]; if (r < c) return i; }
    return n - 1;
}

// Generate a short continuation of `prompt`. Stops on newline token or budget.
static String llm_generate(const String& prompt, int max_new, float temp) {
    if (!M.ready) return "";
    resolve_weights();
    int cap = M.h.seq_len;
    int* toks = (int*)malloc(sizeof(int) * cap);
    if (!toks) return "";
    int n = encode(prompt, toks, cap);
    if (n == 0) { free(toks); return ""; }

    // prime the model with the prompt
    int pos = 0, token = toks[0];
    for (; pos < n - 1 && pos < cap - 1; ++pos) {
        forward(token, pos);
        token = toks[pos + 1];
    }

    String out;
    int steps = 0;
    while (steps < max_new && pos < cap - 1) {
        const float* logits = forward(token, pos);
        // copy logits (sampler mutates) — reuse M.att as scratch is unsafe; alloc small
        int next = sample_temp((float*)logits, M.h.vocab, temp);
        pos++;
        token = next;
        const char* piece = (next >= 0 && next < V.size) ? V.tok[next] : "";
        if (piece && piece[0] == '\n') break;
        out += piece;
        steps++;
        if (out.length() > 80) break;
    }
    free(toks);
    out.trim();
    return out;
}
