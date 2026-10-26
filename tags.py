#!/usr/bin/env python3
"""Fix song tags (ID3) on a Tiny Tunes card with a spreadsheet.

The card goes in the Mac. Export every song's tags to a CSV, fix them in Numbers or
Excel, then apply: it shows what would change, and only writes when you say so.

    PY=~/.venvs/esp/bin/python
    $PY tags.py export "/Volumes/NO NAME" tags.csv     # one row per song
    $PY tags.py apply "/Volumes/NO NAME" tags.csv      # shows the changes, asks first
    $PY tags.py apply "/Volumes/NO NAME" tags.csv --yes

Columns: shelf, folder, file (which song; don't edit these), then title, artist, album,
album artist, track, disc. An empty cell removes that tag. Podcasts are left out: their
names come from the feed, and the next sync would replace them anyway. Each file keeps
its ID3 version (v2.3 or v2.4, both of which Tiny Tunes reads), its cover picture and
everything else; songs without tags get ID3v2.3.

After applying, eject the card and switch Tiny Tunes on: it reads the tags at boot.
(Needs mutagen: ~/.venvs/esp/bin/pip install mutagen)
"""
import argparse
import csv
import os
import subprocess
import sys

try:
    from mutagen.easyid3 import EasyID3  # reading
    from mutagen.id3 import ID3, ID3NoHeaderError, TALB, TIT2, TPE1, TPE2, TPOS, TRCK
except ImportError:
    sys.exit("Needs mutagen: ~/.venvs/esp/bin/pip install mutagen")

FIELDS = [("title", "title"), ("artist", "artist"), ("album", "album"),
          ("album artist", "albumartist"), ("track", "tracknumber"), ("disc", "discnumber")]
WHICH = ["shelf", "folder", "file"]
FRAME = {"title": TIT2, "artist": TPE1, "album": TALB, "albumartist": TPE2, "tracknumber": TRCK, "discnumber": TPOS}


def songs(card):
    """(shelf, folder, file, path) for every MP3 the device would play, podcasts aside."""
    for shelf in sorted(os.listdir(card), key=str.lower):
        top = os.path.join(card, shelf)
        if shelf.startswith(".") or shelf == "System Volume Information" or shelf.lower() == "podcasts":
            continue
        if os.path.isfile(top):
            if shelf.lower().endswith(".mp3"):
                yield "", "", shelf, top  # loose songs at the card's root
            continue
        for name in sorted(os.listdir(top), key=str.lower):
            p = os.path.join(top, name)
            if name.startswith("."):
                continue
            if os.path.isfile(p):
                if name.lower().endswith(".mp3"):
                    yield shelf, "", name, p  # loose songs in a shelf folder
                continue
            for f in sorted(os.listdir(p), key=str.lower):
                if f.lower().endswith(".mp3") and not f.startswith("."):
                    yield shelf, name, f, os.path.join(p, f)


def read_tags(path):
    try:
        t = EasyID3(path)
    except ID3NoHeaderError:
        return {key: "" for _, key in FIELDS}
    return {key: "; ".join(t.get(key, [])) for _, key in FIELDS}


def export(card, out):
    n = 0
    with open(out, "w", newline="", encoding="utf-8-sig") as f:  # the BOM makes Excel read UTF-8
        w = csv.writer(f)
        w.writerow(WHICH + [col for col, _ in FIELDS])
        for shelf, folder, name, path in songs(card):
            tags = read_tags(path)
            w.writerow([shelf, folder, name] + [tags[key] for _, key in FIELDS])
            n += 1
    print(f"Wrote {n} songs to {out}. Edit the tag columns, then: tags.py apply \"{card}\" {out}")


def apply(card, sheet, yes):
    with open(sheet, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    missing = [c for c in WHICH + [col for col, _ in FIELDS] if rows and c not in rows[0]]
    if missing:
        sys.exit(f"The sheet is missing these columns: {', '.join(missing)}")
    plan = []  # (path, {key: (old, new)})
    for r in rows:
        path = os.path.join(card, *[p for p in (r["shelf"], r["folder"], r["file"]) if p])
        if not os.path.isfile(path):
            print(f"  not on the card, skipped: {path}")
            continue
        old = read_tags(path)
        diff = {key: (old[key], (r[col] or "").strip()) for col, key in FIELDS if (r[col] or "").strip() != old[key]}
        if diff:
            plan.append((path, diff))
    if not plan:
        print("Nothing to change: the tags already match the sheet.")
        return
    names = {key: col for col, key in FIELDS}
    for path, diff in plan:
        print(os.path.relpath(path, card))
        for key, (a, b) in diff.items():
            print(f"    {names[key]}: {a or '(none)'!r} -> {b or '(removed)'!r}")
    print(f"\n{len(plan)} song(s) would change.")
    if not yes:
        try:
            answer = input("Write these tags? [y/N] ")
        except EOFError:  # nobody to ask
            answer = ""
        if answer.strip().lower() not in ("y", "yes"):
            print("Nothing written. (--yes writes without asking.)")
            return
    failed = []
    for path, diff in plan:
        try:
            t = ID3(path)
            version = 4 if t.version[1] >= 4 else 3  # keep the file's own ID3 version
        except ID3NoHeaderError:
            t, version = ID3(), 3
        for key, (_, new) in diff.items():
            t.delall(FRAME[key].__name__)
            if new:
                t.add(FRAME[key](encoding=3, text=new))  # UTF-8 (v2.3 files get UTF-16, which it can store)
        try:
            t.save(path, v2_version=version)
        except ValueError:
            # Some taggers leave broken frames that can't be written back (LibriVox files
            # carry a LINK frame with a garbled ID): drop those and try again.
            for k in list(t.keys()):
                if k.startswith("LINK") or not k[:4].isalnum() or "\x00" in k:
                    del t[k]
            try:
                t.save(path, v2_version=version)
            except ValueError as e:
                failed.append(f"{os.path.relpath(path, card)}: {e}")
    print(f"Wrote {len(plan) - len(failed)} song(s).")
    for f in failed:
        print(f"  couldn't write {f}")
    if sys.platform == "darwin":  # macOS can leave ._ files on FAT cards; Tiny Tunes ignores them, but tidy anyway
        subprocess.run(["dot_clean", "-m", card], check=False)
    print("Eject the card and switch Tiny Tunes on to see the new names.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export", help="write every song's tags to a CSV")
    e.add_argument("card")
    e.add_argument("csv", nargs="?", default="tags.csv")
    a = sub.add_parser("apply", help="write the CSV's tags back into the songs (shows the changes first)")
    a.add_argument("card")
    a.add_argument("csv")
    a.add_argument("--yes", action="store_true", help="don't ask before writing")
    args = ap.parse_args()
    if not os.path.isdir(args.card):
        sys.exit(f"{args.card} isn't a folder. Is the SD card in the Mac?")
    if args.cmd == "export":
        export(args.card, args.csv)
    else:
        apply(args.card, args.csv, args.yes)


if __name__ == "__main__":
    main()
