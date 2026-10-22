/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Speakable text for radio values. See voice_text.h.
 */

#include "voice_text.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

namespace voice {

static bool all_digits(const std::string &s, size_t from, size_t to) {
    if (from >= to) {
        return false;
    }
    for (size_t i = from; i < to; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

static std::vector<std::string> words(const std::string &text) {
    std::vector<std::string> out;
    std::istringstream       in(text);
    std::string              w;

    while (in >> w) {
        out.push_back(w);
    }
    return out;
}

static std::string join(const std::vector<std::string> &w) {
    std::string out;

    for (const std::string &s : w) {
        if (s.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += s;
    }
    return out;
}

/* "0 0 5", "0 50", "200": each leading zero is its own word. */
static std::string khz_digits(int khz) {
    if (khz < 10) {
        return "0 0 " + std::to_string(khz);
    }
    if (khz < 100) {
        return "0 " + std::to_string(khz);
    }
    return std::to_string(khz);
}

/* Hz below 1 kHz as decimal digits of the kHz value: 50 -> " point 0 5". */
static std::string hz_digits(int hz) {
    if (hz == 0) {
        return "";
    }

    char buf[4];
    snprintf(buf, sizeof(buf), "%03d", hz);

    std::string digits(buf);
    while (!digits.empty() && digits.back() == '0') {
        digits.pop_back();
    }

    std::string out = " point";
    for (char c : digits) {
        out += ' ';
        out += c;
    }
    return out;
}

std::string freq_text(int32_t freq_hz) {
    if (freq_hz < 0) {
        freq_hz = 0;
    }

    int mhz = freq_hz / 1000000;
    int khz = (freq_hz / 1000) % 1000;
    int hz  = freq_hz % 1000;

    if (mhz == 0) {
        return std::to_string(khz) + hz_digits(hz) + " kilohertz";
    }
    return std::to_string(mhz) + ".|" + khz_digits(khz) + hz_digits(hz);
}

std::string band_text(const std::string &name, int32_t band_id) {
    if (band_id < 0) {
        return "out of band";
    }

    std::vector<std::string> w = words(name);

    for (std::string &s : w) {
        if (s.size() > 2 && s.compare(s.size() - 2, 2, "cm") == 0 && all_digits(s, 0, s.size() - 2)) {
            s = s.substr(0, s.size() - 2) + " centimeters";
        } else if (s.size() > 1 && s.back() == 'm' && all_digits(s, 0, s.size() - 1)) {
            s = s.substr(0, s.size() - 1) + " meters";
        }
    }

    std::string out = join(w);
    return out.empty() ? "unnamed band" : out;
}

struct Expansion {
    const char *from;
    const char *to;
};

/* Whole words only. Matching ignores a trailing comma. */
static const Expansion expansions[] = {
    {"Hz",     "hertz"            },
    {"kHz",    "kilohertz"        },
    {"dB",     "decibels"         },
    {"ms",     "milliseconds"     },
    {"wpm",    "words per minute" },
    {"W",      "watts"            },
    {"BW",     "bandwidth"        },
    {"H-Mic",  "hand mic"         },
    {"I-Mic",  "internal mic"     },
    {"MIC",    "mic"              },
    {"Sel",    "select"           },
    {"Moni",   "monitor"          },
    {"Comp",   "compressor"       },
    {"Dec",    "decoder"          },
    {"a-gain", "anti gain"        },
    {"SNR",    "S N R"            },
    {"VOL",    "volume"           },
    {"MFK",    "multi function"   },
    {"MEM",    "memory"           },
    {"APP",    "application"      },
    {"DFN",    "D F N"            },
    {"ZAP",    "zap"              },
};

std::string label_text(const std::string &label) {
    std::string flat;

    // Line breaks and colons are pauses, except a colon between digits,
    // which is a page number ("1:2" -> "1 of 2"). Parentheses go.
    for (size_t i = 0; i < label.size(); i++) {
        char c = label[i];

        if (c == '\n' || c == '\r' || c == '\t') {
            flat += ' ';
        } else if (c == ':') {
            bool digit_before = i > 0 && isdigit((unsigned char)label[i - 1]);
            bool digit_after  = i + 1 < label.size() && isdigit((unsigned char)label[i + 1]);

            flat += (digit_before && digit_after) ? " of " : ", ";
        } else if (c == '(' || c == ')') {
            flat += ' ';
        } else {
            flat += c;
        }
    }

    std::vector<std::string> w = words(flat);

    for (std::string &s : w) {
        bool        comma = !s.empty() && s.back() == ',';
        std::string core  = comma ? s.substr(0, s.size() - 1) : s;

        for (const Expansion &e : expansions) {
            if (core == e.from) {
                core = e.to;
                break;
            }
        }
        s = comma ? core + "," : core;
    }

    std::string out = join(w);

    // "Volume, " at the end of an empty value reads badly; drop it.
    while (!out.empty() && (out.back() == ',' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

std::string row_part_name(const std::string &row_label, size_t index, size_t count) {
    if (count <= 1) {
        return row_label;
    }

    std::vector<std::string> parts;
    std::string              part;

    for (char c : row_label + ",") {
        if (c == ',') {
            std::string trimmed = join(words(part));

            if (!trimmed.empty()) {
                parts.push_back(trimmed);
            }
            part.clear();
        } else {
            part += c;
        }
    }

    if (parts.size() == count && index < count) {
        return parts[index];
    }
    return row_label + ", " + std::to_string(index + 1) + " of " + std::to_string(count);
}

std::string s_meter_text(float dbm) {
    const float s1 = -121.0f;
    const float s9 = -73.0f;

    if (dbm > s9 + 0.5f) {
        int over = (int)std::lround(dbm - s9);
        return "S 9 plus " + std::to_string(over) + " decibels";
    }

    int s = (int)std::lround((dbm - s1) / 6.0f) + 1;

    if (s < 0) {
        s = 0;
    }
    if (s > 9) {
        s = 9;
    }
    return "S " + std::to_string(s);
}

std::string number_text(float value, int decimals) {
    if (decimals < 0) {
        decimals = 0;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", decimals, value);

    std::string out(buf);

    if (out.find('.') != std::string::npos) {
        while (!out.empty() && out.back() == '0') {
            out.pop_back();
        }
        if (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
    }
    if (out == "-0") {
        out = "0";
    }
    return out;
}

} // namespace voice
