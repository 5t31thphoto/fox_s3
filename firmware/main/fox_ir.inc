// fox_ir.inc — IR blaster: TV-B-Gone-style sweep + "fake learning".
//
// Hardware is transmit-only (AtomS3R has an IR LED on GPIO47 but no receiver),
// so the fox cannot learn a code by capture. Instead it *narrows*: it plays
// batches of power codes from the compiled library and lets the human confirm
// which batch worked ("that one!"), binary-searching down to a single code it
// then remembers as this user's TV. That turns a transmit-only blaster into
// something that feels like it learned your remote.
//
// Codes come from the `foxdata` partition (built by tools/build_ir.py, format
// "FOXI" v2). We mmap it read-only and play raw timing pairs over RMT.
#pragma once
#include <esp_partition.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>

// ---- FOXI blob layout (mirror of tools/build_ir.py) -------------------------
struct __attribute__((packed)) IrEntry {
    uint32_t name_off;   // absolute
    uint8_t  category;   // 0 tv 1 audio 2 projector 3 ac 4 misc
    uint8_t  button;     // 0 power 1 vol_up 2 vol_dn 3 mute ...
    uint8_t  freq_div;   // freq = freq_div * 250
    uint8_t  flags;      // bit0 = power-sweep pool
    uint32_t data_off;   // absolute, into u16 timing blob
    uint16_t n_pairs;
    uint16_t _pad;
};

static const uint8_t* ir_base = nullptr;
static esp_partition_mmap_handle_t ir_map = 0;
static uint16_t ir_n_codes = 0;
static const IrEntry* ir_table = nullptr;
static rmt_channel_handle_t ir_chan = nullptr;
static rmt_encoder_handle_t ir_encoder = nullptr;

void ir_begin() {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "foxdata");
    if (p) {
        const void* ptr = nullptr;
        if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &ptr, &ir_map) == ESP_OK) {
            ir_base = (const uint8_t*)ptr;
            if (memcmp(ir_base, "FOXI", 4) == 0) {
                ir_n_codes = *(const uint16_t*)(ir_base + 6);
                ir_table = (const IrEntry*)(ir_base + 16);
            }
        }
    }

    // RMT TX channel on the IR LED. Carrier is applied per-code below.
    rmt_tx_channel_config_t tx = {};
    tx.gpio_num = IR_TX_GPIO;
    tx.clk_src = RMT_CLK_SRC_DEFAULT;
    tx.resolution_hz = 1000000;      // 1us tick
    tx.mem_block_symbols = 128;
    tx.trans_queue_depth = 4;
    if (rmt_new_tx_channel(&tx, &ir_chan) == ESP_OK) {
        rmt_copy_encoder_config_t ce = {};
        rmt_new_copy_encoder(&ce, &ir_encoder);
        rmt_enable(ir_chan);
    }
    Serial.printf("FOX: IR ready, %u codes\n", ir_n_codes);
}

__attribute__((unused)) static const char* ir_name(const IrEntry* e) {
    return ir_base ? (const char*)(ir_base + e->name_off) : "";
}

// Play one code's raw timing pairs with the right carrier.
static void ir_play(const IrEntry* e) {
    if (!ir_chan || !ir_encoder || !e) return;
    int freq = e->freq_div * 250; if (freq < 30000) freq = 38000;
    rmt_carrier_config_t carrier = {};
    carrier.frequency_hz = freq;
    carrier.duty_cycle = 0.33f;
    rmt_apply_carrier(ir_chan, &carrier);

    const uint16_t* t = (const uint16_t*)(ir_base + e->data_off);
    // Convert u16 on/off microsecond pairs to rmt_symbol_word_t.
    int pairs = e->n_pairs;
    rmt_symbol_word_t* syms = (rmt_symbol_word_t*)malloc(pairs * sizeof(rmt_symbol_word_t));
    if (!syms) return;
    for (int i = 0; i < pairs; ++i) {
        syms[i].level0 = 1; syms[i].duration0 = t[i * 2];
        syms[i].level1 = 0; syms[i].duration1 = t[i * 2 + 1];
    }
    rmt_transmit_config_t txc = {}; txc.loop_count = 0;
    rmt_transmit(ir_chan, ir_encoder, syms, pairs * sizeof(rmt_symbol_word_t), &txc);
    rmt_tx_wait_all_done(ir_chan, 200);
    free(syms);
}

// Direct command: send the user's saved code for (category, button) if known,
// else (for power) fall into the narrowing flow.
static int ir_saved_index(const char* category, const char* button);

void ir_command(const char* category, const char* button) {
    int idx = ir_saved_index(category, button);
    if (idx >= 0) { ir_play(&ir_table[idx]); speak("*beep* done~"); return; }
    // No saved code. For power we narrow; for others we sweep the named button.
    extern void ir_narrow_power();
    if (!strcmp(button, "power")) { ir_narrow_power(); return; }
    // sweep named button across all codes in category
    speak("trying a few...");
    for (uint16_t i = 0; i < ir_n_codes; ++i) {
        const IrEntry* e = &ir_table[i];
        if (e->category == 0 /*tv default*/ ) {
            ir_play(e); delay(120);
        }
    }
    speak("did any of those work?");
}

// ---- fake learning: binary-narrow the power-code pool -----------------------
static Preferences ir_prefs;

static int ir_saved_index(const char* category, const char* button) {
    ir_prefs.begin("foxir", true);
    String key = String(category) + "_" + button;
    int v = ir_prefs.getInt(key.c_str(), -1);
    ir_prefs.end();
    return (v >= 0 && v < ir_n_codes) ? v : -1;
}

static void ir_save_index(const char* category, const char* button, int idx) {
    ir_prefs.begin("foxir", false);
    String key = String(category) + "_" + button;
    ir_prefs.putInt(key.c_str(), idx);
    ir_prefs.end();
}

// Collect indices of power codes into a working set, then repeatedly split:
// play the first half, ask "that one?" If yes keep that half, else the other,
// until one code remains. Confirmation comes from the USER button (tap = yes)
// within a short window, which we poll here.

void ir_narrow_power() {
    if (!ir_table || !ir_n_codes) { speak("i don't have any tv codes"); return; }
    static uint16_t pool[400]; int np = 0;
    for (uint16_t i = 0; i < ir_n_codes && np < 400; ++i)
        if (ir_table[i].flags & 1) pool[np++] = i;
    if (!np) { speak("no power codes to try"); return; }

    speak("i'll sweep the tv. tap me the moment it turns off!");
    int lo = 0, hi = np;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        // play this batch
        for (int i = lo; i < mid; ++i) { ir_play(&ir_table[pool[i]]); delay(90); }
        // did it work during this batch?
        if (input_wait_yes(1800)) { hi = mid; }   // it was in [lo,mid)
        else {
            // try the other half; if that works, narrow there
            for (int i = mid; i < hi; ++i) { ir_play(&ir_table[pool[i]]); delay(90); }
            if (input_wait_yes(1800)) { lo = mid; }
            else { speak("hmm, none of those. let's try again later~"); return; }
        }
    }
    int chosen = pool[lo];
    ir_save_index("tv", "power", chosen);
    speak(String("got it! i learned your tv~"));
}
