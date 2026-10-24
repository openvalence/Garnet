# Garnet design

Garnet is the firmware for handheld Valence remotes. It runs a Valence client
session to a hub, builds its screens from the hub's catalog, and maps the
board's physical inputs to actions the user picks.

Volatile state (what is built, what is measured, open work) lives on the `gar`
board, not here. Wire truth lives in `../Valence/spec/`. This file holds the
design rulings.

## 1. Rulings (operator, 2026-10-11)

| Topic | Ruling |
|---|---|
| Transport | WiFi WebSocket first. ESP-NOW follows the Nucleus C6 work (SPEC §13.3 plus a candidate RFC). A remote opens no BLE session (RFC-121; BLE is a hub setup transport). |
| Stop | A dedicated, latched stop. It is labeled "Halt", or "E-Stop" only when the hub declares `estop_cuts_power` (§11.2 H1, RENDERING.md law 15). |
| Pairing | At the `control` tier, with `watch` as the fallback. |
| UI | Built live from the hub's catalog. |
| Toolchain | Pure ESP-IDF plus LVGL 9. |
| Protocols | Valence only. No legacy OSSM protocols. |
| License | Apache-2.0. The Valence client library is MIT. |
| Input mapping | The BSP only exposes the board's inputs. The user maps them to actions on the device. |
| Dead man's handle | Open question. Not built (candidate RFC R3 in the research draft). |

## 2. Architecture

```
+---------------------------------------------------------------+
| ui/        LVGL screens; the safety strip on lv_layer_top     |
| app/       Session (owns the Client), Model (decoded catalog  |
|            + live STATE), Mapping (input -> action), console  |
+----------------------+----------------------------------------+
| link/                |  ../Valence/lib/valence (MIT, header-  |
| WsLink, TetherLink,  |  only): Client, catalog codec, ESTOP   |
| Udp (discovery and   |  frame, EstopDatagramSender            |
| ESTOP datagram)      |                                        |
+----------------------+----------------------------------------+
| bsp/<board>.cpp: panel, touch, input scan, board manifest      |
| net/wifi: STA and the provisioning portal                      |
+---------------------------------------------------------------+
| ESP-IDF 5.5 (pioarduino platform 55.03.39), LVGL 9.2           |
+---------------------------------------------------------------+
```

### 2.1 Tasks

| Task | Owns | Talks to the UI task through |
|---|---|---|
| `ui` (core 1) | LVGL, Session, Client, Model, Mapping, input scan | (it is the UI task) |
| WebSocket client | the TCP socket | `FrameQueue` (link/link.h) |
| `console` | the USB console reader | `FrameQueue` for "@V" frames, a command queue for the rest |
| WiFi, lwIP, esp_timer, portal DNS and HTTP | the network stack | atomics, plus one mutex over the saved list and join state, in net/wifi.cpp |

The Client is not thread-safe and needs no locks: only the UI task calls it.
A receive task copies each frame into a FreeRTOS queue. The UI task drains
that queue in `Client::update()`. This queue is the only point where data
crosses between tasks.

### 2.2 Memory

- The catalog ceiling is `catalog_max_bytes` (1 MiB) and 256 entries (SPEC §8.1,
  RFC-121); client-watch must accept both. Every large buffer lives in PSRAM
  and none on a task stack.
- The `Client` carries its catalog reassembly buffer inline: 5,462 chunks,
  1,048,704 B (`VALENCE_CLIENT_CATALOG_CHUNKS` in `src/CMakeLists.txt`).
  Garnet allocates the Client from PSRAM with placement new, and
  `session.cpp` static-asserts the budget against `catalog_max_bytes`.
- The `Model` holds the decoded catalog (`BasicCatalog<256, 8192, 8192, 12288,
  8, 1024>`, about 1.9 MB) and the one raw copy Garnet keeps of the encoded
  bytes, allocated at the catalog's exact size on each load. The decoded
  strings point into that copy. It is a copy, not a view of the Client's
  buffer, because a refetch overwrites that buffer while the old catalog is
  still in use. The Model lives in PSRAM.
- The count pools follow the 256-entry, 64-fields-per-entry bound (16,384
  layout plus schema fields), split evenly. A catalog needing more than 8,192
  fields of one class is refused: the screen says "Catalog too big for this
  remote.", and Halt and the safety channel do not depend on the Model.
- The panel's frame buffer is in PSRAM. LVGL draws into a 20-line partial
  buffer in internal RAM. `CONFIG_SPIRAM_XIP_FROM_PSRAM` keeps the cache up
  during flash writes, so the scan-out does not drift.
- No `thread_local` and no function-local statics in task code (two field
  bugs in Nucleus). State lives in file-scope objects or in heap objects
  owned by the UI task.

### 2.3 Library work (Valence main)

| Gap | Library addition |
|---|---|
| L1: catalog dropped after the hash check | `ClientDelegate::onCatalog(bytes, verified)`, fired before LIVE |
| L2: 12 KB reassembly budget | `VALENCE_CLIENT_CATALOG_CHUNKS`, default 192 chunks (36 KB); Garnet sets 5,462 (RFC-121) |
| L3: no mid-session SUBSCRIBE | `Client::subscribe()` |
| L4: no safety helpers | `halt()`, `pause()`, `resume()`, `release()`. `pause` and `halt` bypass a full pending table. |
| L5: no sessionless stop | `EstopDatagramSender` (RFC-053) |
| Halt label | `Client::estopCutsPower()` from WELCOME identity |
| Link health | `Client::rxAgeMs()` |

The epic is `rfc-0d12` on the Valence board. None of these changes alter
the wire.

## 3. Board support (`src/bsp/bsp.h`)

```cpp
namespace garnet::bsp {
struct Caps { uint16_t width, height; uint8_t buttons; };
const Caps& caps();
void init();                  // panel, touch, IO expander, LVGL display + pointer
uint32_t buttons();           // bit i = physical button i (manifest order), debounced
std::string_view manifest();  // boards/<board>/manifest.json, embedded
void screenshot(void (*row)(void*, int y, const uint16_t* px, int w), void*);
}
```

Each build compiles exactly one `bsp/<board>.cpp`. `src/CMakeLists.txt`
picks the file from `GARNET_BOARD`. Shared code calls these functions by
name. The compiler checks each call against `bsp.h`, and the linker binds it
to the one definition it finds. There is no virtual table and no function
pointer, so there is no runtime dispatch. A board file that is missing a
function fails at link time on the build machine.

Encoders get one more function when the first board with encoders lands
(`int8_t encoder_delta(int i)`, detents since the last call). It is not
declared yet.

## 4. Input manifest and mapping

### 4.1 The manifest

`boards/<board>/manifest.json` is the one home of a board's input geometry.
The firmware embeds the file and draws the board from it on the Map screen.
`tools/board_svg.py` renders `board.svg` from the same file. The drawing
contains only shapes and labels: no vendor photo, ever.

```json
{
  "board": "waveshare_s3_43",
  "view": "front, screen facing the user, USB connectors on the right edge",
  "image": { "file": "board.svg", "width": 1060, "height": 680, "px_per_mm": 10 },
  "outline": { "x": 0, "y": 0, "w": 1060, "h": 680, "r": 30 },
  "screen": { "x": 53, "y": 44, "w": 955, "h": 543 },
  "features": [ { "label": "RESET", "x": 437, "y": 4, "w": 60, "h": 26 } ],
  "inputs": [
    { "id": "boot", "kind": "button", "label": "BOOT", "x": 545, "y": 4, "w": 70, "h": 26 },
    { "id": "screen", "kind": "touch_zone", "label": "Touchscreen", "x": 53, "y": 44, "w": 955, "h": 543 }
  ]
}
```

- `kind` is one of `button`, `encoder`, `encoder_push`, `dial` (a pot),
  `switch`, `touch_zone`.
- Coordinates are image pixels at `px_per_mm`.
- `features` are drawn for orientation only and cannot be mapped (RESET is
  wired to EN and cannot be read).
- Bit `i` of `bsp::buttons()` is the i-th entry in `inputs` that is not a
  `touch_zone`.

### 4.2 Actions

The mapping table maps an input `id` to an action string. Strings keep an
exported file readable and stable across firmware updates.

| Action | Meaning |
|---|---|
| `none` | unbound |
| `halt` | the latched stop, same as the on-screen Halt |
| `pause` | pause, or resume when paused (resume needs `control`) |
| `speed+` `speed-` | nudge the setting with role `pattern.speed` by its step |
| `stroke+` `stroke-` | role `pattern.stroke` |
| `wmin+` `wmin-` `wmax+` `wmax-` | roles `window.min` and `window.max` |
| `menu+` `menu-` | next or previous screen |
| `inc:<ch>.<key>` `dec:<ch>.<key>` | nudge any writable catalog setting; `<ch>` is the INTENT channel in hex, `<key>` its schema key |
| `toggle:<ch>.<key>` | flip a boolean catalog setting |

The picker lists the fixed actions first, then one entry per writable
setting in the catalog the remote holds now.

### 4.3 The mapping flow (Map screen)

1. The screen draws the board from the manifest and lists each physical
   input with its current action.
2. "Map inputs" walks the physical inputs in manifest order. It highlights
   the current one and asks the user to press it.
3. A press of the highlighted input confirms it. A press of another input
   says which one was pressed and keeps waiting. "Skip" moves on.
4. After the confirm, the user picks an action and saves. The table is
   written to NVS at once.
5. With zero physical inputs, the screen says the touchscreen is the input,
   and "Map inputs" is disabled.

An input already mapped to `halt` still halts during the flow. Every other
mapped action is suspended while the flow runs.

### 4.4 Storage and transfer

- NVS namespace `garnet`, key `map`: `{"version":1,"bindings":{"boot":"halt"}}`.
- Console `map` prints the JSON. `map import <json>` replaces the table and
  saves it. `map clear` empties it.

### 4.5 Safety rules for mapping

- The on-screen Halt is not part of the table and cannot be. A mapping can
  add Halt inputs. It can never remove the screen one.
- No input is mapped by default. On the Waveshare board, BOOT is mappable
  and starts unbound.

## 5. Screens

| Area | Content |
|---|---|
| Safety strip (top 64 px, `lv_layer_top`) | link state and hub name; Pause/Resume; Halt. Nothing else is created on that layer, so no screen, list or dropdown covers it. |
| Status | position (the field with role `telemetry.position`) as a number and a bar; the safety state in words; the access tier |
| Controls | one row per writable setting: a switch for booleans, a dropdown for options, a slider otherwise. A slider writes on release with an absolute value. A row shows the hub's applied value and is disabled, never hidden, without the access or when its `meta.enabled_mask` bit is clear (§8.9). |
| Map | the board drawing and the mapping flow (section 4) |
| Setup | WiFi state or the portal SSID and passphrase; the saved networks (name, live marker, Up/Down/Remove per row); Add network (opens the portal; Cancel leaves it); Request control (knock); Forget hub; link mode |

The strip's states:

- **Halt (not latched):** red. A press sends Halt at once.
- **Halted:** dark red, "HALTED, hold 3 s: release". Releasing takes a new
  press held for 3 s. The press that latched the stop never counts toward
  the release.
- **Halt not confirmed:** amber. The repeat budget ran out with no latch
  seen, or a datagram went out with no session to confirm it.
- **Pause / Resume:** one control with two states, both taken from the hub's
  latch, never from local belief (RENDERING.md law 14). Resume is disabled
  for a `watch` session.
- **Link:** the strip turns dark red with "LINK DOWN" or "LINK STALE" when
  there is no live session or no frame has arrived for 3 s.

## 6. Transport

| Step | Binding | State |
|---|---|---|
| 1 | WiFi WebSocket (§13.2), subprotocol `valence.v1`, path `/` | built |
| 1a | UDP discovery (§13.8) on registry port 22096: probe broadcast, first reply wins, its `hub_instance_id` is remembered | built |
| 1b | RFC-053 ESTOP datagram on the same port, broadcast plus the hub's address | built |
| bench | Tether: frames as `@V <base64>` lines on the USB console, relayed by `tools/tether_bridge.py` | built |
| 2 | ESP-NOW client binding (§13.3) | waits on Nucleus C6 work and the binding RFC (channel discovery, control-tier encryption, peer-table split) |

Discovery is the §13.8 UDP probe, the same front door Phosphor uses. Garnet
does not use mDNS: Phosphor runs none (RFC-072 ruling, 2026-10-01), Nucleus
does not advertise it, and valencesim has none.

WiFi provisioning is a SoftAP captive portal: SSID `Garnet-XXXX`, a random
8-digit WPA2 passphrase shown on the screen only, and a form at
`http://192.168.4.1`. It works on every board, including ones with no touch
input. The form adds a network to the saved list; the console `wifi <ssid>
[pass]` does the same on the bench, with `wifi-list`, `wifi-rm <n>`,
`wifi-mv <from> <to>` and `forget-wifi` (slot numbers, never names: the
console output is a log).

Up to 4 networks are saved (RFC-116, draft; `valence/net/saved_networks.hpp`)
as one NVS blob `nets` in namespace `garnet`. A pre-RFC-116 `wifi_ssid` /
`wifi_pass` pair becomes slot 1 on first boot and is erased. The join rule
runs on boot and after every drop. One saved network connects directly. Two
or more start an all-channel scan, then try the strongest visible first (a
near tie within 3 dB goes to the earlier entry), then the unseen ones in
list order, about 12 s per entry or until a disconnect reason. After the
whole list fails the retry timer backs off (capped at 10 s) and the sequence
restarts from the scan. Every join takes the strongest BSSID above -85 dBm.
The screen is local and shows names; no log line carries an SSID or a
passphrase (a slot number, the BSSID tail, channel and RSSI instead).
Removing the live network is allowed and rejoins from the rest; removing the
last one opens the portal. One mutex guards the list and the join state,
because the UI task and the console call the edits while the event loop and
the timer task run the join. The radio runs with power save off, so a stop
never waits for a DTIM interval.

Reconnects keep the catalog. The cached etag rides the next HELLO, so a
reconnect to an unchanged hub downloads nothing (§6.7).

## 7. Safety rules

| Rule | Source |
|---|---|
| Halt is reachable from every screen, never covered, never remappable away. | RENDERING.md §12, law 1 |
| Halt and Pause bind to spec-core identity (`safety` 0x0003, `safety-intents` 0x0005), never to catalog annotations. | RENDERING.md law 2 |
| Halt and Pause work from the `watch` tier. | §12.2 (role-exempt ops), §11.1 |
| Halt works before LIVE and with no session: the raw frame goes out on any open transport, and the RFC-053 datagram goes out when WiFi is up. | §11.2 ("any endpoint, any tier, any session state"), §6.4, §6.3 |
| In a session, Halt sends the raw frame until the latch is seen (20 x 50 ms) plus the `estop` op. A budget that runs out is shown as a loud failure. | §11.2, registry `estop_repeat_*` |
| The label is "E-Stop" only when the hub declares `estop_cuts_power`, otherwise "Halt". The firmware never presents itself as the hardware e-stop. | §11.2 H1, RENDERING.md law 15 |
| Release is a deliberate held press (3 s) that starts after the latch. Resume is a deliberate press. Neither is ever sent on Garnet's own initiative. | §11.1, §11.2, RFC-085 |
| A full pending-intent table never blocks `pause` or `estop`. | §11.1, §11.2 |
| The stop control shows link state. Stale telemetry is marked, never shown as fresh. | RENDERING.md law 8 |
| Pause, Resume and settings are disabled while not LIVE. Garnet acts on no hub state before LIVE. | §2.2 |

Bench screenshots (console `shot`) block the UI task for about 1.5 s. They
are a bench tool only.

## 8. Pairing

A remote with no token connects at `watch`, where Halt and Pause work. Setup's
"Request control" sends a PAIR_REQ knock (RFC-027(a)). An operator approves it
from any `configure` session. The token from PAIR_GRANT is stored in NVS, and
Garnet reconnects with it at `control`.

## 9. Boards

| | Waveshare ESP32-S3-Touch-LCD-4.3 | R+D RADR v0.4 | M5Stack Core2 (OSSM M5 remote) | M5Stack CoreS3 (OSSM M5 remote) |
|---|---|---|---|---|
| MCU | ESP32-S3 (QFN56 rev 0.2) | ESP32-S3-WROOM-1-N16R8 | ESP32 (classic) | ESP32-S3 |
| Flash / PSRAM | 8 MB QIO / 8 MB octal in package (measured) | 16 MB / 8 MB octal | 16 MB / PSRAM | 16 MB / PSRAM |
| Display | 800x480 RGB565 parallel (ST7262), 16 MHz pclk | 2.0 in ST7789 320x240, SPI 40 MHz | ILI9342C 320x240, SPI | ILI9342C 320x240, SPI |
| Touch | GT911, I2C 0x5D | none | FT6336-class | FT6336-class, I2C 0x38 |
| Physical inputs | BOOT (GPIO0, shared with panel data line 6) | 5 buttons, 2 encoders (A/B) | MX key, 4 encoders (2 with push) | MX key, 4 encoders (2 with push) |
| Haptics, LEDs, sound | none | coin motor, 3 x WS2812B, buzzer | motor on the PMU | none |
| Battery | connector and charger, no gauge | 800 mAh, MAX17048 | 2000 mAh via the PMU | via the PMU |
| BSP state | built (`bsp/waveshare_s3_43.cpp`) | `gar-cs8` | `gar-e2w` | `gar-e2w` |

### 9.1 Waveshare 4.3 specifics

- Source: Waveshare's ESP-IDF demo `08_lvgl_Porting` and the board schematic.
- RGB panel: HSYNC 46, VSYNC 3, DE 5, PCLK 7. Data lines D0..D15 are GPIO
  14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40. Timing:
  hsync 4/8/8, vsync 4/8/8, pclk on the falling edge. Bounce buffer of 10
  lines.
- I2C on SDA 8 and SCL 9. The CH422G IO expander takes command bytes at
  0x24 (mode) and 0x38 (outputs). EXIO1 is the touch reset, EXIO2 the
  backlight enable, EXIO3 the LCD reset, EXIO4 the SD card select, and EXIO5
  USB_SEL. **EXIO5 must stay 0**: 1 disconnects the native USB, which is the
  board's console. Waveshare's demo writes 1 there.
- GT911 address select: hold INT (GPIO4) low while the reset is released to
  get 0x5D.
- **BOOT shares GPIO0 with panel data line 6 (G3), with no series
  resistor.** While the panel runs, the LCD peripheral drives GPIO0 as a
  16 MHz push-pull output, and pressing BOOT shorts that driven output to
  ground. Garnet does three things about this:
  - It drops GPIO0 to the weakest drive strength
    (`GPIO_DRIVE_CAP_0`), which limits the current through the button.
  - It writes every pixel with bit 6 set, so the line idles high. This costs
    one green LSB step, a faint tint on black.
  - It reads a press as "48 samples all low", held for three 10 ms scans. A
    vertical blank lasts about 1 ms, so three scans cannot all land in one.
  Rev the board with a series resistor (or move BOOT off a data line) before
  BOOT becomes a primary input. On this board the touchscreen is the primary
  input.

## 10. Not built yet

- A catalog cache in flash. Today the catalog persists across reconnects
  but not across reboots.
- OTA updates.
- The static-profile fallback (§8.5) for a catalog over the 48 KB model
  budget. Today Garnet logs the overflow and stays at Halt and Pause.
- Settings beyond 14 watched STATE channels. The Client holds 16
  subscription wishes, and safety and catalog take two.
- `meta.trial_pending` and trial writes (RFC-099).
