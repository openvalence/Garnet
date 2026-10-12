// Waveshare ESP32-S3-Touch-LCD-4.3: 800x480 RGB565 panel (ST7262) on the
// LCD_CAM RGB bus, GT911 touch and a CH422G IO expander on I2C (SDA 8, SCL 9).
// Pins and timing: Waveshare's ESP-IDF demo 08_lvgl_Porting,
// waveshare_rgb_lcd_port.c (docs/DESIGN.md, Waveshare 4.3 section).
//
// Constraints:
// - BOOT is GPIO0, and GPIO0 is also RGB data line 6 (panel G3) with no
//   series resistor. The panel drives it push-pull at 16 MHz. So: GPIO0 runs
//   at the weakest drive (limits the current while the button shorts it),
//   every pixel is written with bit 6 set (kBootBit) so the line idles high,
//   and a press is "all samples low" across a burst, held for three scans
//   (a vertical blank is ~1 ms; three 10 ms scans never all land in one).
// - CH422G EXIO5 is USB_SEL: it MUST stay 0 or the native USB console (the
//   only port this board is wired to on the bench) disconnects.
#include "bsp/bsp.h"

#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
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
constexpr uint16_t kWidth = 800;
constexpr uint16_t kHeight = 480;
constexpr int kDrawLines = 20;
constexpr uint16_t kBootBit = 1u << 6;  // RGB565 bit 6 = data line 6 = GPIO0
constexpr gpio_num_t kBootGpio = GPIO_NUM_0;
constexpr gpio_num_t kTouchInt = GPIO_NUM_4;

// CH422G: each "address" is a command byte (CH422 datasheet).
constexpr uint8_t kChMode = 0x24;    // 0x01 = IO0-7 outputs
constexpr uint8_t kChOutput = 0x38;  // IO7..IO0 levels
constexpr uint8_t kExTpRst = 1u << 1, kExDisp = 1u << 2, kExLcdRst = 1u << 3, kExSdCs = 1u << 4;

const Caps kCaps{kWidth, kHeight, 1};

struct Board {
    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_dev_handle_t chMode = nullptr;
    i2c_master_dev_handle_t chOut = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_touch_handle_t touch = nullptr;
    uint16_t* drawBuf = nullptr;
    uint8_t bootStreak = 0;
    bool bootHeld = false;
};
Board g;

void chWrite(i2c_master_dev_handle_t dev, uint8_t v) {
    ESP_ERROR_CHECK(i2c_master_transmit(dev, &v, 1, 50));
}

void flushCb(lv_display_t* disp, const lv_area_t* a, uint8_t* px) {
    auto* p = reinterpret_cast<uint16_t*>(px);
    const size_t n = size_t(a->x2 - a->x1 + 1) * size_t(a->y2 - a->y1 + 1);
    for (size_t i = 0; i < n; ++i) p[i] |= kBootBit;
    esp_lcd_panel_draw_bitmap(g.panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, p);
    lv_display_flush_ready(disp);
}

void touchCb(lv_indev_t*, lv_indev_data_t* d) {
    esp_lcd_touch_point_data_t pt{};
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(g.touch);
    const bool down = esp_lcd_touch_get_data(g.touch, &pt, &cnt, 1) == ESP_OK && cnt > 0;
    d->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (down) {
        d->point.x = int32_t(pt.x);
        d->point.y = int32_t(pt.y);
    }
}

uint32_t tickMs() { return uint32_t(esp_timer_get_time() / 1000); }

void initExpanderAndTouchReset() {
    i2c_master_bus_config_t bc{};
    bc.i2c_port = I2C_NUM_0;
    bc.sda_io_num = GPIO_NUM_8;
    bc.scl_io_num = GPIO_NUM_9;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &g.bus));
    i2c_device_config_t dc{};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.scl_speed_hz = 400000;
    dc.device_address = kChMode;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(g.bus, &dc, &g.chMode));
    dc.device_address = kChOutput;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(g.bus, &dc, &g.chOut));

    // GT911 address select: INT low across the reset release gives 0x5D.
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << kTouchInt;
    io.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(kTouchInt, 0);
    chWrite(g.chMode, 0x01);
    chWrite(g.chOut, kExDisp | kExLcdRst | kExSdCs);  // touch held in reset, USB_SEL 0
    vTaskDelay(pdMS_TO_TICKS(100));
    chWrite(g.chOut, kExTpRst | kExDisp | kExLcdRst | kExSdCs);
    vTaskDelay(pdMS_TO_TICKS(200));
    io.mode = GPIO_MODE_INPUT;
    ESP_ERROR_CHECK(gpio_config(&io));
}

void initPanel() {
    esp_lcd_rgb_panel_config_t pc{};
    pc.clk_src = LCD_CLK_SRC_DEFAULT;
    pc.timings.pclk_hz = 16 * 1000 * 1000;
    pc.timings.h_res = kWidth;
    pc.timings.v_res = kHeight;
    pc.timings.hsync_pulse_width = 4;
    pc.timings.hsync_back_porch = 8;
    pc.timings.hsync_front_porch = 8;
    pc.timings.vsync_pulse_width = 4;
    pc.timings.vsync_back_porch = 8;
    pc.timings.vsync_front_porch = 8;
    pc.timings.flags.pclk_active_neg = 1;
    pc.data_width = 16;
    pc.bits_per_pixel = 16;
    pc.num_fbs = 1;
    pc.bounce_buffer_size_px = kWidth * 10;
    pc.dma_burst_size = 64;
    pc.hsync_gpio_num = 46;
    pc.vsync_gpio_num = 3;
    pc.de_gpio_num = 5;
    pc.pclk_gpio_num = 7;
    pc.disp_gpio_num = -1;
    const int data[16] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};
    for (int i = 0; i < 16; ++i) pc.data_gpio_nums[i] = data[i];
    pc.flags.fb_in_psram = 1;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&pc, &g.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(g.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g.panel));

    // GPIO0 stays routed to the LCD; only its pad input and drive change.
    gpio_set_drive_capability(kBootGpio, GPIO_DRIVE_CAP_0);
    gpio_input_enable(kBootGpio);

    void* fb = nullptr;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(g.panel, 1, &fb));
    auto* p = static_cast<uint16_t*>(fb);
    for (size_t i = 0; i < size_t(kWidth) * kHeight; ++i) p[i] = kBootBit;
}

void initTouch() {
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_i2c_config_t ic{};  // ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG(), whose designator order C++ rejects
    ic.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    ic.scl_speed_hz = 400000;
    ic.control_phase_bytes = 1;
    ic.dc_bit_offset = 0;
    ic.lcd_cmd_bits = 16;
    ic.flags.disable_control_phase = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(g.bus, &ic, &io));
    esp_lcd_touch_config_t tc{};
    tc.x_max = kWidth;
    tc.y_max = kHeight;
    tc.rst_gpio_num = GPIO_NUM_NC;
    tc.int_gpio_num = GPIO_NUM_NC;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(io, &tc, &g.touch));
}

}  // namespace

const Caps& caps() { return kCaps; }

void init() {
    initExpanderAndTouchReset();
    initPanel();
    initTouch();

    lv_tick_set_cb(tickMs);
    lv_display_t* d = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    const size_t bytes = size_t(kWidth) * kDrawLines * sizeof(uint16_t);
    g.drawBuf = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!g.drawBuf) g.drawBuf = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));  // slower, still correct
    configASSERT(g.drawBuf);
    lv_display_set_buffers(d, g.drawBuf, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flushCb);

    lv_indev_t* in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touchCb);
    ESP_LOGI(kTag, "Waveshare 4.3: panel 800x480 @16 MHz, GT911, CH422G USB_SEL=0");
}

uint32_t buttons() {
    bool allLow = true;
    for (int i = 0; i < 48 && allLow; ++i) allLow = gpio_get_level(kBootGpio) == 0;
    if (allLow) {
        if (g.bootStreak < 3) ++g.bootStreak;
    } else {
        g.bootStreak = 0;
    }
    g.bootHeld = g.bootStreak >= 3;
    return g.bootHeld ? 1u : 0u;
}

std::string_view manifest() {
    return std::string_view(manifest_json_start, strnlen(manifest_json_start, size_t(manifest_json_end - manifest_json_start)));
}

void screenshot(void (*row)(void* ctx, int y, const uint16_t* px, int width), void* ctx) {
    void* fb = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(g.panel, 1, &fb) != ESP_OK) return;
    const auto* p = static_cast<const uint16_t*>(fb);
    for (int y = 0; y < kHeight; ++y) row(ctx, y, p + size_t(y) * kWidth, kWidth);
}

}  // namespace garnet::bsp
