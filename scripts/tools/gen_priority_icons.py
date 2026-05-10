"""One-shot: write Scripts/art/priority/<slug>.png fallback icons."""
from __future__ import annotations

import struct
import zlib
from pathlib import Path


def chunk(typ: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(typ + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + typ + data + struct.pack(">I", crc)


def write_png(path: Path, pixels: list[tuple[int, int, int, int]], w: int = 16, h: int = 16) -> None:
    sig = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            row.extend(pixels[y * w + x])
        rows.append(bytes(row))
    raw = b"".join(rows)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    idat = zlib.compress(raw, 9)
    data = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def mix(a: int, b: int, t: float) -> int:
    return max(0, min(255, int(a + (b - a) * t)))


def lighten(rgb: tuple[int, int, int], amount: float) -> tuple[int, int, int]:
    return tuple(mix(c, 255, amount) for c in rgb)


def darken(rgb: tuple[int, int, int], amount: float) -> tuple[int, int, int]:
    return tuple(mix(c, 0, amount) for c in rgb)


def icon_pixels(base: tuple[int, int, int], glyph: str) -> list[tuple[int, int, int, int]]:
    w = h = 16
    hi = lighten(base, 0.22)
    lo = darken(base, 0.25)
    pixels: list[tuple[int, int, int, int]] = []
    for y in range(h):
        t = y / (h - 1)
        rgb = tuple(mix(hi[i], lo[i], t) for i in range(3))
        for x in range(w):
            a = 255 if 1 <= x <= 14 and 1 <= y <= 14 else 0
            pixels.append((*rgb, a))

    def setpx(x: int, y: int, rgba: tuple[int, int, int, int]) -> None:
        if 0 <= x < w and 0 <= y < h:
            pixels[y * w + x] = rgba

    white = (255, 255, 255, 245)
    shadow = (0, 0, 0, 70)
    if glyph in ("up2", "up1"):
        for dy in range(6):
            half = dy // 2 + (1 if glyph == "up2" else 0)
            cy = 3 + dy
            for x in range(8 - half, 9 + half):
                setpx(x, cy + 1, shadow)
                setpx(x, cy, white)
        if glyph == "up2":
            for y in range(8, 13):
                for x in range(6, 11):
                    setpx(x, y + 1, shadow)
                    setpx(x, y, white)
    elif glyph in ("down2", "down1"):
        for dy in range(6):
            half = (5 - dy) // 2 + (1 if glyph == "down2" else 0)
            cy = 7 + dy
            for x in range(8 - half, 9 + half):
                setpx(x, cy + 1, shadow)
                setpx(x, cy, white)
        if glyph == "down2":
            for y in range(3, 8):
                for x in range(6, 11):
                    setpx(x, y + 1, shadow)
                    setpx(x, y, white)
    elif glyph == "bar":
        for y in range(6, 10):
            for x in range(3, 13):
                setpx(x, y + 1, shadow)
                setpx(x, y, white)
    elif glyph == "dot":
        for y in range(5, 11):
            for x in range(5, 11):
                if (x - 8) * (x - 8) + (y - 8) * (y - 8) <= 10:
                    setpx(x, y + 1, shadow)
                    setpx(x, y, white)
    else:  # blocker / critical
        for y in range(3, 13):
            for x in range(6, 10):
                setpx(x, y + 1, shadow)
                setpx(x, y, white)
        for x in range(3, 13):
            for y in range(6, 10):
                setpx(x, y + 1, shadow)
                setpx(x, y, white)
    return pixels


def main() -> None:
    # parents[2] = repo root when this file is at <repo>/scripts/tools/...
    repo = Path(__file__).resolve().parents[2]
    root = repo / "scripts" / "art" / "priority"
    icons = {
        "blocker": ((120, 0, 0), "stop"),
        "critical": ((200, 30, 30), "stop"),
        "high": ((230, 120, 0), "up1"),
        "highest": ((180, 0, 90), "up2"),
        "low": ((50, 100, 200), "down1"),
        "lowest": ((80, 110, 155), "down2"),
        "major": ((120, 60, 180), "up1"),
        "medium": ((210, 170, 40), "bar"),
        "minor": ((40, 140, 80), "down1"),
        "trivial": ((150, 160, 170), "dot"),
    }
    for name, (rgb, glyph) in icons.items():
        write_png(root / f"{name}.png", icon_pixels(rgb, glyph))
    print("wrote", len(icons), "files to", root)


if __name__ == "__main__":
    main()
