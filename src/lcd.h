#pragma once
#include <Arduino.h>

extern uint8_t g_display_data[];

extern const uint8_t SEG_I_14[] PROGMEM;

extern const uint16_t CHAR_MAP_14SEG[] PROGMEM;

void set_segment_char(uint8_t seg_i, char c);
void set_segments(uint8_t i0, const char *text);
void set_all_segments(const char *text);

