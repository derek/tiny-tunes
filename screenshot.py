#!/usr/bin/env python3
"""Screenshots of Tiny Tunes, read back from the Core2's display over USB.

    PY=~/.venvs/esp/bin/python
    $PY screenshot.py shot.png                      # whatever is on the screen now
    $PY screenshot.py --tour docs/screenshots       # the README's set: home, albums, songs, player, book
    $PY screenshot.py --tour docs/screenshots --music "Rumours" --book "Wizard"

Opening the serial port restarts the device, so --tour waits for it to boot, then
picks the albums (paused, no sound) and screens itself.
"""
import argparse
import os
import struct
import sys
import time
import zlib

import serial

BAUD = 460800
W, H = 320, 240


def find_port():
    import glob
    ports = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*"))
    if not ports:
        sys.exit("No Core2 found on USB.")
    return ports[0]


def wait_ready(s, timeout=40):
    end = time.time() + timeout
    while time.time() < end:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("READY"):
            return line
    sys.exit("The Core2 didn't finish booting.")


def command(s, line, timeout=10):
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    end = time.time() + timeout
    while time.time() < end:
        r = s.readline().decode(errors="replace").strip()
        if r == "OK":
            return
        if r.startswith("ERR"):
            sys.exit(f"{line}: {r}")
    sys.exit(f"{line}: no answer")


def grab(s):
    """The screen as rows of (r, g, b)."""
    s.reset_input_buffer()
    s.write(b"SHOT\n")
    end = time.time() + 10
    while time.time() < end:
        if s.readline().decode(errors="replace").strip() == "SHOT 320 240":
            break
    else:
        sys.exit("SHOT: no answer")
    raw = bytearray()
    while len(raw) < W * H * 2 and time.time() < end + 20:
        raw += s.read(W * H * 2 - len(raw))
    if len(raw) < W * H * 2:
        sys.exit(f"SHOT: got {len(raw)} of {W * H * 2} bytes")
    # The display hands back RGB565 in one byte order or the other: the screens are mostly
    # the dark blue background (18, 22, 48), so take whichever order makes it so.
    def decode(big):
        px = []
        for i in range(0, len(raw), 2):
            v = (raw[i] << 8 | raw[i + 1]) if big else (raw[i + 1] << 8 | raw[i])
            px.append(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
        return px
    def bgness(px):
        return sum(1 for p in px[::97] if abs(p[0] - 16) < 12 and abs(p[1] - 20) < 12 and abs(p[2] - 48) < 12)
    a, b = decode(True), decode(False)
    return a if bgness(a) >= bgness(b) else b


def write_png(path, px, scale=2):
    """A PNG with only the standard library; scale 2 keeps the pixels crisp on retina screens."""
    w, h = W * scale, H * scale
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        src = px[(y // scale) * W:(y // scale + 1) * W]
        for p in src:
            rows += bytes(p) * scale
    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)
    print(f"  {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="a .png file, or with --tour a folder")
    ap.add_argument("--tour", action="store_true", help="take the README's set of screens")
    ap.add_argument("--music", default="", help="part of a music album's name (default: what's loaded)")
    ap.add_argument("--book", default="", help="part of a book's name, for the book player shot")
    ap.add_argument("--port")
    args = ap.parse_args()

    s = serial.Serial(args.port or find_port(), BAUD, timeout=1)
    wait_ready(s)  # opening the port restarted it
    time.sleep(1.5)
    if not args.tour:
        write_png(args.out, grab(s))
        return
    if args.music:
        command(s, f"LOAD {args.music}")
    shots = [("home", "SHOW home"), ("albums", "SHOW albums"), ("songs", "SHOW songs"), ("player", "SHOW player")]
    for name, cmd in shots:
        command(s, cmd)
        time.sleep(1.5)  # let it draw (the song list's cover animation too)
        write_png(os.path.join(args.out, f"{name}.png"), grab(s))
    if args.book:
        command(s, f"LOAD {args.book}")
        command(s, "SHOW player")
        time.sleep(1.5)
        write_png(os.path.join(args.out, "book.png"), grab(s))


if __name__ == "__main__":
    main()
