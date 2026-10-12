#include "app/mapping.h"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

namespace garnet {
namespace {
constexpr const char* kTag = "mapping";
constexpr const char* kNs = "garnet";
constexpr const char* kKey = "map";
}  // namespace

void Mapping::load() {
    clear();
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = 0;
    if (nvs_get_str(h, kKey, nullptr, &n) == ESP_OK && n > 0) {
        std::string s(n, '\0');
        if (nvs_get_str(h, kKey, s.data(), &n) == ESP_OK) fromJson(std::string_view(s.c_str()));
    }
    nvs_close(h);
    ESP_LOGI(kTag, "%u binding(s) loaded", unsigned(_count));
}

bool Mapping::save() const {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    const std::string s = toJson();
    const bool ok = nvs_set_str(h, kKey, s.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

const char* Mapping::action(std::string_view input) const {
    for (size_t i = 0; i < _count; ++i) {
        if (input == _b[i].input) return _b[i].action;
    }
    return "none";
}

bool Mapping::set(std::string_view input, std::string_view action) {
    if (input.empty() || input.size() >= sizeof(Binding::input) || action.size() >= sizeof(Binding::action)) return false;
    size_t i = 0;
    while (i < _count && input != _b[i].input) ++i;
    if (action.empty() || action == "none") {
        if (i == _count) return true;
        _b[i] = _b[--_count];
        return true;
    }
    if (i == _count) {
        if (_count >= kMaxBindings) return false;
        ++_count;
    }
    std::memset(&_b[i], 0, sizeof(Binding));
    std::memcpy(_b[i].input, input.data(), input.size());
    std::memcpy(_b[i].action, action.data(), action.size());
    return true;
}

void Mapping::clear() { _count = 0; }

std::string Mapping::toJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON* b = cJSON_AddObjectToObject(root, "bindings");
    for (size_t i = 0; i < _count; ++i) cJSON_AddStringToObject(b, _b[i].input, _b[i].action);
    char* txt = cJSON_PrintUnformatted(root);
    std::string out = txt ? txt : "{}";
    cJSON_free(txt);
    cJSON_Delete(root);
    return out;
}

bool Mapping::fromJson(std::string_view json) {
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (!root) return false;
    const cJSON* b = cJSON_GetObjectItemCaseSensitive(root, "bindings");
    if (!cJSON_IsObject(b)) {
        cJSON_Delete(root);
        return false;
    }
    clear();
    bool ok = true;
    for (const cJSON* it = b->child; it; it = it->next) {
        if (!cJSON_IsString(it) || !set(it->string, it->valuestring)) ok = false;
    }
    cJSON_Delete(root);
    return ok;
}

}  // namespace garnet
