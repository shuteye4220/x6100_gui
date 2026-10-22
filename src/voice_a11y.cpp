/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Spoken feedback for dialog items. See voice_a11y.h.
 *
 *  Everything here runs on the LVGL (UI) thread. Events only note which
 *  control to describe; the description is built on the next LVGL cycle
 *  (lv_async_call), after every callback of the event has run - a slider's
 *  value label, for one, is updated by a callback registered after ours.
 */

#include "voice_a11y.h"

#include "voice.h"
#include "voice_text.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

enum class Why {
    Focus,
    Value,
    Key,
};

struct ItemInfo {
    std::string name;
    lv_obj_t   *value_label = nullptr;
};

static std::unordered_map<lv_obj_t *, ItemInfo> items;

static lv_obj_t *pending_obj  = nullptr;
static Why       pending_why  = Why::Focus;
static bool      scheduled    = false;
static std::string page_name;
static bool        page_interrupt = true;
static uint32_t    page_time      = 0;

/* A page name only prefixes a focus that follows it closely. */
static const uint32_t page_prefix_ms = 1000;

/***** Name *****/

/* The direct child of the nearest grid-layout ancestor. */
static lv_obj_t *grid_cell(lv_obj_t *obj, lv_obj_t **grid) {
    for (lv_obj_t *cur = obj; cur; cur = lv_obj_get_parent(cur)) {
        lv_obj_t *parent = lv_obj_get_parent(cur);

        if (parent && lv_obj_get_style_layout(parent, LV_PART_MAIN) == LV_LAYOUT_GRID) {
            *grid = parent;
            return cur;
        }
    }
    return nullptr;
}

/* Registered controls under `obj`, in screen (child) order. */
static void collect_items(lv_obj_t *obj, std::vector<lv_obj_t *> &out) {
    if (items.count(obj)) {
        out.push_back(obj);
        return;
    }

    uint32_t cnt = lv_obj_get_child_cnt(obj);

    for (uint32_t i = 0; i < cnt; i++) {
        collect_items(lv_obj_get_child(obj, i), out);
    }
}

/* The label at the start of the control's grid row. Rows holding several
 * controls name them in order ("Day, Month, Year"); the control gets its
 * part, or the row label with its position. */
static std::string row_name(lv_obj_t *obj) {
    lv_obj_t *grid = nullptr;
    lv_obj_t *cell = grid_cell(obj, &grid);

    if (!cell) {
        return "";
    }

    lv_coord_t row = lv_obj_get_style_grid_cell_row_pos(cell, LV_PART_MAIN);
    uint32_t   cnt = lv_obj_get_child_cnt(grid);

    std::string             label;
    std::vector<lv_obj_t *> controls;

    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(grid, i);

        if (lv_obj_get_style_grid_cell_row_pos(child, LV_PART_MAIN) != row) {
            continue;
        }
        if (label.empty() && lv_obj_check_type(child, &lv_label_class) &&
            lv_obj_get_style_grid_cell_column_pos(child, LV_PART_MAIN) == 0) {
            label = voice::label_text(lv_label_get_text(child));
        } else {
            collect_items(child, controls);
        }
    }

    if (label.empty()) {
        return "";
    }

    size_t index = 0;

    for (size_t i = 0; i < controls.size(); i++) {
        if (controls[i] == obj) {
            index = i;
        }
    }
    return voice::row_part_name(label, index, controls.size());
}

static std::string own_label(lv_obj_t *obj) {
    uint32_t cnt = lv_obj_get_child_cnt(obj);

    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);

        if (lv_obj_check_type(child, &lv_label_class)) {
            return voice::label_text(lv_label_get_text(child));
        }
    }
    return "";
}

static std::string item_name(lv_obj_t *obj) {
    auto it = items.find(obj);

    if (it != items.end() && !it->second.name.empty()) {
        return it->second.name;
    }

    std::string name = row_name(obj);

    return name.empty() ? own_label(obj) : name;
}

/***** Value *****/

static std::string spinbox_value(lv_obj_t *obj) {
    lv_spinbox_t *sb       = (lv_spinbox_t *)obj;
    int32_t       value    = lv_spinbox_get_value(obj);
    int           decimals = sb->dec_point_pos ? (int)sb->digit_count - (int)sb->dec_point_pos : 0;

    if (decimals <= 0) {
        return std::to_string(value);
    }

    float div = 1.0f;
    for (int i = 0; i < decimals; i++) {
        div *= 10.0f;
    }
    return voice::number_text(value / div, decimals);
}

static std::string slider_value(lv_obj_t *obj) {
    auto it = items.find(obj);

    if (it != items.end() && it->second.value_label) {
        return voice::label_text(lv_label_get_text(it->second.value_label));
    }
    if (lv_slider_get_mode(obj) == LV_SLIDER_MODE_RANGE) {
        return "from " + std::to_string(lv_slider_get_left_value(obj)) + " to " +
               std::to_string(lv_slider_get_value(obj));
    }
    return std::to_string(lv_slider_get_value(obj));
}

static std::string item_value(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_switch_class)) {
        return lv_obj_has_state(obj, LV_STATE_CHECKED) ? "on" : "off";
    }

    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        char buf[64];

        /* While the list is open this is the highlighted option. */
        lv_dropdown_get_selected_str(obj, buf, sizeof(buf));
        return voice::label_text(buf);
    }

    if (lv_obj_check_type(obj, &lv_spinbox_class)) {
        return spinbox_value(obj);
    }

    if (lv_obj_check_type(obj, &lv_slider_class)) {
        return slider_value(obj);
    }

    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        const char *text = lv_textarea_get_text(obj);
        return (text && *text) ? std::string(text) : "empty";
    }

    if (lv_obj_check_type(obj, &lv_btn_class) && lv_obj_has_flag(obj, LV_OBJ_FLAG_CHECKABLE)) {
        return lv_obj_has_state(obj, LV_STATE_CHECKED) ? "on" : "off";
    }

    return "";
}

static bool item_editing(lv_obj_t *obj) {
    lv_group_t *group = (lv_group_t *)lv_obj_get_group(obj);

    return group && lv_group_get_editing(group) && lv_group_get_focused(group) == obj;
}

/***** Speech *****/

static void describe(lv_obj_t *obj, Why why) {
    std::string name  = item_name(obj);
    std::string value = item_value(obj);

    if (why == Why::Focus && item_editing(obj)) {
        value += value.empty() ? "editing" : ", editing";
    }

    if (!page_name.empty() && lv_tick_elaps(page_time) > page_prefix_ms) {
        page_name.clear();
    }

    if (why == Why::Focus && !page_name.empty()) {
        name = name.empty() ? page_name : page_name + ". " + name;
    }

    std::string text;

    if (name.empty()) {
        text = value;
    } else if (value.empty()) {
        text = name;
    } else {
        /* The name is skipped if it was just heard: "Voice rate 100",
         * then "101", "102" while editing. */
        text = name + "|" + value;
    }

    bool interrupt = true;

    if (why == Why::Focus && !page_name.empty()) {
        interrupt      = page_interrupt;
        page_name.clear();
    }

    voice_say_focus(text.c_str(), interrupt);
}

static void run_pending(void *) {
    scheduled = false;

    lv_obj_t *obj = pending_obj;
    pending_obj   = nullptr;

    if (obj) {
        describe(obj, pending_why);
    }
}

static void note(lv_obj_t *obj, Why why) {
    /* A focus change outranks a value change of the control just left. */
    if (pending_obj && pending_why == Why::Focus && why != Why::Focus && pending_obj != obj) {
        return;
    }

    pending_obj = obj;
    pending_why = why;

    if (!scheduled) {
        scheduled = true;
        lv_async_call(run_pending, nullptr);
    }
}

/***** Events *****/

static void focus_cb(lv_event_t *e) {
    note(lv_event_get_target(e), Why::Focus);
}

static void value_cb(lv_event_t *e) {
    note(lv_event_get_target(e), Why::Value);
}

static void key_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);

    if (!lv_obj_check_type(obj, &lv_dropdown_class)) {
        return;
    }

    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    switch (key) {
        case LV_KEY_UP:
        case LV_KEY_DOWN:
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
        case LV_KEY_ESC:
            note(obj, Why::Key);
            break;

        default:
            break;
    }
}

static void delete_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);

    items.erase(obj);
    if (pending_obj == obj) {
        pending_obj = nullptr;
    }
}

/***** API *****/

void voice_a11y_item(lv_obj_t *obj) {
    items.emplace(obj, ItemInfo{});

    lv_obj_add_event_cb(obj, focus_cb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(obj, value_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(obj, key_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, nullptr);
}

void voice_a11y_set_name(lv_obj_t *obj, const char *name) {
    auto it = items.find(obj);

    if (it != items.end() && name) {
        it->second.name = voice::label_text(name);
    }
}

void voice_a11y_set_value_label(lv_obj_t *obj, lv_obj_t *label) {
    auto it = items.find(obj);

    if (it != items.end()) {
        it->second.value_label = label;
    }
}

void voice_a11y_page(const char *name, bool interrupt) {
    page_name      = name ? name : "";
    page_interrupt = interrupt;
    page_time      = lv_tick_get();

    /* If the page has no controls this is all that is said; otherwise the
     * first control's focus replaces it with "<page>. <control> <value>". */
    if (!page_name.empty()) {
        voice_say_focus(page_name.c_str(), interrupt);
    }
}
