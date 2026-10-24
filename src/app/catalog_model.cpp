#include "app/catalog_model.h"

#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "valence/generated/registry_constants.hpp"
#include "valence/wire/catalog_codec.hpp"
#include "valence/wire/packed/layout_codec.hpp"

namespace garnet {
namespace {
constexpr const char* kTag = "model";
constexpr size_t kMaxWatchedWishes = 14;  // Client::kMaxWishes minus safety and catalog
}  // namespace

using valence::CborFieldType;
using valence::ChannelClass;

bool Setting::isToggle() const { return sf->type == CborFieldType::bool_t || lf->unit == "flag"; }

float Setting::min() const {
    if (sf->hasMin) return sf->min;
    if (lf->hasMin) return lf->min;
    return 0.0f;
}

float Setting::max() const {
    if (sf->hasMax) return sf->max;
    if (lf->hasMax) return lf->max;
    return hasOptions() ? float(sf->options.count - 1) : 100.0f;
}

float Setting::step() const {
    if (sf->hasStep && sf->step > 0.0f) return sf->step;
    if (lf->hasStep && lf->step > 0.0f) return lf->step;
    if (sf->type == CborFieldType::f32_t) return (max() - min()) / 100.0f;
    return 1.0f;
}

void Model::release() {
    heap_caps_free(_bytes);
    _bytes = nullptr;
    _len = 0;
}

bool Model::load(std::span<const std::byte> encoded) {
    _loaded = false;
    _refusal = nullptr;
    ++_gen;
    _settingCount = 0;
    _watchedCount = 0;
    _position = {};
    clearValues();
    release();
    if (encoded.size() > valence::limits::catalog_max_bytes) {
        ESP_LOGE(kTag, "catalog %u B over the %u B ceiling", unsigned(encoded.size()),
                 unsigned(valence::limits::catalog_max_bytes));
        _refusal = "Catalog over 1 MiB.";
        return false;
    }
    _bytes = static_cast<std::byte*>(heap_caps_malloc(encoded.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_bytes) {
        ESP_LOGE(kTag, "no PSRAM for the %u B catalog", unsigned(encoded.size()));
        _refusal = "No memory for the catalog.";
        return false;
    }
    std::memcpy(_bytes, encoded.data(), encoded.size());
    _len = encoded.size();
    _cat.clear();
    const auto r = valence::decodeCatalog(std::span<const std::byte>(_bytes, _len), _cat);
    if (!r || !_cat.ok()) {
        _refusal = (!r && r.error() != valence::DecodeError::CapacityExceeded) ? "Catalog unreadable."
                                                                              : "Catalog too big for this remote.";
        ESP_LOGE(kTag, "catalog decode failed (%u B, error %d): entries %u, layout %u, schema %u, labels %u, stores %u, safe %u",
                 unsigned(_len), r ? -1 : int(r.error()), unsigned(_cat.count), unsigned(_cat.layoutUsed),
                 unsigned(_cat.schemaUsed), unsigned(_cat.labelUsed), unsigned(_cat.storeUsed), unsigned(_cat.safeUsed));
        return false;
    }
    index();
    _loaded = true;
    ESP_LOGI(kTag, "catalog %u B: %u entries, %u layout, %u schema; %u settings, position %s, %u watched",
             unsigned(_len), unsigned(_cat.count), unsigned(_cat.layoutUsed), unsigned(_cat.schemaUsed),
             unsigned(_settingCount), _position.lf ? "yes" : "no", unsigned(_watchedCount));
    return true;
}

void Model::index() {
    auto watch = [&](uint16_t ch) {
        for (size_t i = 0; i < _watchedCount; ++i) {
            if (_watched[i] == ch) return true;
        }
        if (_watchedCount >= kMaxWatchedWishes) return false;
        _watched[_watchedCount++] = ch;
        return true;
    };
    for (uint16_t i = 0; i < _cat.count && !_position.lf; ++i) {
        const auto& e = _cat.entries[i];
        if (e.cls != ChannelClass::STATE) continue;
        uint16_t off = 0;
        for (const auto& f : _cat.layoutFields(e)) {
            if (f.role == "telemetry.position") {
                _position = Readout{&f, e.id, off};
                watch(e.id);
                break;
            }
            off = uint16_t(off + f.wireSize());
        }
    }
    for (uint16_t i = 0; i < _cat.count; ++i) {
        const auto& e = _cat.entries[i];
        if (e.cls != ChannelClass::STATE || !e.hasSettingChannel) continue;
        const auto* intent = _cat.find(e.settingChannel);
        if (!intent) continue;
        const auto fields = _cat.layoutFields(e);
        uint16_t maskOff = 0xFFFF;
        uint16_t off = 0;
        for (const auto& f : fields) {
            if (f.role == "meta.enabled_mask") maskOff = off;
            off = uint16_t(off + f.wireSize());
        }
        off = 0;
        uint8_t ordinal = 0;
        bool watchedThis = false;
        for (const auto& f : fields) {
            const uint16_t at = off;
            off = uint16_t(off + f.wireSize());
            if (!f.hasSettingKey) continue;
            const uint8_t bit = ordinal++;
            if (f.isString() || (f.flags & valence::setting_flags::secret) || _settingCount >= kMaxSettings) continue;
            const valence::SchemaField* sf = nullptr;
            for (const auto& s : _cat.schemaFields(*intent)) {
                if (s.key == f.settingKey) sf = &s;
            }
            if (!sf || sf->type == CborFieldType::tstr_t || sf->type == CborFieldType::bstr_t) continue;
            if (!watchedThis && !watch(e.id)) break;
            watchedThis = true;
            Setting& s = _settings[_settingCount++];
            s = Setting{&f, sf, e.id, intent->id, at, maskOff, bit, sf->hasAccess ? sf->access : intent->access};
        }
    }
}

const Setting* Model::byRole(std::string_view role) const {
    for (size_t i = 0; i < _settingCount; ++i) {
        if (_settings[i].lf->role == role || _settings[i].sf->role == role) return &_settings[i];
    }
    return nullptr;
}

const Setting* Model::byWrite(uint16_t intentCh, uint8_t key) const {
    for (size_t i = 0; i < _settingCount; ++i) {
        if (_settings[i].intentCh == intentCh && _settings[i].sf->key == key) return &_settings[i];
    }
    return nullptr;
}

void Model::onState(uint16_t ch, std::span<const std::byte> payload) {
    Value* free = nullptr;
    for (auto& v : _values) {
        if (v.len != 0 && v.ch == ch) {
            free = &v;
            break;
        }
        if (v.len == 0 && !free) free = &v;
    }
    if (!free) return;
    const size_t n = payload.size() < free->bytes.size() ? payload.size() : free->bytes.size();
    free->ch = ch;
    free->len = uint16_t(n);
    std::memcpy(free->bytes.data(), payload.data(), n);
}

void Model::clearValues() {
    for (auto& v : _values) v.len = 0;
}

const Model::Value* Model::find(uint16_t ch) const {
    for (const auto& v : _values) {
        if (v.len != 0 && v.ch == ch) return &v;
    }
    return nullptr;
}

std::optional<float> Model::value(const Setting& s) const { return value(Readout{s.lf, s.stateCh, s.offset}); }

std::optional<float> Model::value(const Readout& r) const {
    const Value* v = find(r.ch);
    if (!v || size_t(r.offset) + r.lf->wireSize() > v->len) return std::nullopt;
    return valence::readField(*r.lf, std::span<const std::byte>(v->bytes.data() + r.offset, r.lf->wireSize()));
}

bool Model::enabled(const Setting& s, valence::AccessLevel roles) const {
    if (uint8_t(roles) < uint8_t(s.access)) return false;
    if (s.maskOffset == 0xFFFF) return true;
    const Value* v = find(s.stateCh);
    if (!v || s.maskOffset >= v->len) return true;
    return (uint8_t(v->bytes[s.maskOffset]) >> s.maskBit) & 1u;
}

std::string_view Model::optionLabel(const Setting& s, size_t i) const {
    const auto labels = _cat.optionLabels(*s.sf);
    return i < labels.size() ? labels[i] : std::string_view{};
}

std::optional<valence::IntentValueMap> Model::encode(const Setting& s, float physical) const {
    float v = physical;
    if (v < s.min()) v = s.min();
    if (v > s.max()) v = s.max();
    valence::IntentValueMap m{};
    m.count = 1;
    m.fields[0].key = s.sf->key;
    switch (s.sf->type) {
        case CborFieldType::uint_t: m.fields[0].value = valence::IntentValue::ofU64(uint64_t(std::lround(v < 0 ? 0 : v))); break;
        case CborFieldType::int_t: m.fields[0].value = valence::IntentValue::ofI64(int64_t(std::lround(v))); break;
        case CborFieldType::f32_t: m.fields[0].value = valence::IntentValue::ofF32(v); break;
        case CborFieldType::bool_t: m.fields[0].value = valence::IntentValue::ofBool(v >= 0.5f); break;
        default: return std::nullopt;
    }
    return m;
}

}  // namespace garnet
