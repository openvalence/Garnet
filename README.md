# Garnet

Embedded Valence client firmware for handheld remotes.

- Link: WiFi WebSocket first; ESP-NOW later. A remote opens no BLE session.
- UI: built live from the hub's catalog.
- Input: user button-mapping system over a per-board input manifest.

## Boards

- Waveshare ESP32-S3-Touch-LCD-4.3 (first)
- R+D RADR v0.4 (next)
- M5Stack CoreS3 SE (env `m5_cores3se`)
- OSSM M5 Remote on a CoreS3 / CoreS3 SE (env `m5_remote`, `docs/m5-remote.md`)

## Toolchain

PlatformIO with the pioarduino platform, framework `espidf` (ESP-IDF 5.x),
LVGL 9.

## Build, flash, bench

```
pio run -e waveshare_s3_43
pio run -e waveshare_s3_43 -t upload --upload-port COMxx   # name the port, every time
python tools/tether_bridge.py --port COMxx --hub ws://127.0.0.1:82/ --log artifacts/serial.log
```

On the board's console, `link tether` routes the session through the bridge to
a hub's WebSocket (valencesim on this PC); `wifi <ssid> <pass>` and
`link wifi` use the WiFi path. `help` lists the rest.

## Layout

- `docs/DESIGN.md`: design rulings and open questions.
- The Valence client library (MIT) lives in `../Valence/lib/valence`; the
  build reads that sibling checkout at the sha in `valence.pin`.
- Volatile status lives on the dev board: `bd ready` (prefix `gar`).

## License

Apache-2.0. See `LICENSE`.
