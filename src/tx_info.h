/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <unistd.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

lv_obj_t * tx_info_init(lv_obj_t *parent);

void tx_info_update(float pwr, float vswr, float alc);

/* Peak power and the SWR at that moment during the last transmission (or
 * ATU tune). False if nothing has been transmitted since power-on. */
bool tx_info_get_summary(float *pwr_p, float *vswr_p);

bool tx_info_refresh(uint8_t * prev_msg_id, float * alc_p, float * pwr_p, float * vswr_p);
