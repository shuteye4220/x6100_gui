/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Speech scheduling for the voice assistant.
 *
 *  This is the part of the voice assistant that decides *what* to say and
 *  *when*; it has no dependency on RHVoice, LVGL or the radio, so it can be
 *  unit tested on the host (tests/test_voice.cpp). voice.cpp owns the worker
 *  thread, the lock and the synthesizer, and drives a VoiceQueue.
 *
 *  Model
 *  -----
 *  Every utterance has a key (a channel, e.g. "freq" or "NR level") and a due
 *  time. A newer utterance on the same key replaces a pending one, so turning
 *  a knob quickly only speaks where it stopped. Depending on its abort policy
 *  a new utterance can also cut short the one being spoken right now: user
 *  actions interrupt (like a screen reader), background announcements don't.
 *
 *  Utterance text may contain one '|', splitting it into a label and a value
 *  ("NR level|5"). If the same label was heard in full a moment ago, only the
 *  value is spoken ("6", "7"...), so stepping through values stays quick. An
 *  utterance that was cut short doesn't count as heard.
 *
 *  Thread safety: none here. The owner serialises every call with one lock.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace voice {

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Ms        = std::chrono::milliseconds;

enum class Abort {
    None,    // never cut short what is being said
    SameKey, // cut it short only if it is on the same key
    Any,     // cut short whatever is being said (a direct user action)
};

struct Request {
    std::string text;
    std::string key;            // empty: never replaces, never replaced
    Ms          delay{0};
    Abort       abort = Abort::SameKey;
};

struct Utterance {
    uint64_t    id;
    std::string key;
    std::string text;   // as requested, with the '|' if any
    std::string spoken; // what to synthesise, after label elision
};

class VoiceQueue {
  public:
    // How long a label counts as "just heard" for elision.
    static constexpr Ms label_memory{5000};

    // Queue a request. Returns true if the utterance currently being spoken
    // must be cut short (the caller then stops the synthesizer).
    bool push(const Request &req, TimePoint now);

    // Drop everything pending and ask for the current utterance to stop.
    // Returns true if something was being spoken.
    bool clear();

    // Worker side. Takes the next utterance whose due time has come and
    // marks it as being spoken. Returns nothing if none is due yet.
    std::optional<Utterance> pop_due(TimePoint now);

    // Due time of the earliest pending utterance, if any.
    std::optional<TimePoint> next_due() const;

    // Worker side. The utterance from pop_due() has finished; `completed` is
    // false if it was cut short.
    void finished(TimePoint now, bool completed);

    // True while the current utterance has been asked to stop.
    bool abort_requested() const { return abort_; }

    bool   speaking() const { return current_.has_value(); }
    size_t pending() const { return queue_.size(); }

  private:
    struct Item {
        uint64_t    id;
        std::string key;
        std::string text;
        TimePoint   due;
    };

    std::string render(const std::string &text, TimePoint now);

    std::deque<Item>         queue_;
    std::optional<Utterance> current_;
    std::string              current_label_;
    bool                     abort_   = false;
    uint64_t                 next_id_ = 1;

    std::string last_label_;
    TimePoint   last_label_time_{};
    bool        have_label_ = false;
};

/* Splits "label|value". Without a '|' the label is empty. */
void split_label(const std::string &text, std::string &label, std::string &value);

} // namespace voice
