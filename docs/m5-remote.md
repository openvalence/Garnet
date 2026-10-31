# OSSM M5 Remote on Garnet

The OSSM M5 Remote is a carrier PCB with four knobs and a key that an M5Stack
core plugs into. Its own firmware speaks a legacy ESP-NOW protocol. Garnet
runs the same hardware as a Valence client (env `m5_remote`). This file holds
the hardware map and the interaction model Garnet copies. Build state lives
on the `gar` board.

Source: [ortlof/OSSM-M5-Remote](https://github.com/ortlof/OSSM-M5-Remote),
read at commit `05d9abd` (2026-04-30). Links below point at that commit.

[cfg]: https://github.com/ortlof/OSSM-M5-Remote/blob/05d9abd6a8f8dfda0e76efb9cef171cff2034b8e/src/config.h
[main]: https://github.com/ortlof/OSSM-M5-Remote/blob/05d9abd6a8f8dfda0e76efb9cef171cff2034b8e/src/main.cpp
[ui]: https://github.com/ortlof/OSSM-M5-Remote/blob/05d9abd6a8f8dfda0e76efb9cef171cff2034b8e/src/ui/ui.c
[lang]: https://github.com/ortlof/OSSM-M5-Remote/blob/05d9abd6a8f8dfda0e76efb9cef171cff2034b8e/src/language.h
[ini]: https://github.com/ortlof/OSSM-M5-Remote/blob/05d9abd6a8f8dfda0e76efb9cef171cff2034b8e/platformio.ini
[mnt]: https://github.com/ortlof/OSSM-M5-Remote/blob/05d9abd6a8f8dfda0e76efb9cef171cff2034b8e/Hardware/PCB/OSSM-M5-Remote-V2.3.mnt
[bom]: https://github.com/ortlof/OSSM-M5-Remote/blob/05d9abd6a8f8dfda0e76efb9cef171cff2034b8e/Hardware/PCB/OSSM-M5-Remote-V2.3.txt
[readme]: https://github.com/ortlof/OSSM-M5-Remote/blob/05d9abd6a8f8dfda0e76efb9cef171cff2034b8e/README.md

## 1. Hardware

| Part | Fact | Source |
|---|---|---|
| Core | M5Stack CoreS3 SE (BOM), or Core2 or CoreS3. Arduino, M5Unified, LVGL 9. | [readme], [ini] |
| Screen | 320x240, the core's own ILI9342C, touch on | [cfg], [main] |
| Knobs | 4 Bourns PEC12R, 24 detents, 24 pulses per turn. The two outer knobs (U1 left, U4 right) have a push switch (PEC12R-4220F-S0024); the two inner knobs (U2, U3) do not. | [bom], [mnt] |
| Key | 1 Cherry MX switch, centered between the inner knobs | [bom], [mnt] |
| Pulls | 11 x 10k on the PCB: 8 on the knob A/B lines, 3 on the keys | [bom] |
| Haptics | `M5.Power.setVibration`: Core2 only; the CoreS3 has no motor | [main] |
| Battery | 2000 mAh LiPo through the core's PMU | [readme] |

Front view, left to right in one row: left knob (push), left-center knob,
MX key, right-center knob, right knob (push). The outer knobs sit higher,
beside the core ([mnt]).

### 1.1 CoreS3 pins (the `m5_remote` env)

| Garnet id | Part | GPIO | Pull (as the remote's firmware sets it) | Source |
|---|---|---|---|---|
| `knob_l` | encoder 1 (speed) | A (CLK) 5, B (DT) 9 | weak pull-down (ESP32Encoder default) | [cfg] |
| `knob_cl` | encoder 2 (depth) | A 18, B 17 | weak pull-down | [cfg] |
| `knob_cr` | encoder 3 (stroke) | A 1, B 2 | weak pull-down | [cfg] |
| `knob_r` | encoder 4 (sensation, menu focus) | A 7, B 6 | weak pull-down | [cfg] |
| `mx` | MX key | 10, active high | none (external) | [cfg] `Button1(10, false)` |
| `push_l` | left knob push | 8, active high | none (external) | [cfg] `Button2(8, false)` |
| `push_r` | right knob push | 14, active high | weak pull-up | [cfg] `Button3(14, false, true)` |
| `power` | the core's power key (AXP2101) | n/a | n/a | Garnet's CoreS3 BSP |

Decoding: ESP32Encoder `attachHalfQuad`, which counts both edges of A with
B as the direction level, two counts per detent. The firmware takes one step
per two counts ([main]). Garnet does the same on the S3's four PCNT units
with ESP32Encoder's glitch filter (3.125 us).

The knob-to-position map (encoder 1 is the left knob, and so on) is inferred,
not traced: the left push is "Encoder Left" and starts Connect ([cfg],
[readme]); encoder 4 moves the menu focus and the right push selects ([main]);
the home sliders run Speed, Depth, Stroke, Sensation top to bottom ([ui]).
The schematic PDF lists the M-Bus pins but its nets are not legible as text.
The bench check is gar-4bv.1.

The Core2 pins differ ([cfg] `#else` branch: encoders 25/26, 13/14, 33/32,
19/27; keys 35, 36, 34). The Core2 is a classic ESP32, which no Garnet env
targets.

## 2. Interaction model of the original

Keys: MX `click` and `long`, left push `click`, right push `click`, `long`
and `double` (OneButton, [main]). The three on-screen buttons at the bottom
of every screen are labeled with what the three keys do (L = left push,
M = MX, R = right push).

| Screen | Knobs | Left push (L) | MX (M) | Right push (R) |
|---|---|---|---|---|
| Start | none | Connect | Settings | Demo (Home without a machine) |
| Home | 1 Speed, 2 Depth, 3 Stroke, 4 Sensation; a fast spin ramps the step up to 8 (75 ms window) | Pullout: speed, depth and stroke to 0 (EJECT add-on) | Start/Stop (OSSM on/off) | click: Pattern; long: reset Sensation to 0; double: dynamic stroke |
| Menu | 4 moves the focus | Settings | Home | selects the focused item; long: reboot the machine |
| Pattern | 4 scrolls the list | Menu | Home | Save the pattern, back to Home |
| Torque | 1 outward torque, 4 inward torque | none | Home | Menu |
| Settings | 4 moves the focus | (touch) Save | Home | Menu |

MX long press is detected but bound to nothing. There is no dedicated
emergency stop: the stop is MX (Start/Stop) or Pullout.

Home shows, top to bottom: connection state, the four values as a slider
and a number, the pattern name, battery percent. Speed is strokes per minute
(0 to the machine's limit, 300 by default), depth and stroke are mm,
sensation is -100 to 100 ([main], [lang]).

## 3. On Garnet

Everything goes through the Valence session. The knobs nudge catalog
settings found by role, within the hub's ranges and steps; nothing is sent
outside the catalog.

| Original | Garnet (default mapping, `boards/m5_remote/manifest.json`) |
|---|---|
| Knob 1 Speed | `knob_l`: `speed+` (role `pattern.speed`), counterclockwise `speed-` |
| Knob 2 Depth | `knob_cl`: `depth+` (role `pattern.depth`) |
| Knob 3 Stroke | `knob_cr`: `stroke+` (role `pattern.stroke`). Clockwise is longer; the original ran this knob the other way because its slider drew the stroke from the depth end. |
| Knob 4 Sensation | `knob_r`: `sens+` (role `pattern.sensation`) |
| Fast-spin ramp | kept: up to 8 steps per detent within 75 ms, one write per 50 ms |
| MX Start/Stop | `mx`: `run`, toggles role `pattern.running` from the hub's value |
| Left push Pullout | `push_l`: `halt`. One press latches the stop (the same path as the on-screen Halt, datagram included). A new press held 3 s after the latch releases it. |
| Right push Pattern | `push_r`: `pattern+`, the next pattern (role `pattern.select`), wrapping |
| Home screen | the Home tab: Speed, Depth, Stroke, Sensation as bar plus number with unit, the pattern, run state, the key legend. The row a knob turned last is highlighted. |
| Menu, Settings, Torque | the Controls and Setup tabs, by touch |
| Battery percent | not shown yet |

A write that cannot go out says why on the Home status line: no session,
needs `control` (Setup: Request control), needs `configure`, or disabled by
the hub. The value stays the hub's.

Every binding can be changed on the Map tab. A knob takes only actions that
have a direction; its counterclockwise action is the mirror
(`speed+`/`speed-`, `inc:`/`dec:`).
