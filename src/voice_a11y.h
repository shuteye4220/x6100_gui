/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Spoken feedback for dialog items (the settings window): each control
 *  says its name and value when it gets focus, when editing starts or stops,
 *  when its value changes, and while a dropdown list is scrolled.
 *
 *  The name is the one given with voice_a11y_set_name(), or else the label
 *  at the start of the control's grid row.
 */

#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called by dialog_item() for every focusable dialog control. */
void voice_a11y_item(lv_obj_t *obj);

/* A name for a control that shares its row with others ("Voice rate"). */
void voice_a11y_set_name(lv_obj_t *obj, const char *name);

/* The label showing a slider's value as the user sees it ("-3.0 dB"). */
void voice_a11y_set_value_label(lv_obj_t *obj, lv_obj_t *label);

/* A page of a window was (re)built: its name is said before the first
 * control. `interrupt`: the user switched pages, cut short what is being
 * said; otherwise (a window just opened) wait for its name to be said. */
void voice_a11y_page(const char *name, bool interrupt);

#ifdef __cplusplus
}
#endif
