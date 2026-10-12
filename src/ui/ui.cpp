#include "ui/ui.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/mapping.h"
#include "app/session.h"
#include "bsp/bsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "net/wifi.h"

namespace garnet::ui {
namespace {

constexpr const char* kTag = "ui";
constexpr int kStripH = 64;
constexpr uint32_t kReleaseHoldMs = 3000;  // RFC-085: release is a held press
constexpr uint32_t kRefreshMs = 100;

constexpr uint32_t kRed = 0xC62828, kRedDark = 0x5A1010, kAmber = 0xE0A030,
                   kPanel = 0x101820, kText = 0xE8EEF2, kDim = 0x8A98A4;

struct Input {
    std::string id;
    std::string label;
    std::string kind;
    lv_obj_t* box = nullptr;
};

struct Row {
    const Setting* s = nullptr;
    lv_obj_t* widget = nullptr;
    lv_obj_t* value = nullptr;
};

enum class MapStep : uint8_t { Idle, WaitPress, Pick };

struct Ui {
    Session* session = nullptr;
    Mapping* mapping = nullptr;

    lv_obj_t* stripStatus = nullptr;
    lv_obj_t* strip = nullptr;
    lv_obj_t* pauseBtn = nullptr;
    lv_obj_t* pauseLbl = nullptr;
    lv_obj_t* haltBtn = nullptr;
    lv_obj_t* haltLbl = nullptr;
    uint32_t haltPressMs = 0;
    bool releaseArmed = false;
    bool releaseSent = false;

    lv_obj_t* tabs = nullptr;
    lv_obj_t* posLbl = nullptr;
    lv_obj_t* posBar = nullptr;
    lv_obj_t* safetyLbl = nullptr;
    lv_obj_t* roleLbl = nullptr;

    lv_obj_t* controls = nullptr;
    std::vector<Row> rows;
    uint32_t modelGen = 0;

    lv_obj_t* wifiLbl = nullptr;
    lv_obj_t* linkBtnLbl = nullptr;
    lv_obj_t* knockBtn = nullptr;

    std::vector<Input> inputs;  // physical inputs, manifest order (bsp bit order)
    lv_obj_t* mapText = nullptr;
    lv_obj_t* mapList = nullptr;
    lv_obj_t* mapStart = nullptr;
    lv_obj_t* mapPick = nullptr;
    lv_obj_t* mapSave = nullptr;
    lv_obj_t* mapSkip = nullptr;
    std::vector<std::string> pickIds;
    MapStep step = MapStep::Idle;
    size_t mapIndex = 0;

    uint32_t lastButtons = 0;
    uint32_t lastRefreshMs = 0;
};
Ui u;

uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font = &lv_font_montserrat_20,
                uint32_t color = kText) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb, lv_obj_t** lbl = nullptr) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_t* l = label(b, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    if (lbl) *lbl = l;
    return b;
}

void setEnabled(lv_obj_t* o, bool on) {
    if (on) {
        lv_obj_remove_state(o, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(o, LV_STATE_DISABLED);
    }
}

void refreshMapList();

// ---- the safety strip ---------------------------------------------------------

void haltEvent(lv_event_t* e) {
    const auto code = lv_event_get_code(e);
    Session& s = *u.session;
    if (code == LV_EVENT_PRESSED) {
        // A press that began before the latch never counts toward release.
        u.releaseArmed = s.estopLatched();
        u.releaseSent = false;
        u.haltPressMs = nowMs();
        if (!u.releaseArmed) s.halt();
    } else if (code == LV_EVENT_PRESSING && u.releaseArmed && !u.releaseSent &&
               nowMs() - u.haltPressMs >= kReleaseHoldMs) {
        u.releaseSent = true;
        s.releaseHalt();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        u.releaseArmed = false;
    }
}

void pauseEvent(lv_event_t*) { u.session->pauseToggle(); }

void buildStrip() {
    u.strip = lv_obj_create(lv_layer_top());
    lv_obj_set_size(u.strip, bsp::caps().width, kStripH);
    lv_obj_set_pos(u.strip, 0, 0);
    lv_obj_set_style_radius(u.strip, 0, 0);
    lv_obj_set_style_border_width(u.strip, 0, 0);
    lv_obj_set_style_pad_all(u.strip, 6, 0);
    lv_obj_set_style_bg_color(u.strip, lv_color_hex(kPanel), 0);
    lv_obj_remove_flag(u.strip, LV_OBJ_FLAG_SCROLLABLE);

    u.stripStatus = label(u.strip, "starting", &lv_font_montserrat_20, kText);
    lv_obj_set_width(u.stripStatus, 380);
    lv_label_set_long_mode(u.stripStatus, LV_LABEL_LONG_DOT);
    lv_obj_align(u.stripStatus, LV_ALIGN_LEFT_MID, 4, 0);

    u.pauseBtn = button(u.strip, "Pause", pauseEvent, &u.pauseLbl);
    lv_obj_set_size(u.pauseBtn, 150, 52);
    lv_obj_align(u.pauseBtn, LV_ALIGN_RIGHT_MID, -232, 0);
    lv_obj_set_style_bg_color(u.pauseBtn, lv_color_hex(kAmber), 0);
    lv_obj_set_style_text_color(u.pauseLbl, lv_color_hex(0x101010), 0);

    u.haltBtn = lv_button_create(u.strip);
    lv_obj_set_size(u.haltBtn, 220, 52);
    lv_obj_align(u.haltBtn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(u.haltBtn, lv_color_hex(kRed), 0);
    u.haltLbl = label(u.haltBtn, "HALT", &lv_font_montserrat_20);
    lv_obj_center(u.haltLbl);
    lv_obj_add_event_cb(u.haltBtn, haltEvent, LV_EVENT_ALL, nullptr);
}

void refreshStrip() {
    Session& s = *u.session;
    const bool live = s.live();
    const bool stale = s.linkStale();
    const char* stopName = s.estopCutsPower() ? "E-STOP" : "HALT";
    char buf[64];
    if (s.haltUnconfirmed()) {
        std::snprintf(buf, sizeof(buf), "%s NOT CONFIRMED", stopName);
        lv_obj_set_style_bg_color(u.haltBtn, lv_color_hex(kAmber), 0);
    } else if (s.estopLatched()) {
        const uint32_t held = u.releaseArmed ? nowMs() - u.haltPressMs : 0;
        if (u.releaseArmed && held < kReleaseHoldMs) {
            std::snprintf(buf, sizeof(buf), "Releasing... %u", unsigned((kReleaseHoldMs - held + 999) / 1000));
        } else {
            std::snprintf(buf, sizeof(buf), "%sED\nhold 3 s: release", s.estopCutsPower() ? "E-STOPP" : "HALT");
        }
        lv_obj_set_style_bg_color(u.haltBtn, lv_color_hex(kRedDark), 0);
    } else {
        std::snprintf(buf, sizeof(buf), "%s", stopName);
        lv_obj_set_style_bg_color(u.haltBtn, lv_color_hex(kRed), 0);
    }
    lv_label_set_text(u.haltLbl, buf);

    const bool paused = s.pauseLatched();
    lv_label_set_text(u.pauseLbl, paused ? "Resume" : "Pause");
    setEnabled(u.pauseBtn, live && (!paused || s.roles() != valence::AccessLevel::watch));

    if (!live) {
        std::snprintf(buf, sizeof(buf), "LINK DOWN: %s", s.status());
    } else if (stale) {
        std::snprintf(buf, sizeof(buf), "LINK STALE: %s", s.hubName());
    } else {
        std::snprintf(buf, sizeof(buf), "%s", s.status());
    }
    const uint32_t lost = s.lostRecently();
    if (lost) {
        const size_t used = std::strlen(buf);
        std::snprintf(buf + used, sizeof(buf) - used, " | %u NOT SENT", unsigned(lost));
    }
    lv_label_set_text(u.stripStatus, buf);
    lv_obj_set_style_bg_color(u.strip, lv_color_hex(live && !stale && !lost ? kPanel : kRedDark), 0);
}

// ---- status tab ---------------------------------------------------------------

void buildStatus(lv_obj_t* tab) {
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    label(tab, "Position", &lv_font_montserrat_14, kDim);
    u.posLbl = label(tab, "--", &lv_font_montserrat_28);
    u.posBar = lv_bar_create(tab);
    lv_obj_set_size(u.posBar, 740, 28);
    u.safetyLbl = label(tab, "");
    u.roleLbl = label(tab, "", &lv_font_montserrat_14, kDim);
}

void refreshStatus() {
    Session& s = *u.session;
    const Readout* r = s.model().position();
    const auto v = r ? s.model().value(*r) : std::nullopt;
    if (v) {
        lv_label_set_text_fmt(u.posLbl, "%.1f %.*s", double(*v), int(r->lf->unit.size()), r->lf->unit.data());
        const float lo = r->lf->hasMin ? r->lf->min : 0.0f;
        const float hi = r->lf->hasMax ? r->lf->max : 100.0f;
        lv_bar_set_range(u.posBar, int32_t(lo), int32_t(hi > lo ? hi : lo + 1));
        lv_bar_set_value(u.posBar, int32_t(*v), LV_ANIM_OFF);
    } else {
        lv_label_set_text(u.posLbl, r ? "--" : "no position in this catalog");
    }
    if (!s.live()) {
        lv_label_set_text(u.safetyLbl, "No session: Halt still goes out as a datagram.");
    } else if (s.estopLatched()) {
        lv_label_set_text(u.safetyLbl, "Halt latched. Hold the Halt button 3 s to release.");
    } else if (s.pauseLatched()) {
        lv_label_set_text(u.safetyLbl, "Paused.");
    } else {
        lv_label_set_text(u.safetyLbl, "No stop latched.");
    }
    const bool watch = s.roles() == valence::AccessLevel::watch;
    lv_label_set_text_fmt(u.roleLbl, "Access: %s%s", watch ? "watch" : (s.roles() == valence::AccessLevel::control ? "control" : "configure"),
                          watch ? " (Halt and Pause only; request control in Setup)" : "");
}

// ---- controls tab ---------------------------------------------------------------

int32_t toTicks(const Setting& s, float v) { return int32_t(std::lround((v - s.min()) / s.step())); }
float fromTicks(const Setting& s, int32_t t) { return s.min() + float(t) * s.step(); }

void rowEvent(lv_event_t* e) {
    auto* row = static_cast<Row*>(lv_event_get_user_data(e));
    lv_obj_t* w = row->widget;
    const Setting& s = *row->s;
    if (lv_obj_check_type(w, &lv_slider_class)) {
        u.session->write(s, fromTicks(s, lv_slider_get_value(w)));
    } else if (lv_obj_check_type(w, &lv_switch_class)) {
        u.session->write(s, lv_obj_has_state(w, LV_STATE_CHECKED) ? 1.0f : 0.0f);
    } else if (lv_obj_check_type(w, &lv_dropdown_class)) {
        u.session->write(s, float(lv_dropdown_get_selected(w)));
    }
}

void rebuildControls() {
    lv_obj_clean(u.controls);
    u.rows.clear();
    Model& m = u.session->model();
    u.modelGen = m.generation();
    if (!m.loaded()) {
        label(u.controls, "Waiting for the hub's catalog.", &lv_font_montserrat_20, kDim);
        return;
    }
    u.rows.reserve(m.settings().size());
    for (const Setting& s : m.settings()) {
        lv_obj_t* row = lv_obj_create(u.controls);
        lv_obj_set_size(row, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(row, lv_color_hex(kPanel), 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* name = label(row, "", &lv_font_montserrat_20);
        lv_label_set_text_fmt(name, "%.*s", int(s.name().size()), s.name().data());
        lv_obj_set_width(name, 250);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        Row r{&s, nullptr, nullptr};
        if (s.isToggle()) {
            r.widget = lv_switch_create(row);
            lv_obj_align(r.widget, LV_ALIGN_LEFT_MID, 280, 0);
            lv_obj_add_event_cb(r.widget, rowEvent, LV_EVENT_VALUE_CHANGED, nullptr);
        } else if (s.hasOptions()) {
            r.widget = lv_dropdown_create(row);
            std::string opts;
            for (size_t i = 0; i < s.sf->options.count; ++i) {
                if (i) opts += '\n';
                opts += m.optionLabel(s, i);
            }
            lv_dropdown_set_options(r.widget, opts.c_str());
            lv_obj_set_width(r.widget, 300);
            lv_obj_align(r.widget, LV_ALIGN_LEFT_MID, 280, 0);
            lv_obj_add_event_cb(r.widget, rowEvent, LV_EVENT_VALUE_CHANGED, nullptr);
        } else {
            r.widget = lv_slider_create(row);
            lv_slider_set_range(r.widget, 0, toTicks(s, s.max()));
            lv_obj_set_width(r.widget, 300);
            lv_obj_align(r.widget, LV_ALIGN_LEFT_MID, 290, 0);
            lv_obj_add_event_cb(r.widget, rowEvent, LV_EVENT_RELEASED, nullptr);
        }
        lv_obj_set_style_opa(r.widget, LV_OPA_40, LV_STATE_DISABLED);
        r.value = label(row, "--", &lv_font_montserrat_20, kDim);
        lv_obj_align(r.value, LV_ALIGN_RIGHT_MID, 0, 0);
        u.rows.push_back(r);
    }
    // user_data points into the vector: set only once it stops growing.
    for (Row& r : u.rows) {
        lv_obj_remove_event_cb(r.widget, rowEvent);
        const lv_event_code_t code = lv_obj_check_type(r.widget, &lv_slider_class) ? LV_EVENT_RELEASED : LV_EVENT_VALUE_CHANGED;
        lv_obj_add_event_cb(r.widget, rowEvent, code, &r);
    }
    ESP_LOGI(kTag, "controls: %u rows from the catalog", unsigned(u.rows.size()));
}

void refreshControls() {
    Session& s = *u.session;
    if (s.model().generation() != u.modelGen) rebuildControls();
    for (Row& r : u.rows) {
        const bool on = s.live() && s.model().enabled(*r.s, s.roles());
        setEnabled(r.widget, on);
        const auto v = s.model().value(*r.s);
        if (!v) {
            lv_label_set_text(r.value, "--");
            continue;
        }
        const auto unit = r.s->lf->unit;
        lv_label_set_text_fmt(r.value, "%.*f %.*s", r.s->step() < 1.0f ? 2 : 0, double(*v), int(unit.size()), unit.data());
        if (lv_obj_has_state(r.widget, LV_STATE_PRESSED)) continue;  // the user is dragging it
        if (lv_obj_check_type(r.widget, &lv_slider_class)) {
            lv_slider_set_value(r.widget, toTicks(*r.s, *v), LV_ANIM_OFF);
        } else if (lv_obj_check_type(r.widget, &lv_switch_class)) {
            if (*v >= 0.5f) {
                lv_obj_add_state(r.widget, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(r.widget, LV_STATE_CHECKED);
            }
        } else if (lv_obj_check_type(r.widget, &lv_dropdown_class) && !lv_dropdown_is_open(r.widget)) {
            lv_dropdown_set_selected(r.widget, uint32_t(std::lround(*v)));
        }
    }
}

// ---- setup tab ------------------------------------------------------------------

void forgetWifi(lv_event_t*) { wifi::forget(); }
void forgetHub(lv_event_t*) { u.session->forgetHub(); }
void knock(lv_event_t*) { u.session->knock(); }
void toggleLink(lv_event_t*) {
    u.session->setMode(u.session->mode() == LinkMode::Wifi ? LinkMode::Tether : LinkMode::Wifi);
}

void buildSetup(lv_obj_t* tab) {
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    u.wifiLbl = label(tab, "");
    lv_obj_t* row = lv_obj_create(tab);
    lv_obj_set_size(row, LV_PCT(100), 80);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    u.knockBtn = button(row, "Request control", knock);
    button(row, "Forget hub", forgetHub);
    button(row, "Forget WiFi", forgetWifi);
    button(row, "Link: WiFi", toggleLink, &u.linkBtnLbl);
}

void refreshSetup() {
    Session& s = *u.session;
    switch (wifi::state()) {
        case wifi::State::Portal:
            lv_label_set_text_fmt(u.wifiLbl, "WiFi setup: join \"%s\", password %s,\nthen open http://192.168.4.1",
                                  wifi::portal_ssid(), wifi::portal_pass());
            break;
        case wifi::State::Connected: {
            const uint32_t ip = wifi::ipv4();
            lv_label_set_text_fmt(u.wifiLbl, "WiFi: %s, %u.%u.%u.%u", wifi::sta_ssid(), unsigned(ip & 0xFF),
                                  unsigned((ip >> 8) & 0xFF), unsigned((ip >> 16) & 0xFF), unsigned(ip >> 24));
            break;
        }
        default:
            lv_label_set_text_fmt(u.wifiLbl, "WiFi: joining \"%s\"", wifi::sta_ssid());
    }
    lv_label_set_text(u.linkBtnLbl, s.mode() == LinkMode::Wifi ? "Link: WiFi" : "Link: tether");
    setEnabled(u.knockBtn, s.live() && s.roles() == valence::AccessLevel::watch);
}

// ---- map tab --------------------------------------------------------------------

std::string actionLabel(const std::string& id) {
    for (const auto& a : kFixedActions) {
        if (id == a.id) return a.label;
    }
    return id;
}

void setStep(MapStep st) {
    u.step = st;
    const bool pick = st == MapStep::Pick;
    const bool busy = st != MapStep::Idle;
    if (pick) {
        lv_obj_remove_flag(u.mapPick, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(u.mapSave, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(u.mapPick, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(u.mapSave, LV_OBJ_FLAG_HIDDEN);
    }
    if (busy) {
        lv_obj_remove_flag(u.mapSkip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(u.mapStart, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(u.mapSkip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(u.mapStart, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < u.inputs.size(); ++i) {
        const bool hot = busy && i == u.mapIndex;
        lv_obj_set_style_border_color(u.inputs[i].box, lv_color_hex(hot ? 0xFFFF00 : kAmber), 0);
        lv_obj_set_style_border_width(u.inputs[i].box, hot ? 5 : 1, 0);
    }
    if (st == MapStep::WaitPress) {
        lv_label_set_text_fmt(u.mapText, "Press %s on the board to confirm it.", u.inputs[u.mapIndex].label.c_str());
    } else if (pick) {
        lv_label_set_text_fmt(u.mapText, "%s confirmed. Pick its action:", u.inputs[u.mapIndex].label.c_str());
    } else if (u.inputs.empty()) {
        lv_label_set_text(u.mapText, "This board has no physical inputs: the touchscreen is the input.");
    } else {
        lv_label_set_text(u.mapText, "The on-screen Halt is always there; a mapping can only add more.");
    }
    refreshMapList();
}

void fillPicker() {
    u.pickIds.clear();
    std::string opts;
    for (const auto& a : kFixedActions) {
        u.pickIds.emplace_back(a.id);
        opts += a.label;
        opts += '\n';
    }
    char id[32];
    for (const Setting& s : u.session->model().settings()) {
        const std::string name(s.name());
        const char* ops[] = {"inc", "dec", "toggle"};
        for (const char* op : ops) {
            if ((std::string_view(op) == "toggle") != s.isToggle()) continue;
            std::snprintf(id, sizeof(id), "%s:%04x.%u", op, unsigned(s.intentCh), unsigned(s.sf->key));
            u.pickIds.emplace_back(id);
            opts += std::string(op) + " " + name + '\n';
        }
    }
    if (!opts.empty()) opts.pop_back();
    lv_dropdown_set_options(u.mapPick, opts.c_str());
    const std::string cur = u.mapping->action(u.inputs[u.mapIndex].id);
    for (size_t i = 0; i < u.pickIds.size(); ++i) {
        if (u.pickIds[i] == cur) lv_dropdown_set_selected(u.mapPick, uint32_t(i));
    }
}

void nextInput() {
    if (++u.mapIndex >= u.inputs.size()) {
        setStep(MapStep::Idle);
        ESP_LOGI(kTag, "mapping flow done: %s", u.mapping->toJson().c_str());
        return;
    }
    setStep(MapStep::WaitPress);
}

void mapStartEvent(lv_event_t*) {
    if (u.inputs.empty()) return;
    u.mapIndex = 0;
    setStep(MapStep::WaitPress);
}

void mapSaveEvent(lv_event_t*) {
    const uint32_t sel = lv_dropdown_get_selected(u.mapPick);
    if (sel < u.pickIds.size()) {
        u.mapping->set(u.inputs[u.mapIndex].id, u.pickIds[sel]);
        u.mapping->save();
        ESP_LOGI(kTag, "mapped %s -> %s", u.inputs[u.mapIndex].id.c_str(), u.pickIds[sel].c_str());
    }
    nextInput();
}

void mapSkipEvent(lv_event_t*) { nextInput(); }

void refreshMapList() {
    std::string txt;
    for (const auto& in : u.inputs) {
        txt += in.label + ": " + actionLabel(u.mapping->action(in.id)) + "\n";
    }
    txt += "Touchscreen: the on-screen controls";
    lv_label_set_text(u.mapList, txt.c_str());
}

void buildMap(lv_obj_t* tab) {
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    cJSON* m = cJSON_ParseWithLength(bsp::manifest().data(), bsp::manifest().size());
    const cJSON* img = cJSON_GetObjectItem(m, "image");
    const float iw = img ? float(cJSON_GetObjectItem(img, "width")->valuedouble) : 1.0f;
    const float ih = img ? float(cJSON_GetObjectItem(img, "height")->valuedouble) : 1.0f;
    constexpr float kW = 420.0f, kH = 270.0f;
    const float k = std::fmin(kW / iw, kH / ih);
    lv_obj_t* board = lv_obj_create(tab);
    lv_obj_set_size(board, int32_t(iw * k) + 8, int32_t(ih * k) + 8);
    lv_obj_set_style_pad_all(board, 0, 0);
    lv_obj_set_style_bg_opa(board, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(board, 0, 0);
    lv_obj_remove_flag(board, LV_OBJ_FLAG_SCROLLABLE);
    auto rect = [&](const cJSON* r, uint32_t fill, lv_opa_t opa, uint32_t border) {
        lv_obj_t* o = lv_obj_create(board);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(o, int32_t(cJSON_GetObjectItem(r, "x")->valuedouble * k),
                       int32_t(cJSON_GetObjectItem(r, "y")->valuedouble * k));
        lv_obj_set_size(o, int32_t(cJSON_GetObjectItem(r, "w")->valuedouble * k) + 1,
                        int32_t(cJSON_GetObjectItem(r, "h")->valuedouble * k) + 1);
        lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
        lv_obj_set_style_bg_opa(o, opa, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_radius(o, 3, 0);
        lv_obj_set_style_pad_all(o, 0, 0);
        return o;
    };
    if (const cJSON* o = cJSON_GetObjectItem(m, "outline")) rect(o, 0x1D2A22, LV_OPA_COVER, 0x6A7A70);
    if (const cJSON* s = cJSON_GetObjectItem(m, "screen")) rect(s, 0x05070A, LV_OPA_COVER, 0x3A4450);
    const cJSON* feats = cJSON_GetObjectItem(m, "features");
    for (const cJSON* f = feats ? feats->child : nullptr; f; f = f->next) rect(f, 0x2C3530, LV_OPA_COVER, 0x6A7A70);
    const cJSON* ins = cJSON_GetObjectItem(m, "inputs");
    for (const cJSON* in = ins ? ins->child : nullptr; in; in = in->next) {
        const std::string kind = cJSON_GetObjectItem(in, "kind")->valuestring;
        if (kind == "touch_zone") {
            rect(in, 0x40B070, LV_OPA_10, 0x40B070);
            continue;
        }
        Input x{cJSON_GetObjectItem(in, "id")->valuestring, cJSON_GetObjectItem(in, "label")->valuestring, kind};
        x.box = rect(in, kAmber, LV_OPA_COVER, kAmber);
        lv_obj_t* tag = label(board, x.label.c_str(), &lv_font_montserrat_14, kAmber);
        lv_obj_align_to(tag, x.box, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
        u.inputs.push_back(std::move(x));
    }
    cJSON_Delete(m);

    lv_obj_t* side = lv_obj_create(tab);
    lv_obj_set_size(side, 330, 300);
    lv_obj_align(side, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(side, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(side, 0, 0);
    u.mapText = label(side, "", &lv_font_montserrat_20);
    lv_obj_set_width(u.mapText, 310);
    lv_label_set_long_mode(u.mapText, LV_LABEL_LONG_WRAP);
    u.mapPick = lv_dropdown_create(side);
    lv_obj_set_width(u.mapPick, 300);
    u.mapSave = button(side, "Save", mapSaveEvent);
    u.mapSkip = button(side, "Skip", mapSkipEvent);
    u.mapStart = button(side, "Map inputs", mapStartEvent);
    setEnabled(u.mapStart, !u.inputs.empty());
    u.mapList = label(tab, "", &lv_font_montserrat_14, kDim);
    lv_obj_align(u.mapList, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    setStep(MapStep::Idle);
}

// ---- physical inputs ------------------------------------------------------------

void onPress(size_t i) {
    if (i >= u.inputs.size()) return;
    const std::string act = u.mapping->action(u.inputs[i].id);
    ESP_LOGI(kTag, "input %s pressed (action %s)", u.inputs[i].id.c_str(), act.c_str());
    if (act == "halt") u.session->halt();  // a mapped Halt works during the flow too
    if (u.step == MapStep::WaitPress) {
        if (i == u.mapIndex) {
            fillPicker();
            setStep(MapStep::Pick);
        } else {
            lv_label_set_text_fmt(u.mapText, "That was %s. Press %s.", u.inputs[i].label.c_str(),
                                  u.inputs[u.mapIndex].label.c_str());
        }
        return;
    }
    if (u.step == MapStep::Pick || act == "halt" || act == "none") return;
    if (act == "menu+" || act == "menu-") {
        const uint32_t n = lv_tabview_get_tab_count(u.tabs);
        const uint32_t cur = lv_tabview_get_tab_active(u.tabs);
        lv_tabview_set_active(u.tabs, act == "menu+" ? (cur + 1) % n : (cur + n - 1) % n, LV_ANIM_OFF);
        return;
    }
    u.session->runAction(act);
}

}  // namespace

void build(Session& session, Mapping& mapping) {
    u.session = &session;
    u.mapping = &mapping;
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05080B), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(kText), 0);

    u.tabs = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(u.tabs, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(u.tabs, 52);
    lv_obj_set_size(u.tabs, bsp::caps().width, bsp::caps().height - kStripH);
    lv_obj_set_pos(u.tabs, 0, kStripH);
    lv_obj_set_style_bg_color(u.tabs, lv_color_hex(0x05080B), 0);
    buildStatus(lv_tabview_add_tab(u.tabs, "Status"));
    u.controls = lv_tabview_add_tab(u.tabs, "Controls");
    lv_obj_set_flex_flow(u.controls, LV_FLEX_FLOW_COLUMN);
    buildMap(lv_tabview_add_tab(u.tabs, "Map"));
    buildSetup(lv_tabview_add_tab(u.tabs, "Setup"));
    rebuildControls();

    buildStrip();  // last, on the top layer: above everything built here
}

void tick(uint32_t buttons) {
    const uint32_t edges = buttons & ~u.lastButtons;
    u.lastButtons = buttons;
    for (size_t i = 0; i < 32; ++i) {
        if (edges & (1u << i)) onPress(i);
    }
    const uint32_t t = nowMs();
    if (t - u.lastRefreshMs >= kRefreshMs) {
        u.lastRefreshMs = t;
        refreshStrip();
        refreshStatus();
        refreshControls();
        refreshSetup();
    }
}

void injectPress(int index) {
    if (index >= 0) onPress(size_t(index));
}

void benchCommand(const char* verb, const char* arg) {
    const std::string v = verb ? verb : "";
    if (v == "tab" && arg) {
        lv_tabview_set_active(u.tabs, uint32_t(std::atoi(arg)), LV_ANIM_OFF);
    } else if (v == "mapstart") {
        mapStartEvent(nullptr);
    } else if (v == "mappick" && arg && u.step == MapStep::Pick) {
        for (size_t i = 0; i < u.pickIds.size(); ++i) {
            if (u.pickIds[i] == arg) lv_dropdown_set_selected(u.mapPick, uint32_t(i));
        }
        mapSaveEvent(nullptr);
    }
    ESP_LOGI(kTag, "bench %s %s: map step %d", verb ? verb : "", arg ? arg : "", int(u.step));
}

}  // namespace garnet::ui
