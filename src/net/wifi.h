#pragma once

#include <cstdint>

namespace garnet::wifi {

enum class State : uint8_t { Off, Connecting, Connected, Portal, Failed };

// Call once after nvs_flash_init() and esp_event_loop_create_default().
// Joins the stored network, else opens the provisioning portal.
void start();

// state() and ipv4() are safe from any task.
State state();
const char* sta_ssid();         // stored SSID or ""
uint32_t ipv4();                // network byte order, 0 unless Connected
uint32_t broadcast_ipv4();      // network byte order, 0 unless Connected

// Valid in Portal. The passphrase is for the device screen only, never a log.
const char* portal_ssid();
const char* portal_pass();

// From the portal the switch to STA is deferred ~1 s so the HTTP reply lands.
bool set_credentials(const char* ssid, const char* pass);
void forget();                  // erase credentials, open the portal

}  // namespace garnet::wifi
