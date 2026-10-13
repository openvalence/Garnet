#pragma once

#include <cstddef>
#include <cstdint>

namespace garnet::wifi {

enum class State : uint8_t { Off, Connecting, Connected, Portal, Failed };

constexpr int kMaxNetworks = 4;

// Call once after nvs_flash_init() and esp_event_loop_create_default().
// Joins the saved networks, else opens the provisioning portal.
void start();

// state() and ipv4() are safe from any task.
State state();
uint32_t ipv4();                // network byte order, 0 unless Connected
uint32_t broadcast_ipv4();      // network byte order, 0 unless Connected

// Valid in Portal. The passphrase is for the device screen only, never a log.
const char* portal_ssid();
const char* portal_pass();

// The saved-network list (RFC-116). Every call below is safe from any task
// (one mutex) and every edit persists before it returns. Indices are 0-based
// list positions; the console shows them 1-based.
int count();
int live();                                      // index of the joined network, or -1
bool ssid(int i, char* out, size_t n);           // local screen only, never a log; n >= 33
// A same-SSID entry is replaced. Joins only when nothing is connected; from the
// portal the switch is deferred ~1 s so the HTTP reply lands. False: invalid, or full.
bool add(const char* ssid, const char* pass);
bool remove(int i);                              // removing the live one rejoins from the rest
bool move(int from, int to);
void forget_all();                               // erase the list, open the portal
void open_portal();                              // keep the list, open the portal to add one
void cancel_portal();                            // leave the portal and rejoin the list

}  // namespace garnet::wifi
