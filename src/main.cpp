// Garnet: an embedded Valence remote. One UI task owns LVGL, the Session (and
// through it the Client) and the input scan; WiFi, the WebSocket client and
// the console reader run on their own tasks and reach the UI task only
// through queues (link/link.h, app/console.h).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/console.h"
#include "app/mapping.h"
#include "app/session.h"
#include "bsp/bsp.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lwip/inet.h"
#include "mbedtls/base64.h"
#include "net/wifi.h"
#include "nvs_flash.h"
#include "valence/net/saved_networks.hpp"
#include "ui/ui.h"

namespace {

constexpr const char* kTag = "garnet";

struct App {
    garnet::Session* session = nullptr;
    garnet::Mapping* mapping = nullptr;
    char cmd[garnet::console::kLineMax]{};
};
App app;

void shotRow(void*, int y, const uint16_t* px, int width) {
    unsigned char b64[2200];
    size_t n = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &n, reinterpret_cast<const unsigned char*>(px), size_t(width) * 2);
    std::printf("@S %d %d %.*s\n", y, width, int(n), reinterpret_cast<const char*>(b64));
    vTaskDelay(pdMS_TO_TICKS(2));  // the USB driver drops what its TX buffer cannot hold
}

// Splits off the next token; a token may be double-quoted to hold spaces.
char* token(char*& p) {
    while (*p == ' ') ++p;
    if (!*p) return nullptr;
    char* t = p;
    if (*p == '"') {
        t = ++p;
        while (*p && *p != '"') ++p;
    } else {
        while (*p && *p != ' ') ++p;
    }
    if (*p) *p++ = '\0';
    return t;
}

void command(char* line) {
    garnet::Session& s = *app.session;
    char* p = line;
    const char* verb = token(p);
    if (!verb) return;
    const auto is = [&](const char* v) { return std::strcmp(verb, v) == 0; };
    if (is("status")) {
        const auto w = s.live() ? (s.estopLatched() ? "HALT latched" : (s.pauseLatched() ? "paused" : "clear")) : "no session";
        std::printf("status: %s | session %d roles %d | safety %s | link %s | catalog %s, %u settings\n", s.status(),
                    int(s.state()), int(s.roles()), w, s.mode() == garnet::LinkMode::Wifi ? "wifi" : "tether",
                    s.model().loaded() ? "loaded" : "none", unsigned(s.model().settings().size()));
    } else if (is("catalog")) {
        for (const auto& st : s.model().settings()) {
            const auto v = s.model().value(st);
            std::printf("  0x%04x.%u %.*s = %s%.2f [%g..%g step %g]%s\n", unsigned(st.intentCh), unsigned(st.sf->key),
                        int(st.name().size()), st.name().data(), v ? "" : "(none) ", double(v.value_or(0)),
                        double(st.min()), double(st.max()), double(st.step()),
                        s.model().enabled(st, s.roles()) ? "" : " (disabled)");
        }
    } else if (is("linkstat")) {
        s.printLinkStats();
    } else if (is("stallsync")) {
        const char* ms = token(p);
        s.stallOnSync(uint32_t(ms ? std::atoi(ms) : 3000));
    } else if (is("catdump")) {
        const auto b = s.model().bytes();
        for (size_t off = 0; off < b.size(); off += 768) {
            unsigned char b64[1040];
            size_t n = 0;
            const size_t len = b.size() - off < 768 ? b.size() - off : 768;
            mbedtls_base64_encode(b64, sizeof(b64), &n, reinterpret_cast<const unsigned char*>(b.data() + off), len);
            std::printf("@C %.*s\n", int(n), reinterpret_cast<const char*>(b64));
        }
        std::printf("@C end %u\n", unsigned(b.size()));
    } else if (is("wifi")) {
        const char* ssid = token(p);
        const char* pass = token(p);
        std::printf("wifi: %s\n", !ssid ? "usage: wifi <ssid> [pass]"
                                  : garnet::wifi::add(ssid, pass ? pass : "") ? "saved" : "not saved (invalid, or 4 already saved)");
    } else if (is("wifi-list")) {
        std::printf("wifi: %d of %d saved, live slot %d\n", garnet::wifi::count(), garnet::wifi::kMaxNetworks,
                    garnet::wifi::live() + 1);
        for (int i = 0; i < garnet::wifi::count(); ++i) {
            std::printf("  slot %d%s\n", i + 1, i == garnet::wifi::live() ? " (live)" : "");
        }
    } else if (is("wifi-rm")) {
        const char* n = token(p);
        std::printf("wifi-rm: %s\n", n && garnet::wifi::remove(std::atoi(n) - 1) ? "removed" : "usage: wifi-rm <slot>");
    } else if (is("wifi-mv")) {
        const char* a = token(p);
        const char* b = token(p);
        std::printf("wifi-mv: %s\n", a && b && garnet::wifi::move(std::atoi(a) - 1, std::atoi(b) - 1) ? "moved" : "usage: wifi-mv <from> <to>");
    } else if (is("forget-wifi")) {
        garnet::wifi::forget_all();
    } else if (is("hub")) {
        const char* ip = token(p);
        const char* port = token(p);
        const uint32_t a = ip ? inet_addr(ip) : 0;
        s.connectTo(a, uint16_t(port ? std::atoi(port) : 82));
    } else if (is("forget-hub")) {
        s.forgetHub();
    } else if (is("link")) {
        const char* m = token(p);
        if (m) s.setMode(std::strcmp(m, "tether") == 0 ? garnet::LinkMode::Tether : garnet::LinkMode::Wifi);
    } else if (is("halt")) {
        s.halt();
    } else if (is("pause")) {
        s.pauseToggle();
    } else if (is("release")) {
        s.releaseHalt();
    } else if (is("knock")) {
        s.knock();
    } else if (is("askconfigure")) {
        s.askConfigure();
    } else if (is("press")) {
        const char* i = token(p);
        garnet::ui::injectPress(i ? std::atoi(i) : 0);
    } else if (is("tab") || is("mapstart") || is("mappick")) {
        garnet::ui::benchCommand(verb, token(p));
    } else if (is("map")) {
        const char* sub = token(p);
        if (sub && std::strcmp(sub, "import") == 0) {
            const bool ok = app.mapping->fromJson(p) && app.mapping->save();
            std::printf("map import: %s\n", ok ? "saved" : "bad JSON");
        } else if (sub && std::strcmp(sub, "clear") == 0) {
            app.mapping->clear();
            app.mapping->save();
        }
        std::printf("map: %s\n", app.mapping->toJson().c_str());
    } else if (is("shot")) {
        std::printf("@S begin %u %u\n", unsigned(garnet::bsp::caps().width), unsigned(garnet::bsp::caps().height));
        garnet::bsp::screenshot(shotRow, nullptr);
        std::printf("@S end\n");
    } else if (is("help")) {
        std::printf("commands: status, catalog, wifi <ssid> [pass], wifi-list, wifi-rm <n>, wifi-mv <from> <to>,\n"
                    "  forget-wifi, hub <ip> [port], forget-hub,\n"
                    "  link wifi|tether, halt, pause, release, knock, press <input>, map [import <json>|clear], shot,\n"
                    "  tab <n>, mapstart, mappick <action>, catdump\n");
    } else {
        std::printf("unknown command \"%.24s\" (help lists them)\n", verb);
    }
}

void uiTask(void*) {
    lv_init();
    garnet::bsp::init();
    app.mapping = new garnet::Mapping();
    if (!app.mapping->load()) {
        // Nothing stored yet: the board's declared defaults, unsaved, so a
        // firmware update can still change them until the user saves a map.
        cJSON* m = cJSON_ParseWithLength(garnet::bsp::manifest().data(), garnet::bsp::manifest().size());
        if (char* d = cJSON_PrintUnformatted(cJSON_GetObjectItem(m, "default_mapping"))) {
            app.mapping->fromJson(d);
            cJSON_free(d);
        }
        cJSON_Delete(m);
    }
    app.session = new garnet::Session();
    app.session->begin();
    garnet::ui::build(*app.session, *app.mapping);
    garnet::console::start(app.session->tether());
    ESP_LOGI(kTag, "up: free internal %u B, PSRAM %u B", unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    int64_t lastScanUs = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now - lastScanUs >= 10000) {
            lastScanUs = now;
            garnet::ui::tick(garnet::bsp::buttons());
        }
        app.session->tick(uint32_t(now));
        while (garnet::console::next(app.cmd)) {
            command(app.cmd);
            valence::net::wipeBytes(app.cmd, sizeof app.cmd);  // a wifi line holds a passphrase
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    garnet::wifi::start();
    xTaskCreatePinnedToCore(uiTask, "ui", 16384, nullptr, 5, nullptr, 1);
}
