# Garnet

Embedded Valence client firmware for handheld remotes.

- Link: WiFi WebSocket first; ESP-NOW and BLE later.
- UI: built live from the hub's catalog.
- Input: user button-mapping system over a per-board input manifest.

## Boards

- Waveshare ESP32-S3-Touch-LCD-4.3 (first)
- R+D RADR v0.4 (next)
- M5Stack Core2 / CoreS3 OSSM M5 remote (next)

## Toolchain

PlatformIO with the pioarduino platform, framework `espidf` (ESP-IDF 5.x),
LVGL 9.

## Layout

- `docs/DESIGN.md`: design rulings and open questions.
- The Valence client library (MIT) lives in `../Valence/lib/valence`.
- Volatile status lives on the dev board: `bd ready` (prefix `gar`).

## License

Apache-2.0. See `LICENSE`.
