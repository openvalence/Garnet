// The hub's catalog, decoded, plus the latest STATE of every channel Garnet
// shows. The UI is built from this alone (SPEC §8.9): widget from type and
// constraints, settings through the RFC-009 metamodel (a STATE layout field
// with a setting key, written through its entry's setting channel).
// Constraints:
// - UI task only.
// - Lives in PSRAM (Session allocates it): the count pools below are ~1.9 MB.
// - The string views in the catalog alias _bytes; load() replaces both at once.
// - _bytes is the ONLY raw copy of the catalog Garnet keeps (sized per load,
//   PSRAM). It must not alias the Client's reassembly buffer: a refetch
//   overwrites that buffer while this catalog is still the one in use.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "valence/channel/catalog.hpp"
#include "valence/wire/messages/intent.hpp"

namespace garnet {

// Count pools for a catalog at the SPEC 8.1 ceiling: catalog_max_entries (256)
// entries and 64 fields per entry (CatalogEntry::kMaxFields) bound layout plus
// schema fields at 16,384; each pool takes half of that, so a catalog that
// needs more than 8,192 fields of ONE class is refused (Model::refusal()).
// Labels follow the measured Nucleus ratio (576 labels per 778 fields) with
// headroom. A pool is a fixed array: raising one costs PSRAM at boot, whether
// or not a hub ever fills it.
using Catalog = valence::BasicCatalog<256, 8192, 8192, 12288, 8, 1024>;

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
    static constexpr size_t kMaxWatched = 16;

    Model() = default;
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;
    ~Model() { release(); }

    bool load(std::span<const std::byte> encoded);
    bool loaded() const { return _loaded; }
    // Why the last load() refused, in words for the screen; nullptr if it did not.
    const char* refusal() const { return _refusal; }
    std::span<const std::byte> bytes() const { return {_bytes, _len}; }
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

    void release();

    std::byte* _bytes = nullptr;
    size_t _len = 0;
    const char* _refusal = nullptr;
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
