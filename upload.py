#!/usr/bin/env python3
"""Manage music on the Core2's SD card (Tiny Tunes must be running).

Albums are folders named "Artist - Album" holding the MP3s and an optional
cover.jpg. Pass a folder from your Mac and it becomes an album; the cover comes
from an image in the folder, or from art embedded in the MP3s, shrunk to 160x160.

    PY=~/.venvs/esp/bin/python
    $PY upload.py "~/Music/Fleetwood Mac - Rumours"        # upload an album folder
    $PY upload.py --album "Kids Songs" a.mp3 b.mp3          # loose files into an album
    $PY upload.py --shelf books "~/Audiobooks/Charlotte's Web"  # an audiobook (resumes where it stopped)
    $PY upload.py --ls                                      # what's on the card
    $PY upload.py --mv "/music/Landslide.mp3" "/Fleetwood Mac - The Dance/Landslide.mp3"
    $PY upload.py --rm /music                               # delete a file or album
    $PY upload.py --wifi MyNetwork                          # set up WiFi (prompts for password)
    $PY upload.py --mode bt                                 # reboot into Bluetooth / WiFi mode
    $PY upload.py --name Ava                                # whose Tiny Tunes it is ("" to clear)
    $PY upload.py --prepare /Volumes/CARD                   # card in your Mac: make cover.jpg files

Uploads go over WiFi (fast) when the Core2 is in WiFi mode, else over USB.
Every run also sets the Core2's clock from this Mac.
"""
import argparse
import getpass
import glob
import http.client
import os
import re
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.parse
import uuid

import serial

CHUNK = 2048  # must match UP_CHUNK in tiny-tunes.ino
BAUD = 460800
COVER_PX = 160
IMAGE_EXTS = (".jpg", ".jpeg", ".png", ".heic", ".webp")


# ---------- serial link ----------
def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*"))
    if not ports:
        sys.exit("No Core2 serial port found. Is it plugged in?")
    return ports[0]


def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, BAUD, 1
    s.dtr = s.rts = False  # don't toggle the auto-reset lines on open
    s.open()
    return s


def expect(s, *wanted, timeout=10):
    """Read lines until one starts with a wanted token; skip log noise."""
    end = time.time() + timeout
    while time.time() < end:
        line = s.readline().decode(errors="replace").strip()
        if not line:
            continue
        if line.startswith("ERR"):
            raise RuntimeError(line)
        if line.split(" ")[0] in wanted:
            return line
    raise TimeoutError(f"timed out waiting for {wanted}")


def command(s, line, *wanted, timeout=10):
    s.write((line + "\n").encode())
    return expect(s, *(wanted or ("OK",)), timeout=timeout)


def ping(s):
    """Returns (mode, ip). Opening the port reboots the Core2, so retry until it's up."""
    for _ in range(20):
        s.reset_input_buffer()
        s.write(b"PING\n")
        try:
            parts = expect(s, "PONG", timeout=1).split()
            return parts[1], (parts[2] if len(parts) > 2 else None)
        except TimeoutError:
            pass
    sys.exit("Core2 didn't answer. Is Tiny Tunes running with an SD card inserted?")


def sync_clock(s):
    t = time.localtime()
    command(s, f"TIME {t.tm_year} {t.tm_mon} {t.tm_mday} {t.tm_hour} {t.tm_min} {t.tm_sec} {(t.tm_wday + 1) % 7}")


def list_files(s):
    s.write(b"LS\n")
    files = {}
    while True:
        line = expect(s, "F", "END")
        if line == "END":
            return files
        _, rest = line.split(" ", 1)
        path, size = rest.rsplit(" ", 1)
        files[path] = int(size)


# ---------- names ----------
def safe_name(name):
    """ASCII only (the Core2's font and FAT setup), no characters FAT can't store."""
    name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()
    return re.sub(r'[\\/:*?"<>|]', "_", name).strip() or "untitled"


# ---------- covers ----------
def embedded_art(mp3_path):
    """Returns the first APIC picture from an ID3v2.3/2.4 tag, or None."""
    with open(mp3_path, "rb") as f:
        head = f.read(10)
        if len(head) < 10 or head[:3] != b"ID3" or head[3] not in (3, 4):
            return None
        size = (head[6] << 21) | (head[7] << 14) | (head[8] << 7) | head[9]
        tag = f.read(size)
    ver, pos = head[3], 0
    while pos + 10 <= len(tag):
        fid, raw = tag[pos:pos + 4], tag[pos + 4:pos + 8]
        if not fid.strip(b"\0"):
            break
        fsize = ((raw[0] << 21) | (raw[1] << 14) | (raw[2] << 7) | raw[3]) if ver == 4 else int.from_bytes(raw, "big")
        body = tag[pos + 10:pos + 10 + fsize]
        pos += 10 + fsize
        if fid != b"APIC" or len(body) < 4:
            continue
        enc, i = body[0], body.index(b"\0", 1) + 1  # skip MIME type
        i += 1  # picture type
        term = b"\0\0" if enc in (1, 2) else b"\0"  # skip description
        j = body.find(term, i)
        if enc in (1, 2):
            while j != -1 and (j - i) % 2:
                j = body.find(term, j + 1)
        return body[j + len(term):] if j != -1 else None
    return None


def make_cover(folder, mp3s):
    """Finds or extracts album art and returns a 160px baseline JPEG as bytes, or None."""
    images = sorted(f for f in os.listdir(folder) if f.lower().endswith(IMAGE_EXTS) and not f.startswith("."))
    preferred = [f for f in images if os.path.splitext(f)[0].lower() in ("cover", "folder", "front", "album")]
    with tempfile.TemporaryDirectory() as tmp:
        src = None
        if preferred or images:
            src = os.path.join(folder, (preferred or images)[0])
        else:
            for mp3 in mp3s:
                art = embedded_art(mp3)
                if art:
                    src = os.path.join(tmp, "art")
                    with open(src, "wb") as f:
                        f.write(art)
                    break
        if not src:
            return None
        out = os.path.join(tmp, "cover.jpg")
        r = subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "85",
                            "-Z", str(COVER_PX), src, "--out", out], capture_output=True)
        if r.returncode != 0 or not os.path.exists(out):
            print(f"  (couldn't convert cover {os.path.basename(src)})")
            return None
        with open(out, "rb") as f:
            return f.read()


# ---------- transfers ----------
class Progress:
    def __init__(self, name, total):
        self.name, self.total, self.sent, self.t0, self.last = name, total, 0, time.time(), 0

    def add(self, n):
        self.sent += n
        now = time.time()
        if now - self.last > 0.2 or self.sent >= self.total:
            self.last = now
            rate = self.sent / max(now - self.t0, 1e-3) / 1024
            print(f"\r  {self.name}: {self.sent * 100 // max(self.total, 1)}%  {rate:.0f} KB/s   ", end="", flush=True)


def put_serial(s, data, remote, label):
    s.write(f"PUT {remote} {len(data)}\n".encode())
    expect(s, "OK")
    p = Progress(label, len(data))
    for i in range(0, len(data), CHUNK):
        s.write(data[i:i + CHUNK])
        expect(s, "A")
        p.add(len(data[i:i + CHUNK]))
    expect(s, "DONE", timeout=30)
    print()


def put_http(ip, data, folder, name, label):
    boundary = uuid.uuid4().hex
    pre = (f'--{boundary}\r\nContent-Disposition: form-data; name="f"; filename="{name}"\r\n'
           f"Content-Type: application/octet-stream\r\n\r\n").encode()
    post = f"\r\n--{boundary}--\r\n".encode()
    conn = http.client.HTTPConnection(ip, 80, timeout=60)
    conn.putrequest("POST", "/upload?dir=" + urllib.parse.quote(folder))
    conn.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
    conn.putheader("Content-Length", str(len(pre) + len(data) + len(post)))
    conn.endheaders()
    conn.send(pre)
    p = Progress(label, len(data))
    for i in range(0, len(data), 16384):
        conn.send(data[i:i + 16384])
        p.add(len(data[i:i + 16384]))
    conn.send(post)
    r = conn.getresponse()
    print()
    if r.status != 200:
        raise RuntimeError(f"upload failed: {r.status} {r.read().decode(errors='replace')}")


def collect(args):
    """Yields (album, [mp3 paths], folder or None) for each thing to upload."""
    loose = []
    for item in args.files:
        item = os.path.expanduser(item)
        if os.path.isdir(item):
            mp3s = sorted(os.path.join(item, f) for f in os.listdir(item)
                          if f.lower().endswith(".mp3") and not f.startswith("."))
            if not mp3s:
                print(f"No MP3s in {item}, skipping")
                continue
            yield safe_name(args.album or os.path.basename(item.rstrip("/"))), mp3s, item
        elif item.lower().endswith(".mp3"):
            loose.append(item)
        else:
            print(f"Skipping {item} (not an MP3 or folder)")
    if loose:
        yield safe_name(args.album) if args.album else "", loose, None


def prepare(card):
    """Card mounted on this Mac: writes a small cover.jpg into every album folder that needs one."""
    if not os.path.isdir(card):
        sys.exit(f"{card} isn't a folder. Is the SD card mounted?")
    made = 0
    for root, dirs, files in os.walk(card):
        dirs[:] = [d for d in dirs if not d.startswith(".") and d != "System Volume Information"]
        if root[len(card):].count(os.sep) > 2:
            continue
        mp3s = sorted(os.path.join(root, f) for f in files if f.lower().endswith(".mp3") and not f.startswith("."))
        if not mp3s:
            continue
        target = os.path.join(root, "cover.jpg")
        name = os.path.relpath(root, card)
        if os.path.exists(target) and os.path.getsize(target) < 100_000:
            print(f"  {name}: cover.jpg already OK")
            continue
        cover = make_cover(root, mp3s)
        if not cover:
            print(f"  {name}: no cover art found")
            continue
        if os.path.exists(target):  # keep a big original under another name
            os.replace(target, os.path.join(root, "cover-original.jpg"))
        with open(target, "wb") as f:
            f.write(cover)
        made += 1
        print(f"  {name}: made cover.jpg ({len(cover) // 1024} KB)")
    print(f"Done. Made {made} cover(s). Eject the card and put it back in the Core2.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="album folders and/or MP3 files")
    ap.add_argument("--album", help='album folder name for loose MP3s, e.g. "Artist - Album" (default: card root)')
    ap.add_argument("--shelf", help='shelf folder to upload into, e.g. "books" (default: music)')
    ap.add_argument("--ls", action="store_true", help="list what's on the card")
    ap.add_argument("--mv", nargs=2, metavar=("FROM", "TO"), help="move/rename a file or album on the card")
    ap.add_argument("--rm", metavar="PATH", help="delete a file or a whole album folder on the card")
    ap.add_argument("--wifi", metavar="SSID", help="save WiFi credentials and switch to WiFi mode")
    ap.add_argument("--mode", choices=("bt", "wifi"), help="reboot into Bluetooth or WiFi mode")
    ap.add_argument("--name", metavar="NAME", help='the owner\'s first name, shown on the device ("" clears it)')
    ap.add_argument("--usb", action="store_true", help="upload over USB even in WiFi mode")
    ap.add_argument("--prepare", metavar="CARD", help="SD card mounted on this Mac: make cover.jpg files")
    ap.add_argument("--port")
    args = ap.parse_args()

    if args.prepare:  # works on the card directly; no Core2 needed
        prepare(os.path.expanduser(args.prepare))
        return

    s = open_port(args.port or find_port())
    mode, ip = ping(s)
    sync_clock(s)

    if args.wifi:
        password = getpass.getpass(f"Password for {args.wifi}: ")
        command(s, f"WIFI {args.wifi}\t{password}")
        print("Saved. Switching to WiFi mode and connecting...")
        command(s, "MODE wifi")
        try:
            print(f"Connected: http://{expect(s, 'IP', timeout=45).split()[1]}")
        except TimeoutError:
            print("No connection after 45 s. Check the password, and that the network is 2.4 GHz.")
        return
    if args.name is not None:
        command(s, f"NAME {args.name.strip() or '-'}")  # "-" clears it
        print(f"Named it {args.name.strip()}'s Tiny Tunes." if args.name.strip() else "Cleared the owner's name.")
        return
    if args.mode:
        command(s, f"MODE {args.mode}")
        print(f"Switching to {'Bluetooth' if args.mode == 'bt' else 'WiFi'} mode.")
        return
    if args.mv:
        command(s, f"MV {args.mv[0]}\t{args.mv[1]}")
        command(s, "RESCAN")
        print(f"Moved {args.mv[0]} -> {args.mv[1]}")
        return
    if args.rm:
        command(s, f"RM {args.rm}")
        command(s, "RESCAN")
        print(f"Deleted {args.rm}")
        return

    existing = list_files(s)
    if args.ls or not args.files:
        for path, size in sorted(existing.items(), key=lambda kv: kv[0].lower()):
            print(f"{size / 1e6:8.1f} MB  {path}")
        version = command(s, "VER", "VER").split()[1]
        print(f"\nTiny Tunes {version}, in {'WiFi' if mode == 'wifi' else 'Bluetooth'} mode" + (f" at http://{ip}" if ip else ""))
        return

    use_http = mode == "wifi" and ip and not args.usb
    print(f"Uploading over {'WiFi to ' + ip if use_http else 'USB'}")

    # new albums go next to the existing ones: /music/<album> if the card uses /music,
    # or into another shelf (e.g. --shelf books -> /books/<book>)
    if args.shelf and args.shelf.strip("/").lower() != "music":
        root = "/" + safe_name(args.shelf.strip("/").lower())
    else:
        root = "/music" if any(p.startswith("/music/") for p in existing) else ""

    def send(data, album, name):
        remote = root + (f"/{album}" if album else "") + f"/{name}"
        if existing.get(remote) == len(data):
            print(f"  {name}: already on card")
            return
        if use_http:
            put_http(ip, data, root + (f"/{album}" if album else "") or "/", name, name)
        else:
            put_serial(s, data, remote, name)

    for album, mp3s, folder in collect(args):
        print(f"{album or 'Songs (card root)'}:")
        if folder:
            cover = make_cover(folder, mp3s)
            if cover:
                send(cover, album, "cover.jpg")
            else:
                print("  (no cover art found)")
        for mp3 in mp3s:
            with open(mp3, "rb") as f:
                send(f.read(), album, safe_name(os.path.basename(mp3)))

    print(f"Done. {command(s, 'RESCAN', 'OK', timeout=30).split()[1]} albums on the card.")


if __name__ == "__main__":
    main()
