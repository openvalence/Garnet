// The user's input mapping: board input id (manifest) -> action. Stored in
// NVS as JSON, exported and imported as the same JSON (docs/DESIGN.md,
// "Input mapping").
// Constraints:
// - UI task only.
// - The on-screen Halt is NOT in this table and cannot be: a mapping can add
//   Halt inputs, never remove the screen one.
// - Actions are strings so an exported file stays readable and survives a
//   firmware update: fixed names, or "<op>:<intent channel hex>.<key>" for a
//   catalog setting (op = inc, dec, toggle).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace garnet {

struct ActionInfo {
    const char* id;
    const char* label;
};

// The fixed actions, in picker order.
inline constexpr ActionInfo kFixedActions[] = {
    {"none", "Nothing"},
    {"halt", "Halt"},
    {"pause", "Pause / resume"},
    {"speed+", "Speed up"},
    {"speed-", "Speed down"},
    {"stroke+", "Stroke longer"},
    {"stroke-", "Stroke shorter"},
    {"depth+", "Depth deeper"},
    {"depth-", "Depth shallower"},
    {"sens+", "Sensation +"},
    {"sens-", "Sensation -"},
    {"pattern+", "Next pattern"},
    {"pattern-", "Previous pattern"},
    {"run", "Start / stop"},
    {"wmin+", "Window start +"},
    {"wmin-", "Window start -"},
    {"wmax+", "Window end +"},
    {"wmax-", "Window end -"},
    {"win.a+", "Window rear or size +"},
    {"win.a-", "Window rear or size -"},
    {"win.b+", "Window front or position +"},
    {"win.b-", "Window front or position -"},
    {"winmode", "Window mode"},
    {"menu+", "Next tab"},
    {"menu-", "Previous tab"},
};

class Mapping {
public:
    static constexpr size_t kMaxBindings = 16;

    bool load();                                  // from NVS; false (and empty) when none is stored
    bool save() const;
    const char* action(std::string_view input) const;  // "none" when unbound
    bool set(std::string_view input, std::string_view action);
    void clear();
    std::string toJson() const;
    bool fromJson(std::string_view json);         // replaces the table; false on bad JSON
    size_t count() const { return _count; }

private:
    struct Binding {
        char input[16]{};
        char action[32]{};
    };
    std::array<Binding, kMaxBindings> _b{};
    size_t _count = 0;
};

}  // namespace garnet
