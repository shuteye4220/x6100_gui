/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Speech scheduling for the voice assistant. See voice_queue.h.
 */

#include "voice_queue.h"

#include <algorithm>

namespace voice {

void split_label(const std::string &text, std::string &label, std::string &value) {
    size_t bar = text.find('|');

    if (bar == std::string::npos) {
        label.clear();
        value = text;
    } else {
        label = text.substr(0, bar);
        value = text.substr(bar + 1);
    }
}

bool VoiceQueue::push(const Request &req, TimePoint now) {
    if (!req.key.empty()) {
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [&](const Item &item) { return item.key == req.key; }),
                     queue_.end());
    }

    queue_.push_back(Item{next_id_++, req.key, req.text, now + req.delay});

    if (!current_) {
        return false;
    }

    bool stop = false;

    switch (req.abort) {
        case Abort::None:
            break;

        case Abort::SameKey:
            stop = !req.key.empty() && current_->key == req.key;
            break;

        case Abort::Any:
            stop = true;
            break;
    }

    if (stop) {
        abort_ = true;
    }
    return stop;
}

bool VoiceQueue::clear() {
    queue_.clear();

    if (current_) {
        abort_ = true;
        return true;
    }
    return false;
}

std::optional<TimePoint> VoiceQueue::next_due() const {
    if (queue_.empty()) {
        return std::nullopt;
    }

    TimePoint due = queue_.front().due;

    for (const Item &item : queue_) {
        due = std::min(due, item.due);
    }
    return due;
}

std::optional<Utterance> VoiceQueue::pop_due(TimePoint now) {
    if (current_) {
        return std::nullopt;
    }

    // Earliest due first; the queue is in arrival order, so the first of
    // equal due times is the oldest.
    auto best = queue_.end();

    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->due <= now && (best == queue_.end() || it->due < best->due)) {
            best = it;
        }
    }

    if (best == queue_.end()) {
        return std::nullopt;
    }

    Item item = *best;
    queue_.erase(best);

    std::string value;
    split_label(item.text, current_label_, value);

    current_ = Utterance{item.id, item.key, item.text, render(item.text, now)};
    abort_   = false;

    return current_;
}

void VoiceQueue::finished(TimePoint now, bool completed) {
    if (current_ && completed) {
        last_label_      = current_label_;
        last_label_time_ = now;
        have_label_      = !current_label_.empty();
    }

    current_.reset();
    current_label_.clear();
    abort_ = false;
}

std::string VoiceQueue::render(const std::string &text, TimePoint now) {
    std::string label, value;

    split_label(text, label, value);

    // Prompts like "Voice mode|" formatted as "%s|%s" carry a second '|'.
    value.erase(std::remove(value.begin(), value.end(), '|'), value.end());

    if (label.empty()) {
        return value;
    }

    bool heard = have_label_ && label == last_label_ && now - last_label_time_ < label_memory;

    if (heard && !value.empty()) {
        return value;
    }
    if (value.empty()) {
        return label;
    }
    return label + " " + value;
}

} // namespace voice
