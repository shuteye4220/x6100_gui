/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "settings_types.h"

#ifdef __cplusplus

#include <cstdarg>

extern "C" {
#include <stdint.h>
#endif

typedef struct {
    const char *name;
    const char *label;
    const char *welcome;
} voice_item_t;

extern voice_item_t voice_item[VOICES_NUM];

/* Speak the next prompt even if voice mode is off. */
void voice_sure();
void voice_change_mode();

/* Immediate: cuts short whatever is being said (a direct user action). */
void voice_say_text_fmt(const char * fmt, ...);

/* After a short pause. "label|value" texts replace a pending one with the
 * same label, and skip the label if it was just heard. */
void voice_delay_say_text_fmt(const char * fmt, ...);

/* The tuned frequency, after the knob settles. Prefixed with the band name
 * when tuning has crossed into another band (or out of band). */
void voice_say_freq(uint64_t freq);

void voice_say_bool(const char *prompt, bool x);
void voice_say_int(const char *prompt, int32_t x);
void voice_say_float(const char *prompt, float x);
void voice_say_float2(const char *prompt, float x);
void voice_say_text(const char *prompt, const char *x);
void voice_say_lang();

/* Band and frequency, after a band key press. */
void voice_announce_band();

/* VFO, band, frequency, mode, filter and power. */
void voice_say_status();

/* S-meter, and the last transmission's power and SWR. */
void voice_say_meter();

/* What the function keys do, after a short pause (cuts short what is being
 * said: a key page change is a user action). */
void voice_say_keys(const char *text);

/* Settings focus and value changes, after a very short pause. `interrupt`
 * cuts short whatever is being said (the user moved); without it the text
 * waits its turn (the first focus when a window opens). */
void voice_say_focus(const char *text, bool interrupt);

/* Transmit started/stopped. Speech is held while transmitting, and the
 * power and SWR are reported afterwards if that option is on. */
void voice_set_tx(bool tx);

#ifdef __cplusplus
}
#endif
