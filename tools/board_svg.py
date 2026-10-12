#!/usr/bin/env python3
"""Render boards/<board>/board.svg from its manifest.json (the one home of the geometry).

Usage: python tools/board_svg.py boards/waveshare_s3_43/manifest.json
Constraint: drawn from the manifest numbers only; no vendor artwork.
"""
import json
import pathlib
import sys
from xml.sax.saxutils import escape

COLORS = {"button": "#e0a030", "encoder": "#30a0e0", "encoder_push": "#30a0e0",
          "dial": "#30a0e0", "switch": "#a060e0", "touch_zone": "#40b070"}


def rect(r, fill, stroke, extra=""):
    return (f'<rect x="{r["x"]}" y="{r["y"]}" width="{r["w"]}" height="{r["h"]}" rx="{r.get("r", 4)}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="3" {extra}/>')


def label(r, text, color):
    return (f'<text x="{r["x"] + r["w"] / 2}" y="{r["y"] + r["h"] / 2 + 7}" font-family="sans-serif" '
            f'font-size="20" text-anchor="middle" fill="{color}">{escape(text)}</text>')


def main(path):
    m = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    img = m["image"]
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {img["width"]} {img["height"]}" '
           f'width="{img["width"]}" height="{img["height"]}">',
           f'<title>{escape(m["name"])} ({escape(m["view"])})</title>',
           rect(m["outline"], "#1d2a22", "#6a7a70"),
           rect(m["screen"], "#05070a", "#3a4450")]
    for f in m.get("features", []):
        out += [rect(f, "#2c3530", "#6a7a70"), label(f, f["label"], "#9aa8a0")]
    for i in m["inputs"]:
        c = COLORS.get(i["kind"], "#ffffff")
        fill = "none" if i["kind"] == "touch_zone" else c
        out.append(rect(i, fill, c, 'stroke-dasharray="12 8"' if i["kind"] == "touch_zone" else ""))
        out.append(label(i, f'{i["label"]} [{i["id"]}]', "#101010" if fill != "none" else c))
    out.append("</svg>\n")
    dst = pathlib.Path(path).parent / img["file"]
    dst.write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(dst)


if __name__ == "__main__":
    main(sys.argv[1])
