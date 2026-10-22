/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - tests for the voice assistant's scheduling
 *  (src/voice_queue.cpp) and speakable text (src/voice_text.cpp)
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "voice_queue.h"
#include "voice_text.h"

using namespace voice;

namespace {

// A worker that speaks instantly, driven by a fake clock.
struct Harness {
    VoiceQueue               q;
    TimePoint                now{};
    std::vector<std::string> heard;

    bool push(const std::string &text, const std::string &key, int delay_ms, Abort abort) {
        return q.push(Request{text, key, Ms(delay_ms), abort}, now);
    }

    void advance(int ms) { now += Ms(ms); }

    // Speak everything due now, each taking `dur_ms`.
    void run(int dur_ms = 0) {
        while (auto u = q.pop_due(now)) {
            advance(dur_ms);
            heard.push_back(u->spoken);
            q.finished(now, true);
        }
    }
};

} // namespace

TEST_CASE("an immediate request is spoken at once", "[voice][queue]") {
    Harness h;

    h.push("Display off", "now", 0, Abort::Any);
    h.run();

    CHECK(h.heard == std::vector<std::string>{"Display off"});
    CHECK(h.q.pending() == 0);
}

TEST_CASE("a delayed request waits for its time", "[voice][queue]") {
    Harness h;

    h.push("14.|200", "freq", 1000, Abort::SameKey);
    h.run();
    CHECK(h.heard.empty());

    h.advance(999);
    h.run();
    CHECK(h.heard.empty());

    h.advance(1);
    h.run();
    CHECK(h.heard == std::vector<std::string>{"14. 200"});
}

TEST_CASE("turning the knob speaks only where it stopped", "[voice][queue]") {
    Harness h;

    for (int khz = 200; khz < 210; khz++) {
        h.push("14.|" + std::to_string(khz), "freq", 1000, Abort::SameKey);
        h.advance(100);
        h.run();
    }
    CHECK(h.heard.empty());

    h.advance(1000);
    h.run();
    CHECK(h.heard == std::vector<std::string>{"14. 209"});
}

TEST_CASE("nothing is dropped while speaking", "[voice][queue]") {
    // The old engine ignored every request made while it was talking, so a
    // frequency change during a prompt was never announced.
    Harness h;

    h.push("USB modulation", "now", 0, Abort::Any);
    auto u = h.q.pop_due(h.now);
    REQUIRE(u);

    // While "USB modulation" is still being spoken...
    bool stop = h.push("14.|074", "freq", 1000, Abort::SameKey);
    CHECK_FALSE(stop);
    CHECK_FALSE(h.q.abort_requested());

    h.advance(1500);
    h.q.finished(h.now, true);
    h.run();

    CHECK(h.heard == std::vector<std::string>{"14. 074"});
}

TEST_CASE("a user action interrupts whatever is being said", "[voice][queue]") {
    Harness h;

    h.push("Settings window", "now", 0, Abort::Any);
    REQUIRE(h.q.pop_due(h.now));

    bool stop = h.push("Preamplifier On", "now", 0, Abort::Any);
    CHECK(stop);
    CHECK(h.q.abort_requested());

    h.q.finished(h.now, false);
    CHECK_FALSE(h.q.abort_requested());

    h.run();
    CHECK(h.heard == std::vector<std::string>{"Preamplifier On"});
}

TEST_CASE("a newer value cuts short the old value on the same key only", "[voice][queue]") {
    Harness h;

    h.push("NR level|5", "NR level", 0, Abort::SameKey);
    REQUIRE(h.q.pop_due(h.now));

    CHECK_FALSE(h.push("NB|is on", "NB", 1000, Abort::SameKey));
    CHECK_FALSE(h.q.abort_requested());

    CHECK(h.push("NR level|6", "NR level", 1000, Abort::SameKey));
    CHECK(h.q.abort_requested());
}

TEST_CASE("background announcements never interrupt", "[voice][queue]") {
    Harness h;

    h.push("Settings window", "now", 0, Abort::Any);
    REQUIRE(h.q.pop_due(h.now));

    CHECK_FALSE(h.push("F1 General, F2 Interface", "fkeys", 400, Abort::None));

    h.advance(1000);
    h.q.finished(h.now, true);
    h.run();

    CHECK(h.heard == std::vector<std::string>{"F1 General, F2 Interface"});
}

TEST_CASE("requests without a key queue up instead of replacing", "[voice][queue]") {
    Harness h;

    h.push("one", "", 0, Abort::None);
    h.push("two", "", 0, Abort::None);
    h.push("three", "", 0, Abort::None);
    h.run(10);

    CHECK(h.heard == std::vector<std::string>{"one", "two", "three"});
}

TEST_CASE("earliest due is spoken first, ties in arrival order", "[voice][queue]") {
    Harness h;

    h.push("later", "a", 500, Abort::None);
    h.push("first", "b", 0, Abort::None);
    h.push("second", "c", 0, Abort::None);
    h.advance(500);
    h.run();

    CHECK(h.heard == std::vector<std::string>{"first", "second", "later"});
}

TEST_CASE("a label heard a moment ago is not repeated", "[voice][queue]") {
    Harness h;

    h.push("NR level|5", "NR level", 0, Abort::SameKey);
    h.run(500);
    h.push("NR level|6", "NR level", 0, Abort::SameKey);
    h.run(500);
    h.push("NR level|7", "NR level", 0, Abort::SameKey);
    h.run(500);

    CHECK(h.heard == std::vector<std::string>{"NR level 5", "6", "7"});
}

TEST_CASE("the label comes back after a pause or another prompt", "[voice][queue]") {
    Harness h;

    h.push("NR level|5", "NR level", 0, Abort::SameKey);
    h.run(200);

    h.advance((int)VoiceQueue::label_memory.count() + 1);
    h.push("NR level|6", "NR level", 0, Abort::SameKey);
    h.run(200);

    h.push("Display on", "now", 0, Abort::Any);
    h.run(200);
    h.push("NR level|7", "NR level", 0, Abort::SameKey);
    h.run(200);

    CHECK(h.heard == std::vector<std::string>{"NR level 5", "NR level 6", "Display on", "NR level 7"});
}

TEST_CASE("a doubled bar is not spoken", "[voice][queue]") {
    // voice_say_float("Battery voltage|", v) formats "%s|%s".
    Harness h;

    h.push("Battery voltage||12.4", "Battery voltage", 0, Abort::SameKey);
    h.run();
    CHECK(h.heard == std::vector<std::string>{"Battery voltage 12.4"});
}

TEST_CASE("an utterance cut short doesn't count as heard", "[voice][queue]") {
    Harness h;

    h.push("NR level|5", "NR level", 0, Abort::SameKey);
    REQUIRE(h.q.pop_due(h.now));
    h.push("NR level|6", "NR level", 0, Abort::SameKey);
    h.q.finished(h.now, false);

    h.run();
    CHECK(h.heard == std::vector<std::string>{"NR level 6"});
}

TEST_CASE("clear drops pending speech and stops the current one", "[voice][queue]") {
    Harness h;

    CHECK_FALSE(h.q.clear());

    h.push("one", "a", 0, Abort::None);
    h.push("two", "b", 1000, Abort::None);
    REQUIRE(h.q.pop_due(h.now));

    CHECK(h.q.clear());
    CHECK(h.q.abort_requested());
    CHECK(h.q.pending() == 0);

    h.q.finished(h.now, false);
    h.advance(2000);
    h.run();
    CHECK(h.heard.empty());
}

TEST_CASE("next_due reports the earliest pending time", "[voice][queue]") {
    Harness h;

    CHECK_FALSE(h.q.next_due());
    h.push("a", "a", 800, Abort::None);
    h.push("b", "b", 300, Abort::None);
    REQUIRE(h.q.next_due());
    CHECK(*h.q.next_due() == h.now + Ms(300));
}

TEST_CASE("nothing new starts until the current utterance finishes", "[voice][queue]") {
    Harness h;

    h.push("one", "a", 0, Abort::None);
    h.push("two", "b", 0, Abort::None);
    REQUIRE(h.q.pop_due(h.now));
    CHECK_FALSE(h.q.pop_due(h.now));
}

// ---- Speakable text --------------------------------------------------------

TEST_CASE("frequencies are unambiguous", "[voice][text]") {
    CHECK(freq_text(14200000) == "14.|200");
    CHECK(freq_text(14074000) == "14.|0 74");
    CHECK(freq_text(14005000) == "14.|0 0 5");
    CHECK(freq_text(14000000) == "14.|0 0 0");
    CHECK(freq_text(7030500) == "7.|0 30 point 5");
    // The old engine printed 14 200 050 Hz as "14.200.50", i.e. 14 200 500 Hz.
    CHECK(freq_text(14200050) == "14.|200 point 0 5");
    CHECK(freq_text(14200005) == "14.|200 point 0 0 5");
    CHECK(freq_text(432100000) == "432.|100");
    CHECK(freq_text(810000) == "810 kilohertz");
    CHECK(freq_text(153500) == "153 point 5 kilohertz");
}

TEST_CASE("band names are spoken in words", "[voice][text]") {
    CHECK(band_text("20m SSB", 7) == "20 meters SSB");
    CHECK(band_text("160m", 1) == "160 meters");
    CHECK(band_text("70cm", 302) == "70 centimeters");
    CHECK(band_text("Broadcast 41m", 103) == "Broadcast 41 meters");
    CHECK(band_text("CB A", 201) == "CB A");
    CHECK(band_text("", -1) == "out of band");
    CHECK(band_text("20m", -1) == "out of band");
    CHECK(band_text("", 5) == "unnamed band");
}

TEST_CASE("function key labels are spelled out", "[voice][text]") {
    CHECK(label_text("Filter BW:\n2700 Hz") == "Filter bandwidth, 2700 hertz");
    CHECK(label_text("TX power:\n5.0 W") == "TX power, 5.0 watts");
    CHECK(label_text("H-Mic gain:\n50") == "hand mic gain, 50");
    CHECK(label_text("VOX a-gain:\n10") == "VOX anti gain, 10");
    CHECK(label_text("Speed:\n20 wpm") == "Speed, 20 words per minute");
    CHECK(label_text("(VOL 1:2)") == "volume 1 of 2");
    CHECK(label_text("SWR\nScan") == "SWR Scan");
    CHECK(label_text("AGC knee:\n-60 dB") == "AGC knee, -60 decibels");
    CHECK(label_text("Volume:\n") == "Volume");
    CHECK(label_text("") == "");
}

TEST_CASE("controls sharing a settings row get their own names", "[voice][text]") {
    CHECK(row_part_name("Day, Month, Year", 0, 3) == "Day");
    CHECK(row_part_name("Day, Month, Year", 2, 3) == "Year");
    CHECK(row_part_name("Play,Rec gain", 1, 2) == "Rec gain");
    CHECK(row_part_name("Speaker mode", 0, 1) == "Speaker mode");
    CHECK(row_part_name("TX IQ offsets", 1, 2) == "TX IQ offsets, 2 of 2");
    CHECK(row_part_name("Hour, Min, Sec", 1, 2) == "Hour, Min, Sec, 2 of 2");
}

TEST_CASE("S-meter readings", "[voice][text]") {
    CHECK(s_meter_text(-121) == "S 1");
    CHECK(s_meter_text(-97) == "S 5");
    CHECK(s_meter_text(-73) == "S 9");
    CHECK(s_meter_text(-53) == "S 9 plus 20 decibels");
    CHECK(s_meter_text(-140) == "S 0");
}

TEST_CASE("numbers drop useless decimals", "[voice][text]") {
    CHECK(number_text(5.0f, 1) == "5");
    CHECK(number_text(1.25f, 2) == "1.25");
    CHECK(number_text(1.3f, 1) == "1.3");
    CHECK(number_text(12.0f, 0) == "12");
    CHECK(number_text(-0.01f, 1) == "0");
}
