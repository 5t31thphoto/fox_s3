#pragma once
#include <Arduino.h>
#include "fox.h"

// FOXESE: Fox Semantic Encoding.  This is an on-device ABI between the tiny
// Brain A/B and deterministic firmware.  The neural model emits only this
// compact packet; firmware owns facts, capabilities, rendering and reality.
struct Foxese {
    uint8_t version = 1;
    uint8_t mood = 0;
    uint8_t fact_id = 0xFF;
    uint8_t style = 0;
    uint8_t gesture = 0;
    uint8_t intensity = 0;
    bool valid = false;
};

String foxese_encode(uint8_t mood, uint8_t fact_id, uint8_t style,
                     uint8_t gesture, uint8_t intensity);
bool foxese_parse(const String& packet, Foxese& out);
String foxese_expand(const Foxese& x, const String& fact);
uint8_t foxese_fact_id(const String& fact);
