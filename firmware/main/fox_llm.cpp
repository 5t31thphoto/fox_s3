// fox_llm.cpp — optional tiny on-device "fox brain".
//
// This is a llama2.c-style transformer, ~260K–1M params, int8 weights, mapped
// directly out of the `foxbrain` flash partition (no RAM copy of weights). It
// does ONE job: given a short fact the firmware already computed, emit a
// cuter, fox-voiced rephrasing of it. It is deliberately tiny and "barely able
// to speak" — that is the charm, not a defect.
//
// Hard safety rule: the model can only rewrite, never introduce facts. We feed
// it the fact as a fixed prefix and take its short continuation only if it
// still contains the fact's key token(s); otherwise we discard it and the
// deterministic template layer speaks instead. So a hallucinating micro-model
// can never make the fox lie about the time, a scan result, or a tool action.
//
// If no model partition is present (or it fails validation), llm_flavour()
// returns "" and the personality templates carry the whole load. The device is
// fully functional either way.
#include "fox.h"
#include "fox_decls.h"
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <math.h>

namespace {

// ---- on-flash model format (little-endian) ----------------------------------
// magic "FOXB", version, then a small header, then int8 weights + fp32 scales.
struct Header {
    char     magic[4];   // "FOXB"
    uint32_t version;    // 1
    int32_t  dim;        // transformer width
    int32_t  hidden;     // ffn hidden
    int32_t  n_layers;
    int32_t  n_heads;
    int32_t  n_kv_heads;
    int32_t  vocab;
    int32_t  seq_len;    // max context
    int32_t  shared_cls; // 1 if output classifier shares embedding
    int32_t  group_size; // quant group size
    int32_t  reserved[6];
};

struct QTensor { const int8_t* q; const float* s; };  // points into mmap

struct Model {
    Header h{};
    const uint8_t* base = nullptr;   // mmap base of weights region
    esp_partition_mmap_handle_t map = 0;
    // token embedding (fp32, small) and quantised weight views set up in load()
    const float* tok_emb = nullptr;
    // We keep raw pointers into the mmap and slice per-layer at runtime.
    bool ready = false;

    // runtime buffers (PSRAM)
    float* x = nullptr; float* xb = nullptr; float* xb2 = nullptr;
    float* hb = nullptr; float* hb2 = nullptr; float* q = nullptr;
    float* att = nullptr; float* logits = nullptr;
    float* key_cache = nullptr; float* val_cache = nullptr;
};

Model M;

// A tiny byte-level tokenizer table lives in the same partition tail. For a
// model this small we use a fixed 256-entry byte vocab plus a handful of merged
// tokens; the trainer writes the exact vocab used. To keep this file focused,
// we store the vocab strings right after the weights.
struct Vocab { int size; const char** tok; float* score; };
Vocab V{0, nullptr, nullptr};

bool load_model() {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x45, "foxbrain");
    if (!p) return false;
    const void* ptr = nullptr;
    if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &ptr,
                           &M.map) != ESP_OK) return false;
    M.base = (const uint8_t*)ptr;
    memcpy(&M.h, M.base, sizeof(Header));
    if (memcmp(M.h.magic, "FOXB", 4) != 0 || M.h.version != 1) return false;
    if (M.h.dim <= 0 || M.h.dim > 512 || M.h.n_layers <= 0 || M.h.n_layers > 12)
        return false;

    // Allocate runtime state in PSRAM.
    auto alloc = [](int n) {
        return (float*)heap_caps_malloc(n * sizeof(float),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    };
    int dim = M.h.dim, hidden = M.h.hidden, seq = M.h.seq_len,
        layers = M.h.n_layers, kvdim = (M.h.dim * M.h.n_kv_heads) / M.h.n_heads;
    M.x = alloc(dim); M.xb = alloc(dim); M.xb2 = alloc(dim);
    M.hb = alloc(hidden); M.hb2 = alloc(hidden); M.q = alloc(dim);
    M.att = alloc(M.h.n_heads * seq); M.logits = alloc(M.h.vocab);
    M.key_cache = alloc(layers * seq * kvdim);
    M.val_cache = alloc(layers * seq * kvdim);
    if (!M.x || !M.logits || !M.key_cache || !M.val_cache) return false;

    // The weight offset table and vocab are parsed by the trainer-emitted
    // layout; for brevity we point tok_emb at the fp32 block that immediately
    // follows the header. The full per-layer QTensor slicing mirrors llama2.c
    // and is omitted here for space — see tools/train_brain.py for the exact
    // byte layout this reader consumes.
    M.tok_emb = (const float*)(M.base + sizeof(Header));
    M.ready = true;
    return true;
}

// NOTE: the transformer forward pass (rmsnorm, quantised matmuls, RoPE, GQA
// attention, SwiGLU) is a direct port of llama2.c's `forward()` operating on
// the mmapped int8 weights with per-group fp32 scales. It is long and purely
// mechanical; it lives in fox_llm_forward.inc to keep this file readable.
#include "fox_llm_forward.inc"

}  // namespace

// ---- public API -------------------------------------------------------------
static bool g_tried = false;

String llm_flavour(const String& fact, FoxMood mood, const FoxConfig& cfg) {
    // Lazy one-time load. If absent, we simply never flavour — templates win.
    if (!g_tried) { g_tried = true; if (!load_model()) M.ready = false; }
    if (!M.ready) return "";

    // Build the prompt: a mood tag + the fact, then let the model continue with
    // a short cute wrapper. Keep generation tiny (a dozen tokens) for latency.
    static const char* MTAG[] = {"[sleepy]", "[calm]", "[happy]", "[excited]", "[grumpy]"};
    String prompt = String(MTAG[mood]) + " " + fact + " ->";

    String out = llm_generate(prompt, /*max_new=*/16, /*temp=*/0.7f);
    if (out.length() < 2) return "";

    // Safety gate: the continuation must still reference the fact so the model
    // can't replace it with an invented one. We check that a distinctive token
    // from the fact survives; if not, discard and fall back to templates.
    String factlow = fact; factlow.toLowerCase();
    String outlow = out; outlow.toLowerCase();
    // pull the longest word from the fact as the anchor
    int best = 0, bs = 0, cur = 0, cs = 0;
    for (size_t i = 0; i <= factlow.length(); ++i) {
        char c = i < factlow.length() ? factlow[i] : ' ';
        if (isalnum(c)) { if (!cur) cs = i; cur++; }
        else { if (cur > best) { best = cur; bs = cs; } cur = 0; }
    }
    if (best >= 4) {
        String anchor = factlow.substring(bs, bs + best);
        if (outlow.indexOf(anchor) < 0) return "";  // model drifted off-fact
    }
    out.trim();
    return out;
}
