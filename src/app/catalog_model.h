// The hub's catalog, decoded, plus the latest STATE of every channel Garnet
// shows. The UI is built from this alone (SPEC §8.9): widget from type and
// constraints, settings through the RFC-009 metamodel (a STATE layout field
// with a setting key, written through its entry's setting channel).
// Constraints:
// - UI task only.
// - Lives in PSRAM (Session allocates it): the decoded catalog is ~200 KB.
// - The string views in the catalog alias _bytes; load() replaces both at once.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "valence/channel/catalog.hpp"
#include "valence/wire/messages/intent.hpp"

namespace garnet {

// Sized for the Nucleus catalog (measured 234 entries, 438 layout fields,
// 340 schema fields, 576 labels), with headroom.
using Catalog = valence::BasicCatalog<320, 768, 512, 1024, 8, 64>;

struct Setting {
    const valence::LayoutField* lf = nullptr;  // the STATE field: current value
    const valence::SchemaField* sf = nullptr;  // the INTENT field: write type and range
    uint16_t stateCh = 0;
    uint16_t intentCh = 0;
    uint16_t offset = 0;                       // byte offset of lf in the STATE payload
    uint16_t maskOffset = 0xFFFF;              // meta.enabled_mask field, if the layout has one
    uint8_t maskBit = 0;
    valence::AccessLevel access = valence::AccessLevel::control;

    std::string_view name() const { return lf->name; }
    bool isToggle() const;
    bool hasOptions() const { return !sf->options.empty(); }
    float min() const;
    float max() const;
    float step() const;
};

struct Readout {
    const valence::LayoutField* lf = nullptr;
    uint16_t ch = 0;
    uint16_t offset = 0;
};

class Model {
public:
    static constexpr size_t kMaxSettings = 96;
    static constexpr size_t kMaxBytes = 48 * 1024;  // = the Client's reassembly budget
    static constexpr size_t kMaxWatched = 16;

    bool load(std::span<const std::byte> encoded);
    bool loaded() const { return _loaded; }
    std::span<const std::byte> bytes() const { return {_bytes.data(), _len}; }
    const Catalog& catalog() const { return _cat; }
    uint32_t generation() const { return _gen; }  // bumps on every load()

    std::span<const Setting> settings() const { return {_settings.data(), _settingCount}; }
    const Setting* byRole(std::string_view role) const;
    const Setting* byWrite(uint16_t intentCh, uint8_t key) const;
    const Readout* position() const { return _position.lf ? &_position : nullptr; }

    // STATE channels the UI shows, position first: subscribe to these.
    std::span<const uint16_t> watched() const { return {_watched.data(), _watchedCount}; }

    void onState(uint16_t ch, std::span<const std::byte> payload);
    void clearValues();
    std::optional<float> value(const Setting& s) const;
    std::optional<float> value(const Readout& r) const;
    bool enabled(const Setting& s, valence::AccessLevel roles) const;
    std::string_view optionLabel(const Setting& s, size_t i) const;

    // An absolute write of `physical`, clamped to the field range and typed
    // for the INTENT schema. nullopt for a type Garnet does not write.
    std::optional<valence::IntentValueMap> encode(const Setting& s, float physical) const;

private:
    struct Value {
        uint16_t ch = 0;
        uint16_t len = 0;
        std::array<std::byte, 256> bytes{};
    };
    const Value* find(uint16_t ch) const;
    void index();

    std::array<std::byte, kMaxBytes> _bytes{};
    size_t _len = 0;
    Catalog _cat{};
    bool _loaded = false;
    uint32_t _gen = 0;
    std::array<Setting, kMaxSettings> _settings{};
    size_t _settingCount = 0;
    Readout _position{};
    std::array<uint16_t, kMaxWatched> _watched{};
    size_t _watchedCount = 0;
    std::array<Value, kMaxWatched> _values{};
};

}  // namespace garnet
