/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Turning radio values into text that reads well through the speech
 *  synthesizer. Pure functions, unit tested on the host (tests/test_voice.cpp).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace voice {

/* A frequency as "MHz.|kHz", e.g. 14 200 050 Hz -> "14.|200 point 0 5".
 * The '|' lets the MHz part be skipped while tuning within the same MHz.
 * Leading zeros of the kHz part are spoken, so 14 005 kHz is not heard as
 * 14.5 MHz. Below 1 MHz the result is in kilohertz, without a '|'. */
std::string freq_text(int32_t freq_hz);

/* A band name from the band table made speakable: "40m CW" -> "40 meters CW",
 * "70cm" -> "70 centimeters". A negative band id (a gap between bands) is
 * "out of band". */
std::string band_text(const std::string &name, int32_t band_id);

/* An on-screen label made speakable: line breaks and colons become pauses,
 * units and abbreviations used on the function keys are spelled out
 * ("Filter BW:\n2700 Hz" -> "Filter bandwidth, 2700 hertz"). */
std::string label_text(const std::string &label);

/* The name of control `index` of `count` sharing a settings row. Row labels
 * list their controls in order: ("Day, Month, Year", 1, 3) -> "Month". If
 * the parts don't match the controls: ("TX IQ offsets", 1, 2) ->
 * "TX IQ offsets, 2 of 2". */
std::string row_part_name(const std::string &row_label, size_t index, size_t count);

/* S-meter reading in dBm as S units: "S 7", "S 9 plus 20 decibels". */
std::string s_meter_text(float dbm);

/* A number with at most `decimals` digits after the point, trailing zeros
 * dropped: 5.0 -> "5", 1.25 with 1 decimal -> "1.3". */
std::string number_text(float value, int decimals);

} // namespace voice
