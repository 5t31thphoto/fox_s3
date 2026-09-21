// Real SAM (Software Automatic Mouth) via ESP8266SAM / s-macke port.
// Collects 8-bit unsigned mono @ 22050 Hz into a malloc buffer for fox_voice.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "sam/sam.h"
#include "sam/reciter.h"
#include "sam/SamData.h"

SamData* samdata = NULL;

typedef struct {
    uint8_t* buf;
    int capacity;
    int length;
} SamBuf;

static void sam_cb(void* user, unsigned char b) {
    SamBuf* s = (SamBuf*)user;
    if (s->length >= s->capacity) {
        int nc = s->capacity ? s->capacity * 2 : 8192;
        uint8_t* n = (uint8_t*)realloc(s->buf, nc);
        if (!n) return;
        s->buf = n;
        s->capacity = nc;
    }
    s->buf[s->length++] = b;
}

int sam_render(const char* text, uint8_t speed, uint8_t pitch,
               uint8_t throat, uint8_t mouth, uint8_t** out, int* out_len) {
    if (!text || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;

    size_t n = strlen(text);
    if (n == 0 || n > 250) return 0;

    samdata = (SamData*)calloc(1, sizeof(SamData));
    if (!samdata) return 0;

    if (speed)  SetSpeed(speed);
    else        SetSpeed(72);
    if (pitch)  SetPitch(pitch);
    else        SetPitch(64);
    if (mouth)  SetMouth(mouth);
    else        SetMouth(128);
    if (throat) SetThroat(throat);
    else        SetThroat(128);
    EnableSingmode(0);

    char input[256];
    for (size_t i = 0; i < n; ++i)
        input[i] = (char)toupper((unsigned char)text[i]);
    input[n] = 0;
    strncat(input, "[", sizeof(input) - strlen(input) - 1);

    if (!TextToPhonemes(input)) {
        free(samdata);
        samdata = NULL;
        return 0;
    }
    SetInput(input);

    SamBuf sb = {0};
    sb.buf = (uint8_t*)malloc(16384);
    if (!sb.buf) {
        free(samdata);
        samdata = NULL;
        return 0;
    }
    sb.capacity = 16384;
    sb.length = 0;

    int ok = SAMMain(sam_cb, &sb);

    free(samdata);
    samdata = NULL;

    if (!ok || sb.length < 100) {
        free(sb.buf);
        return 0;
    }
    *out = sb.buf;
    *out_len = sb.length;
    return 1;
}

void sam_free(uint8_t* p) { free(p); }
