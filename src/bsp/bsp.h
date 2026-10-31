// The only board-specific surface. Each build compiles exactly one
// bsp/<board>.cpp that defines these (src/CMakeLists.txt, GARNET_BOARD);
// a missing definition fails at link time, never on the device.
// Constraints:
// - Every function here runs on the UI task only (LVGL is single-task).
// - The BSP EXPOSES inputs; it never decides what they do. Meaning comes from
//   the user's mapping (app/mapping.h) over the manifest's input ids.
#pragma once

#include <cstdint>
#include <string_view>

namespace garnet::bsp {

struct Caps {
    uint16_t width;    // landscape pixels
    uint16_t height;
    uint8_t buttons;   // physical buttons, manifest order (touch zones and encoders excluded)
    uint8_t encoders;  // rotary encoders: manifest entries of kind "encoder", in order
};

const Caps& caps();

// Panel, touch, IO expander; registers the LVGL display and pointer input.
// Call once, after lv_init().
void init();

// Bit i set = physical button i (manifest order) is held now, debounced.
// Call every 10 ms.
uint32_t buttons();

// Detents turned on encoder i since the last call, clockwise positive.
// Call every 10 ms; 0 for an index the board does not have.
int encoder_delta(int i);

// The board manifest (boards/<board>/manifest.json), embedded at build time.
std::string_view manifest();

// Walks the frame buffer top to bottom, one RGB565 row per call.
void screenshot(void (*row)(void* ctx, int y, const uint16_t* px, int width), void* ctx);

}  // namespace garnet::bsp
