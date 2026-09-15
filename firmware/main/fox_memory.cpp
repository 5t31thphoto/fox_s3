// fox_memory.cpp — device-owned persistence.
//
// The fox owns its memory. The cloud model never becomes the fox's memory; it
// only ever receives a bounded, recent slice as context. We keep a rolling
// journal in a LittleFS file on a dedicated partition rather than in NVS,
// because NVS string entries cap at ~4000 bytes and a journal outgrows that.
#include "fox.h"
#include <LittleFS.h>

static const char* JOURNAL = "/journal.txt";
static bool g_fs_ok = false;

void mem_begin() {
    // format-on-fail so a fresh device just works.
    g_fs_ok = LittleFS.begin(true, "/littlefs", 5, "foxfs");
    if (!g_fs_ok) {
        Serial.println("FOX: LittleFS mount failed; memory disabled this boot");
    }
}

void mem_append(const String& kind, const String& text) {
    if (!g_fs_ok || !text.length()) return;
    String line = kind + ":" + text;
    line.replace("\n", " ");
    line += "\n";

    // Append, then trim from the front if we've grown past the cap. We rewrite
    // the whole file on trim — cheap given the tiny cap and rare frequency.
    File f = LittleFS.open(JOURNAL, FILE_APPEND);
    if (!f) return;
    f.print(line);
    size_t sz = f.size();
    f.close();
    if (sz <= MEM_MAX_BYTES) return;

    File r = LittleFS.open(JOURNAL, FILE_READ);
    if (!r) return;
    String all = r.readString();
    r.close();
    int cut = (int)all.length() - (int)MEM_MAX_BYTES;
    int nl = all.indexOf('\n', cut);
    String trimmed = nl >= 0 ? all.substring(nl + 1) : all.substring(cut);
    File w = LittleFS.open(JOURNAL, FILE_WRITE);
    if (w) { w.print(trimmed); w.close(); }
}

String mem_tail(size_t max_bytes) {
    if (!g_fs_ok) return "";
    File r = LittleFS.open(JOURNAL, FILE_READ);
    if (!r) return "";
    size_t sz = r.size();
    if (sz > max_bytes) r.seek(sz - max_bytes);
    String out = r.readString();
    r.close();
    // Drop a possibly-partial first line after seeking mid-file.
    int nl = out.indexOf('\n');
    if (sz > max_bytes && nl >= 0) out = out.substring(nl + 1);
    return out;
}

void mem_clear() {
    if (g_fs_ok) LittleFS.remove(JOURNAL);
}
