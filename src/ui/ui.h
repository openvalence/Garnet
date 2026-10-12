// The LVGL screens. The safety strip (Halt, Pause, link state) lives on
// lv_layer_top() so no screen, list or dropdown can cover it; nothing else
// may be created on that layer.
// Constraints: UI task only (LVGL is single-task).
#pragma once

#include <cstdint>

namespace garnet {
class Session;
class Mapping;
}

namespace garnet::ui {

void build(Session& session, Mapping& mapping);
// Every 10 ms with the debounced physical button mask (bsp::buttons()).
void tick(uint32_t buttons);
// A press of physical input `index` from the console, for a bench without
// hands on the board. Goes through the same path as the hardware.
void injectPress(int index);
// Bench drivers for what a finger does on the screen: "tab <n>",
// "mapstart" (the Map inputs button), "mappick <action id>" (pick + Save).
void benchCommand(const char* verb, const char* arg);

}  // namespace garnet::ui
