# Tiny Tunes

A kids' audio player for the [M5Stack Core2](https://docs.m5stack.com/en/core/core2): music,
audiobooks and podcasts from a microSD card, played on the speaker or Bluetooth headphones.
Built for 5-10 year olds: big pictures, few words, no browsing the internet, and
everything on it chosen by a grown-up.

| Home | Albums | Songs | Player |
|---|---|---|---|
| ![Home: Keep listening, then Pods, Music and Books](docs/screenshots/home.png) | ![Albums: a carousel of covers](docs/screenshots/albums.png) | ![Songs: the album's cover and its songs](docs/screenshots/songs.png) | ![The player](docs/screenshots/player.png) |

## Getting started

**You need:**

- An [M5Stack Core2](https://docs.m5stack.com/en/core/core2) (the original, with the
  AXP192 power chip; the Core2 v1.1 is untested, and switching off talks to the AXP192
  directly).
- A microSD card of 32 GB or less, formatted FAT32 (most cards that size come that way).
- A Mac with a USB-C cable that carries data. The scripts look for the Core2 at
  `/dev/cu.usbserial*` or `/dev/cu.wchusbserial*`.
- Optional: Bluetooth headphones or a speaker, and a WiFi network for podcasts.

**1. Install the tools** (once). `flash.sh` expects `arduino-cli` in `~/.local/bin` and the
Python tools in `~/.venvs/esp`:

```sh
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
~/.local/bin/arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
~/.local/bin/arduino-cli core install esp32:esp32@3.3.12
~/.local/bin/arduino-cli lib install M5Unified ESP8266Audio
git clone https://github.com/pschatzmann/ESP32-A2DP ~/Documents/Arduino/libraries/ESP32-A2DP
python3 -m venv ~/.venvs/esp && ~/.venvs/esp/bin/pip install esptool pyserial mutagen
```

**2. Get the code and flash it.** Plug in the Core2, then:

```sh
git clone https://github.com/derek/tiny-tunes.git && cd tiny-tunes
./flash.sh
```

That builds both firmwares (Bluetooth and WiFi mode, a few minutes the first time) and
writes them to the Core2, which then restarts into Bluetooth mode.

**3. Put something on the card.** With the card in the Mac, make a `music` folder and
copy album folders into it (see [Adding music, books and podcasts](#adding-music-books-and-podcasts)
for the layout), then put the card in the Core2 and switch it on. Or leave the card in
the Core2 and send albums over USB:

```sh
~/.venvs/esp/bin/python upload.py "~/Music/Some Artist - Some Album"
```

**4. Make it theirs** (optional):

```sh
~/.venvs/esp/bin/python upload.py --name Ava       # "Hi, Ava!" on the splash screen
~/.venvs/esp/bin/python upload.py --wifi MyNetwork # WiFi for podcasts and the web page (asks for the password,
                                                   # then switches to WiFi mode)
```

To pair headphones, hold two of the touch buttons below the screen for 3 seconds, open
the Bluetooth tab, put the headphones in pairing mode and tap them in the list.

## For grown-ups

Hold two of the touch buttons below the screen for 3 seconds for the grown-ups screen:
the loudest the volume goes, brightness, auto-off, the colors (Forest, Navy, Ocean,
Sunset, Midnight, and the light Candy and Daylight), Bluetooth pairing, switching to WiFi
mode, and stats.

**Bluetooth and WiFi.** The ESP32 can't fit Bluetooth audio and WiFi at once, so Tiny Tunes
is two firmwares in the two app slots, and switching between them reboots (a couple of
seconds; the card and settings are shared):

- **Bluetooth mode** (normal): plays to paired headphones or speakers, or the speaker.
- **WiFi mode:** a web page for adding and organizing music, books and podcasts; podcast
  downloads; the clock set from the internet.

Every night between midnight and 5 am, if it's plugged in and nobody is using it, it
switches to WiFi by itself, downloads new podcast episodes, and switches back.

**A name.** `upload.py --name Ava` (or the web page's Settings) puts "Hi, Ava!" on the
splash screen and makes the web page `http://ava-tunes.local` in WiFi mode.

## Adding music, books and podcasts

The card is FAT32. Each top-level folder is a shelf, and each folder in it is an album:

```
/music/Fleetwood Mac - Rumours/01 Dreams.mp3
/music/Fleetwood Mac - Rumours/cover.jpg              (optional)
/books/The Wonderful Wizard of Oz/01 The Cyclone.mp3
/podcasts/Circle Round/2026-10-21 The Clever Fox.mp3  (managed by the podcast sync)
```

Names come from the songs' ID3 tags (title, artist, album, track and disc numbers) where
they have them, and from the file and folder names otherwise. Covers come from a
`cover.jpg` / `.png` in the folder, or from the picture inside the songs. Books and podcasts
remember where they were.

Three ways to add things:

- **Card reader:** copy the folders over. Changes show up the next time it's switched on.
- **The web page** (WiFi mode): drag in MP3s and a cover; rename, move and delete; Clean
  up tidies names and makes missing covers; add podcast feeds (RSS or Apple Podcasts
  links) and choose how many episodes to keep.
- **`upload.py`** over USB (or WiFi when the device is in WiFi mode):

```sh
PY=~/.venvs/esp/bin/python
$PY upload.py "~/Music/Some Artist - Some Album"           # an album folder
$PY upload.py --shelf books "~/Audiobooks/Charlotte's Web"   # a book
$PY upload.py --ls                                         # what's on the card
$PY upload.py --wifi MyNetwork                             # WiFi setup (asks for the password)
$PY upload.py --mode wifi                                  # switch to WiFi (or bt) mode
$PY upload.py --name Ava                                   # whose Tiny Tunes it is
```

Each run of `upload.py` also sets the device's clock from the computer.

**Fixing names.** If songs show the wrong title, artist or album, fix their tags with a
spreadsheet (the card in the Mac):

```sh
$PY tags.py export "/Volumes/NO NAME" tags.csv   # every song's tags, one row each
# edit tags.csv in Numbers or Excel
$PY tags.py apply "/Volumes/NO NAME" tags.csv    # shows what would change, then asks
```

(`~/.venvs/esp/bin/pip install mutagen` once first.)

## Building and flashing

The tools are in [Getting started](#getting-started) (tested with ESP32 core 3.3.12,
M5Unified 0.2.27 and ESP8266Audio 2.4.1).

```sh
./flash.sh                     # build both firmwares and flash them to the Core2 on USB
./flash.sh /dev/cu.usbserial-X # a particular one, when two are plugged in
./flash.sh --no-build          # flash the last build again
```

`test/run.sh` runs the tests for the title and tag code on the computer (needs a C++17
compiler, such as the one in Xcode's command line tools).

The version (grown-ups → Stats, and the web page's footer) is `VERSION` plus the build
number and commit, e.g. `0.9.3+32.08831bd`.

## Files

- `tiny-tunes.ino` and the numbered `.ino` tabs next to it: the firmware, both modes
  (`#if CORE2_WIFI`). The map of which tab holds what is at the top of `tiny-tunes.ino`;
  the USB serial protocol is documented in `70_serial.ino`.
- `web/index.html`: the WiFi mode web page (HTML, CSS and script). `flash.sh` turns it
  into `tt_web_page.h` (git-ignored) for the firmware.
- `tt_text.h`: title clean-up and the ID3 tag reader, kept apart so `test/run.sh` can
  test them on a computer.
- `upload.py`: uploads, WiFi setup, mode switching, naming.
- `tags.py`: export song tags to a spreadsheet and write the edits back.
- `screenshot.py`: screenshots from the device's display (`--tour docs/screenshots` makes
  the ones above).
- `flash.sh`: builds both firmwares and flashes them.
- `CHANGELOG.md`: what changed in each version.
- `AGENTS.md`: working notes and constraints for AI coding agents.
