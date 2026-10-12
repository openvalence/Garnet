#!/usr/bin/env python3
"""Bench tether: relay Garnet's console frames to a Valence hub's WebSocket.

The board writes each Valence frame as "@V <base64>" on its USB console (link
mode `tether`); this script sends it to the hub as one binary WS message and
writes the hub's messages back the same way. Every other console line is the
board's log and is printed (and appended to --log). A "shot" command's "@S"
rows are written to --shot as a PNG.

Usage:
  python tools/tether_bridge.py --port COM17 --hub ws://127.0.0.1:82/ \
      [--log artifacts/serial.log] [--shot artifacts/screen.png] [--cmd-file artifacts/cmd.txt] [--reset]

Lines appended to --cmd-file (or typed on stdin) are sent to the board as
console commands.

Constraints:
- Opens the port with DTR and RTS low: on the ESP32-S3 USB-Serial/JTAG port
  that sequence does not reset the chip.
- A HELLO from the board opens a fresh WebSocket: one Valence session per
  socket (SPEC §13.2).
- Needs pyserial and websocket-client (both in PlatformIO's penv).
"""
import argparse
import base64
import os
import struct
import sys
import threading
import time
import zlib

import serial
import websocket

HELLO = 0x00


def png(path, width, height, rows):
    raw = bytearray()
    for y in range(height):
        px = rows.get(y, bytes(width * 2))
        raw.append(0)
        for (v,) in struct.iter_unpack("<H", px):
            raw += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


class Bridge:
    def __init__(self, args):
        self.args = args
        self.ser = None
        if args.reset:  # DTR low with RTS high, then RTS low: a chip reset on the USB-Serial/JTAG port
            with serial.Serial(args.port, 115200) as s:
                s.dtr = False
                s.rts = True
                time.sleep(0.1)
                s.rts = False
            time.sleep(0.5)
        self.reopen()
        self.ws = None
        self.lock = threading.Lock()
        self.log = open(args.log, "a", encoding="utf-8") if args.log else None
        self.shot = None

    def reopen(self):
        # The board's USB port vanishes across a reset; wait for it to return.
        while True:
            try:
                ser = serial.Serial()
                ser.port = self.args.port
                ser.baudrate = 115200
                ser.dtr = False
                ser.rts = False
                ser.timeout = 0.2
                ser.open()
                self.ser = ser
                return
            except serial.SerialException:
                time.sleep(0.3)

    def say(self, text):
        line = time.strftime("%H:%M:%S ") + text
        print(line, flush=True)
        if self.log:
            self.log.write(line + "\n")
            self.log.flush()

    def to_board(self, line):
        with self.lock:
            try:
                self.ser.write((line + "\n").encode())
            except serial.SerialException:
                pass  # the reader reopens the port

    def open_ws(self):
        if self.ws:
            try:
                self.ws.close()
            except Exception:
                pass
        self.ws = websocket.create_connection(self.args.hub, subprotocols=["valence.v1"], timeout=5)
        self.ws.settimeout(None)
        ws = self.ws
        threading.Thread(target=self.hub_reader, args=(ws,), daemon=True).start()
        self.say(f"[bridge] websocket open {self.args.hub}")

    def hub_reader(self, ws):
        while True:
            try:
                op, data = ws.recv_data()
            except Exception as e:
                if ws is self.ws:
                    self.say(f"[bridge] websocket closed: {e}")
                return
            if op == websocket.ABNF.OPCODE_BINARY:
                self.to_board("@V " + base64.b64encode(data).decode())

    def on_line(self, line):
        if line.startswith("@V "):
            frame = base64.b64decode(line[3:])
            try:
                if frame and frame[0] == HELLO or self.ws is None:
                    self.open_ws()
                self.ws.send_binary(frame)
            except Exception as e:
                self.say(f"[bridge] send failed: {e}")
                self.ws = None
        elif line.startswith("@S begin"):
            _, _, w, h = line.split()
            self.shot = (int(w), int(h), {})
        elif line.startswith("@S end") and self.shot:
            w, h, rows = self.shot
            png(self.args.shot, w, h, rows)
            self.say(f"[bridge] screenshot {w}x{h} -> {self.args.shot} ({len(rows)} rows)")
            self.shot = None
        elif line.startswith("@S ") and self.shot:
            _, y, _, b64 = line.split(" ", 3)
            self.shot[2][int(y)] = base64.b64decode(b64)
        else:
            self.say(line)

    def command_feeder(self):
        path = self.args.cmd_file
        pos = os.path.getsize(path) if path and os.path.exists(path) else 0
        while path:
            time.sleep(0.2)
            if not os.path.exists(path):
                continue
            with open(path, encoding="utf-8") as f:
                f.seek(pos)
                for cmd in f.read().splitlines():
                    if cmd.strip():
                        self.say(f"[bridge] > {cmd.strip()}")
                        self.to_board(cmd.strip())
                pos = f.tell()

    def stdin_feeder(self):
        for cmd in sys.stdin:
            if cmd.strip():
                self.to_board(cmd.strip())

    def run(self):
        threading.Thread(target=self.command_feeder, daemon=True).start()
        if sys.stdin and sys.stdin.isatty():
            threading.Thread(target=self.stdin_feeder, daemon=True).start()
        buf = b""
        while True:
            try:
                buf += self.ser.read(4096)
            except serial.SerialException:
                self.reopen()
                continue
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                try:
                    self.on_line(raw.decode("utf-8", "replace").rstrip("\r"))
                except Exception as e:  # one torn line never ends the bridge
                    self.say(f"[bridge] dropped a torn line: {e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--hub", default="ws://127.0.0.1:82/")
    ap.add_argument("--log")
    ap.add_argument("--shot", default="screen.png")
    ap.add_argument("--cmd-file")
    ap.add_argument("--reset", action="store_true", help="reset the board on open, to log its boot")
    Bridge(ap.parse_args()).run()


if __name__ == "__main__":
    main()
