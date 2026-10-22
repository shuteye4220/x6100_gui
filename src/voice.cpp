/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 *
 *  Voice assistant: one long-lived worker thread speaks what VoiceQueue
 *  (voice_queue.h) hands it. Callers only queue text under a lock and never
 *  wait for speech. Speech is stopped cooperatively - the synthesizer's
 *  audio callback returns false - so no thread is ever cancelled mid-way
 *  and the speaker/Bluetooth routing is always put back.
 */

#include "voice.h"

#include "voice_queue.h"
#include "voice_text.h"
#include "display.h"
#include "lock_manager.h"

/* Before the C headers: radio.h pulls these in, and they are C++. */
#include "cfg/cfg_api.h"
#include "cfg/db.h"

extern "C" {
#include <aether_radio/x6100_control/control.h>

#include "bt_audio.h"
#include "meter.h"
#include "msg.h"
#include "radio.h"
#include "recorder.h"
#include "tx_info.h"
}

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <RHVoice/core/engine.hpp>
#include <RHVoice/core/document.hpp>
#include <RHVoice/core/client.hpp>
#include <RHVoice/audio.hpp>

using namespace RHVoice;

/* Pause before a value is spoken, so turning a knob only speaks where it
 * stopped. Anything being said on the same channel is cut short at once. */
static const voice::Ms value_delay{500};

/* Pause before a settings focus/value is spoken: just enough to skip the
 * rows scrolled past. */
static const voice::Ms focus_delay{150};

/* Function keys are listed once the window/page name has been said, and
 * before the first settings row. */
static const voice::Ms keys_delay{100};

voice_item_t voice_item[VOICES_NUM] = {
    {.name = "lyubov",      .label = "Lyubov (En)",  .welcome = "Hello. This is voice Lyubov" },
    {.name = "slt",         .label = "SLT (En)",     .welcome = "Hello. This is voice S L T"  },
    {.name = "alan",        .label = "Alan (En)",    .welcome = "Hello. This is voice Alan"   },
    {.name = "evgeniy-eng", .label = "Evgeniy (En)", .welcome = "Hello. This is voice Evgeniy"},
};

/* Never destroyed: the worker is detached and may still be waiting when the
 * process exits. */
struct VoiceState {
    std::mutex              lock;
    std::condition_variable wake;
    voice::VoiceQueue       queue;
    std::atomic<bool>       abort{false};
    bool                    tx = false;
    std::once_flag          started;
};

/* Created on first use, so prompts work whatever the static init order. */
static VoiceState *vs() {
    static VoiceState *state = new VoiceState;
    return state;
}

static std::atomic<bool> sure{false};

/* Band last spoken with a frequency; -2: none yet. UI thread only. */
static int32_t last_band_id = -2;

/***** Synthesis (worker thread) *****/

/* Never speak while transmitting: the speaker routing mutes the mics. */
static bool speech_blocked() {
    switch (radio_get_state()) {
        case RADIO_TX:
        case RADIO_ATU_START:
        case RADIO_ATU_WAIT:
        case RADIO_ATU_RUN:
        case RADIO_SWRSCAN:
            return true;

        default:
            return false;
    }
}

class audio_player : public client {
  public:
    explicit audio_player(const char *device) {
        stream.set_sample_rate(24000);
        stream.set_buffer_size(512);
        if (device) {
            stream.set_device(device);
        }
        stream.open();
    }

    ~audio_player() {
        if (stream.is_open()) {
            stream.close();
        }
    }

    bool play_speech(const short *buf, std::size_t count) override {
        if (stopped()) {
            return false;
        }
        try {
            stream.write(buf, count);
            return true;
        } catch (...) {
            failed = true;
            return false;
        }
    }

    /* Wait for the audio to play out, or drop it if speech was stopped. */
    void finish() {
        if (!stream.is_open()) {
            return;
        }
        if (stopped() || failed) {
            stream.flush();
        } else {
            stream.drain();
        }
    }

    bool stopped() const { return vs()->abort.load() || speech_blocked(); }

    bool failed = false;

  private:
    audio::playback_stream stream;
};

/* Puts the speaker routing back however synthesis ends. */
struct SpeakerGuard {
    explicit SpeakerGuard(bool on) : on(on) {
        if (on) {
            radio_speaker_play(true);
        }
    }
    ~SpeakerGuard() {
        if (on) {
            radio_speaker_play(false);
        }
    }
    bool on;
};

struct BtGuard {
    BtGuard() { on = bt_audio_playback_begin(sink, sizeof(sink)); }
    ~BtGuard() {
        if (on) {
            bt_audio_playback_end();
        }
    }
    char sink[96];
    bool on;
};

/* Returns true if the text was spoken to the end. */
static bool synthesize(const std::string &text) {
    static std::shared_ptr<engine> eng;

    if (text.empty() || speech_blocked()) {
        return false;
    }

    try {
        if (!eng) {
            eng = std::make_shared<engine>();
        }

        voice_profile profile = eng->create_voice_profile(voice_item[cfg.voice.lang()->get()].name);

        /* Bluetooth listening: speak into the headphones, the radio's
         * speaker is silent. */
        BtGuard      bt;
        audio_player player(bt.on ? bt.sink : nullptr);
        SpeakerGuard speaker(!bt.on);

        std::istringstream             in{text};
        std::istreambuf_iterator<char> start{in};
        std::istreambuf_iterator<char> end;
        std::unique_ptr<document>      doc = document::create_from_plain_text(eng, start, end, content_text, profile);

        doc->speech_settings.relative.rate   = cfg.voice.rate()->get() / 100.0;
        doc->speech_settings.relative.pitch  = cfg.voice.pitch()->get() / 100.0;
        doc->speech_settings.relative.volume = cfg.voice.volume()->get() / 100.0;
        doc->set_owner(player);
        doc->synthesize();

        bool completed = !player.stopped() && !player.failed;
        player.finish();
        return completed;
    } catch (const std::exception &e) {
        fprintf(stderr, "voice: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "voice: synthesis failed\n");
    }
    return false;
}

static void worker() {
    std::unique_lock<std::mutex> lk(vs()->lock);

    for (;;) {
        std::optional<voice::Utterance> u;

        if (!vs()->tx) {
            u = vs()->queue.pop_due(voice::Clock::now());
        }

        if (!u) {
            auto due = vs()->queue.next_due();

            if (vs()->tx || !due) {
                vs()->wake.wait(lk);
            } else {
                vs()->wake.wait_until(lk, *due);
            }
            continue;
        }

        vs()->abort = false;
        lk.unlock();

        bool completed = synthesize(u->spoken);

        lk.lock();
        vs()->queue.finished(voice::Clock::now(), completed);
    }
}

/***** Queueing (any thread) *****/

static void push(const std::string &text, const std::string &key, voice::Ms delay, voice::Abort abort) {
    std::call_once(vs()->started, [] { std::thread(worker).detach(); });

    {
        std::lock_guard<std::mutex> lk(vs()->lock);

        if (vs()->queue.push(voice::Request{text, key, delay, abort}, voice::Clock::now())) {
            vs()->abort = true;
        }
    }
    vs()->wake.notify_one();
}

static bool voice_enable() {
    /* Consumed here either way, so it can't leak to a later prompt. */
    bool forced = sure.exchange(false);

    if (recorder_is_on()) {
        return false;
    }

    if (forced) {
        return true;
    }

    switch (cfg.voice.mode()->get()) {
        case VOICE_OFF:
            return false;

        case VOICE_ALWAYS:
            return true;

        case VOICE_LCD:
            return !display_is_on();
    }

    return false;
}

static std::string vformat(const char *fmt, va_list args) {
    char buf[512];

    vsnprintf(buf, sizeof(buf), fmt, args);
    return buf;
}

/* "label|value" goes on the label's channel; plain text queues up. */
static void say_value(const std::string &text) {
    std::string label, value;

    voice::split_label(text, label, value);

    if (label.empty()) {
        push(text, "", value_delay, voice::Abort::None);
    } else {
        push(text, label, value_delay, voice::Abort::SameKey);
    }
}

void voice_sure() {
    sure = true;
}

void voice_change_mode() {
    voice_sure();

    switch (cfg.voice.mode()->get()) {
        case VOICE_OFF:
            cfg.voice.mode()->set(VOICE_LCD);
            msg_update_text_fmt("Voice mode: LCD");
            voice_say_text("Voice mode|", "is LCD");
            break;

        case VOICE_LCD:
            cfg.voice.mode()->set(VOICE_ALWAYS);
            msg_update_text_fmt("Voice mode: Always");
            voice_say_text("Voice mode|", "is always");
            break;

        case VOICE_ALWAYS:
            cfg.voice.mode()->set(VOICE_OFF);
            msg_update_text_fmt("Voice mode: Off");
            voice_say_text("Voice mode|", "is off");
            break;
    }
}

void voice_say_text_fmt(const char *fmt, ...) {
    if (!voice_enable()) {
        return;
    }

    va_list args;

    va_start(args, fmt);
    std::string text = vformat(fmt, args);
    va_end(args);

    push(text, "now", voice::Ms(0), voice::Abort::Any);
}

void voice_delay_say_text_fmt(const char *fmt, ...) {
    if (!voice_enable()) {
        return;
    }

    va_list args;

    va_start(args, fmt);
    std::string text = vformat(fmt, args);
    va_end(args);

    say_value(text);
}

/***** Band and frequency *****/

/* "40 meters CW, " when `freq` is in a different band than last spoken. */
static std::string band_prefix(int32_t freq, bool force) {
    BandInfoLoadResult band = BandsTable::get_by_freq((uint32_t)freq);
    int32_t            id   = band.rc == SUCCESS ? band.value.id : BAND_UNDEFINED;

    if (!force && id == last_band_id) {
        return "";
    }
    last_band_id = id;

    return voice::band_text(band.value.name, id) + ", ";
}

static void say_freq(int32_t freq, bool force_band, voice::Ms delay, voice::Abort abort) {
    std::string text = voice::freq_text(freq);
    std::string band = band_prefix(freq, force_band);

    if (!band.empty()) {
        /* The band is said once, as part of the label, when it changes:
         * "40 meters CW, 7.|030", then "7.|031" -> "7. 031", then "032"... */
        size_t bar = text.find('|');
        text       = band + (bar == std::string::npos ? "|" + text : text);
    }

    push(text, "freq", delay, abort);
}

void voice_say_freq(uint64_t freq) {
    if (!voice_enable()) {
        return;
    }
    say_freq((int32_t)freq, false, value_delay, voice::Abort::SameKey);
}

void voice_announce_band() {
    if (!voice_enable()) {
        return;
    }
    say_freq(cfg.cur.fg_freq()->get(), true, voice::Ms(0), voice::Abort::Any);
}

/***** Status and meters *****/

static const char *mode_text(int32_t mode) {
    switch ((x6100_mode_t)mode) {
        case x6100_mode_lsb:     return "LSB";
        case x6100_mode_lsb_dig: return "LSB digital";
        case x6100_mode_usb:     return "USB";
        case x6100_mode_usb_dig: return "USB digital";
        case x6100_mode_cw:      return "CW";
        case x6100_mode_cwr:     return "CW reverse";
        case x6100_mode_am:      return "A M";
        case x6100_mode_nfm:     return "N F M";
    }
    return "unknown mode";
}

static std::string spoken_freq(int32_t freq) {
    std::string text = voice::freq_text(freq);
    size_t      bar  = text.find('|');

    if (bar != std::string::npos) {
        text.replace(bar, 1, " ");
    }
    return text;
}

void voice_say_status() {
    voice_sure();
    if (!voice_enable()) {
        return;
    }

    int32_t freq = cfg.cur.fg_freq()->get();
    bool    vfo_b = cfg.band.current_vfo()->get() == X6100_VFO_B;

    BandInfoLoadResult band = BandsTable::get_by_freq((uint32_t)freq);
    int32_t            id   = band.rc == SUCCESS ? band.value.id : BAND_UNDEFINED;

    std::string text = vfo_b ? "V F O B. " : "V F O A. ";

    text += voice::band_text(band.value.name, id) + ". ";
    text += spoken_freq(freq) + ". ";
    text += std::string(mode_text(cfg.cur.mode()->get())) + ". ";
    text += "Filter " + std::to_string(cfg.filter.bw()->get()) + " hertz. ";
    text += "Power " + voice::number_text(cfg.pwr()->get(), 1) + " watts.";

    if (cfg.band.split()->get()) {
        text += " Split on.";
    }
    if (lm_get_freq()) {
        text += " Frequency locked.";
    }

    last_band_id = id;
    push(text, "now", voice::Ms(0), voice::Abort::Any);
}

static std::string tx_report_text(bool after_tx) {
    float pwr = 0.0f, swr = 0.0f;

    if (!tx_info_get_summary(&pwr, &swr)) {
        return after_tx ? "" : "No transmission yet.";
    }

    std::string text = after_tx ? "" : "Last transmit, ";

    /* The SWR meter stops at 5. */
    std::string swr_text = swr >= 4.95f ? "5 or more" : voice::number_text(swr < 1.0f ? 1.0f : swr, 1);

    text += "power " + voice::number_text(pwr, 1) + " watts, ";
    text += "S W R " + swr_text + ".";
    return text;
}

void voice_say_meter() {
    voice_sure();
    if (!voice_enable()) {
        return;
    }

    std::string text = voice::s_meter_text(meter_get_db()) + ". " + tx_report_text(false);

    push(text, "now", voice::Ms(0), voice::Abort::Any);
}

/***** Function keys and settings *****/

void voice_say_keys(const char *text) {
    if (!text || !*text || !voice_enable()) {
        return;
    }
    /* A page change is a user action. A window's name, said right after
     * its keys are loaded, is queued later, so it is not cut short. */
    push(text, "keys", keys_delay, voice::Abort::Any);
}

void voice_say_focus(const char *text, bool interrupt) {
    if (!text || !*text || !voice_enable()) {
        return;
    }
    push(text, "focus", focus_delay, interrupt ? voice::Abort::Any : voice::Abort::None);
}

/***** Transmit *****/

void voice_set_tx(bool tx) {
    {
        std::lock_guard<std::mutex> lk(vs()->lock);

        if (vs()->tx == tx) {
            return;
        }
        vs()->tx = tx;

        /* Whatever was about to be said is stale once you key up. */
        if (tx && vs()->queue.clear()) {
            vs()->abort = true;
        }
    }
    vs()->wake.notify_one();

    if (!tx && cfg.voice.tx_report()->get() && voice_enable()) {
        std::string text = tx_report_text(true);

        if (!text.empty()) {
            push(text, "tx", value_delay, voice::Abort::None);
        }
    }
}

/***** Value prompts *****/

void voice_say_bool(const char *prompt, bool x) {
    voice_delay_say_text_fmt("%s|%s", prompt, x ? "is on" : "is off");
}

void voice_say_int(const char *prompt, int32_t x) {
    voice_delay_say_text_fmt("%s|%i", prompt, x);
}

void voice_say_float(const char *prompt, float x) {
    voice_delay_say_text_fmt("%s|%s", prompt, voice::number_text(x, 1).c_str());
}

void voice_say_float2(const char *prompt, float x) {
    voice_delay_say_text_fmt("%s|%s", prompt, voice::number_text(x, 2).c_str());
}

void voice_say_text(const char *prompt, const char *x) {
    voice_delay_say_text_fmt("%s|%s", prompt, x);
}

void voice_say_lang() {
    voice_delay_say_text_fmt("%s", voice_item[cfg.voice.lang()->get()].welcome);
}
