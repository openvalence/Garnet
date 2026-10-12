// M5Stack CoreS3 SE: ILI9342C 320x240 on SPI, FT6336 touch, AXP2101 power,
// AW9523 IO expander, all but the panel on I2C (SDA 12, SCL 11).
// Pins, registers and the panel init list: M5GFX (src/M5GFX.cpp CoreS3 probe,
// lgfx Panel_ILI9342) and M5Unified (Power_Class CoreS3 setup); docs/DESIGN.md
// has the board section.
//
// Constraints:
// - GPIO35 is both the panel's D/C and the SPI MISO (M5 switches it per CS).
//   Garnet never reads the panel or an SD card, so it is D/C only.
// - The panel reset is AW9523 P1_1 and the backlight is the AXP2101 DLDO1
//   rail; touch has no GPIO of its own (its INT reaches GPIO21 through the
//   AW9523), so touch is polled.
// - The power key reaches only the AXP2101. A short press is read from its IRQ
//   status (0x49, write-1-to-clear); the hold-to-power-off stays in hardware
//   and is never reconfigured here.
// - No vibration motor on the CoreS3, and the speaker needs the AW88298 amp
//   over I2S: no click yet.
#include "bsp/bsp.h"

#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

extern const char manifest_json_start[] asm("_binary_manifest_json_start");
extern const char manifest_json_end[] asm("_binary_manifest_json_end");

namespace garnet::bsp {
namespace {

constexpr const char* kTag = "bsp";
constexpr uint16_t kWidth = 320;
constexpr uint16_t kHeight = 240;
constexpr int kDrawLines = 24;
constexpr uint8_t kAxp = 0x34;
constexpr uint8_t kAw = 0x58;
constexpr uint8_t kTouch = 0x38;
constexpr uint32_t kPowerPollMs = 50;

const Caps kCaps{kWidth, kHeight, 1};

struct Board {
    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_dev_handle_t axp = nullptr;
    i2c_master_dev_handle_t aw = nullptr;
    i2c_master_dev_handle_t touch = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    lv_display_t* disp = nullptr;
    uint16_t* shadow = nullptr;  // PSRAM copy of the screen, for screenshot()
    uint32_t lastPowerPollMs = 0;
    bool powerPulse = false;
};
Board g;

uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }

bool regWrite(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v) {
    const uint8_t b[2] = {reg, v};
    return i2c_master_transmit(d, b, 2, 50) == ESP_OK;
}

bool regRead(i2c_master_dev_handle_t d, uint8_t reg, uint8_t* out, size_t n) {
    return i2c_master_transmit_receive(d, &reg, 1, out, n, 50) == ESP_OK;
}

void regBits(i2c_master_dev_handle_t d, uint8_t reg, uint8_t set, uint8_t clear) {
    uint8_t v = 0;
    if (regRead(d, reg, &v, 1)) regWrite(d, reg, uint8_t((v & ~clear) | set));
}

i2c_master_dev_handle_t addDevice(uint8_t addr) {
    i2c_device_config_t dc{};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = 400000;
    i2c_master_dev_handle_t h = nullptr;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(g.bus, &dc, &h));
    return h;
}

// M5's CoreS3 power-up, minus the camera, mic and speaker rails' consumers.
void initPower() {
    i2c_master_bus_config_t bc{};
    bc.i2c_port = I2C_NUM_1;
    bc.sda_io_num = GPIO_NUM_12;
    bc.scl_io_num = GPIO_NUM_11;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &g.bus));
    g.axp = addDevice(kAxp);
    g.aw = addDevice(kAw);
    g.touch = addDevice(kTouch);

    regBits(g.aw, 0x02, 0b00000101, 0);  // P0 levels: touch reset high, bus enables
    regBits(g.aw, 0x03, 0b10000011, 0);  // P1 levels: LCD reset high, boost enable
    regWrite(g.aw, 0x04, 0b00011000);    // P0 direction (1 = input)
    regWrite(g.aw, 0x05, 0b00001100);    // P1 direction
    regWrite(g.aw, 0x11, 0b00010000);    // P0 push-pull
    regWrite(g.aw, 0x12, 0xFF);          // GPIO mode, not LED
    regWrite(g.aw, 0x13, 0xFF);

    regBits(g.axp, 0x23, 0, 0x1F);  // a DCDC dip resets the SoC instead of powering off
    regWrite(g.axp, 0x90, 0xBF);    // LDOs on, DLDO1 (backlight) included
    regWrite(g.axp, 0x95, 33 - 5);  // ALDO4 3.3 V (TF slot)
    regWrite(g.axp, 0x99, 28);      // DLDO1 = backlight level, full
    regWrite(g.axp, 0x49, 0x0C);    // clear stale power-key events
}

void lcdReset() {
    regBits(g.aw, 0x03, 0, 0b10);  // P1_1 low
    vTaskDelay(pdMS_TO_TICKS(10));
    regBits(g.aw, 0x03, 0b10, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

void cmd(uint8_t c, std::initializer_list<uint8_t> p = {}) {
    esp_lcd_panel_io_tx_param(g.io, c, p.size() ? p.begin() : nullptr, p.size());
}

bool flushDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    lv_display_flush_ready(g.disp);
    return false;
}

void initPanel() {
    spi_bus_config_t bus{};
    bus.mosi_io_num = GPIO_NUM_37;
    bus.miso_io_num = -1;
    bus.sclk_io_num = GPIO_NUM_36;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kWidth * kDrawLines * 2 + 8;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io{};
    io.cs_gpio_num = GPIO_NUM_3;
    io.dc_gpio_num = GPIO_NUM_35;
    io.spi_mode = 0;
    io.pclk_hz = 40 * 1000 * 1000;
    io.trans_queue_depth = 4;
    io.on_color_trans_done = flushDone;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io, &g.io));

    lcdReset();
    // lgfx Panel_ILI9342 list0, then pixel format, MADCTL and inversion as
    // M5GFX leaves them for this board (rotation 1 + offset 3, BGR, invert on).
    cmd(0xC8, {0xFF, 0x93, 0x42});
    cmd(0xC0, {0x12, 0x12});
    cmd(0xC1, {0x03});
    cmd(0xC5, {0xF2});
    cmd(0xB0, {0xE0});
    cmd(0xF6, {0x01, 0x00, 0x00});
    cmd(0xE0, {0x00, 0x0C, 0x11, 0x04, 0x11, 0x08, 0x37, 0x89, 0x4C, 0x06, 0x0C, 0x0A, 0x2E, 0x34, 0x0F});
    cmd(0xE1, {0x00, 0x0B, 0x11, 0x05, 0x13, 0x09, 0x33, 0x67, 0x48, 0x07, 0x0E, 0x0B, 0x2E, 0x33, 0x0F});
    cmd(0xB6, {0x08, 0x82, 0x1D, 0x04});
    cmd(0x38);  // idle mode off
    cmd(0x3A, {0x55});
    cmd(0x36, {0x08});
    cmd(0x21);  // inversion on
    cmd(0x11);  // sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x29);  // display on
}

void flushCb(lv_display_t* disp, const lv_area_t* a, uint8_t* px) {
    const int w = a->x2 - a->x1 + 1;
    const int h = a->y2 - a->y1 + 1;
    auto* p = reinterpret_cast<uint16_t*>(px);
    for (int y = 0; y < h; ++y) {
        std::memcpy(g.shadow + size_t(a->y1 + y) * kWidth + a->x1, p + size_t(y) * w, size_t(w) * 2);
    }
    lv_draw_sw_rgb565_swap(px, uint32_t(w * h));  // the panel takes big-endian RGB565
    cmd(0x2A, {uint8_t(a->x1 >> 8), uint8_t(a->x1), uint8_t(a->x2 >> 8), uint8_t(a->x2)});
    cmd(0x2B, {uint8_t(a->y1 >> 8), uint8_t(a->y1), uint8_t(a->y2 >> 8), uint8_t(a->y2)});
    esp_lcd_panel_io_tx_color(g.io, 0x2C, px, size_t(w * h) * 2);  // flushDone() releases the buffer
    (void)disp;
}

void touchCb(lv_indev_t*, lv_indev_data_t* d) {
    uint8_t r[5]{};
    const bool down = regRead(g.touch, 0x02, r, sizeof(r)) && (r[0] & 0x0F) > 0;
    d->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (down) {
        d->point.x = int32_t(((r[1] & 0x0F) << 8) | r[2]);
        d->point.y = int32_t(((r[3] & 0x0F) << 8) | r[4]);
    }
}

uint32_t tickMs() { return nowMs(); }

}  // namespace

const Caps& caps() { return kCaps; }

void init() {
    initPower();
    initPanel();
    g.shadow = static_cast<uint16_t*>(heap_caps_calloc(size_t(kWidth) * kHeight, 2, MALLOC_CAP_SPIRAM));
    configASSERT(g.shadow);

    lv_tick_set_cb(tickMs);
    g.disp = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(g.disp, LV_COLOR_FORMAT_RGB565);
    const size_t bytes = size_t(kWidth) * kDrawLines * 2;
    void* b1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void* b2 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    configASSERT(b1 && b2);
    lv_display_set_buffers(g.disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g.disp, flushCb);

    lv_indev_t* in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touchCb);
    ESP_LOGI(kTag, "M5Stack CoreS3 SE: ILI9342C 320x240 @40 MHz, FT6336, AXP2101, AW9523");
}

uint32_t buttons() {
    // A power-key short press is an event, not a level: report it held for
    // one scan so the UI sees one edge.
    if (g.powerPulse) {
        g.powerPulse = false;
        return 0;
    }
    if (nowMs() - g.lastPowerPollMs < kPowerPollMs) return 0;
    g.lastPowerPollMs = nowMs();
    uint8_t irq = 0;
    if (!regRead(g.axp, 0x49, &irq, 1) || !(irq & 0x0C)) return 0;
    regWrite(g.axp, 0x49, uint8_t(irq & 0x0C));
    g.powerPulse = true;
    return 1;
}

std::string_view manifest() {
    return std::string_view(manifest_json_start, strnlen(manifest_json_start, size_t(manifest_json_end - manifest_json_start)));
}

void screenshot(void (*row)(void* ctx, int y, const uint16_t* px, int width), void* ctx) {
    for (int y = 0; y < kHeight; ++y) row(ctx, y, g.shadow + size_t(y) * kWidth, kWidth);
}

}  // namespace garnet::bsp
