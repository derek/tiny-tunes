# Tiny Tunes

A kids' audio player for the [M5Stack Core2](https://docs.m5stack.com/en/core/core2)
(ESP32, 320×240 touchscreen, speaker, battery, microSD): music, podcasts and audiobooks.
Built for a 5- and a 7-year-old: big picture buttons, no internet browsing, everything
chosen by a grown-up.

The middle touch button below the screen is **Home**; a green bar along the bottom of
the library screens goes to what's playing, and round back buttons go up a level.
Home is the shelves (Podcasts, Music, Books)
as a list of wide cards; then albums as tall cards, three at a time; then the songs.
The player keeps it simple: the title, big buttons, and a volume button with 5 steps.
Books and podcasts skip 15 s back / 30 s ahead and resume where they stopped.
Snackbars at the bottom say when the battery is low, it's charging, or headphones connect.

A quick tap on the power button turns the screen off (the music keeps playing) and on
again; holding it for a second switches Tiny Tunes off.

Holding two of the touch buttons together for 3 seconds opens the grown-ups screen
(loudest volume, brightness, auto-off, Bluetooth pairing, WiFi, and stats).

## Two firmware modes

The precompiled ESP32 Arduino core can't fit Bluetooth audio and WiFi in IRAM at
the same time, so the sketch builds twice and both copies live in the two app slots:

- **Bluetooth mode** (default): plays to paired headphones/speakers, or the speaker.
- **WiFi mode** (`-DCORE2_WIFI=1`): clock sync, a web page for adding and organizing
  music, podcast downloads.

Switching (grown-ups screen, or `upload.py --mode`) reboots into the other slot in a
couple of seconds. The SD card and settings are shared. The Bluetooth build is within
a few hundred bytes of its IRAM limit: libc time/ctype helpers, `rand()`, M5Unified's
`powerOff()` and its Mic class all cost IRAM, which is why the code avoids them.

## Setup

Requires [arduino-cli](https://arduino.github.io/arduino-cli/), the Espressif ESP32 core
(tested with 3.3.12) and these libraries:

```sh
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32
arduino-cli lib install M5Unified ESP8266Audio
git clone https://github.com/pschatzmann/ESP32-A2DP ~/Documents/Arduino/libraries/ESP32-A2DP
python3 -m venv ~/.venvs/esp && ~/.venvs/esp/bin/pip install esptool pyserial
```

## Build and flash

```sh
./flash.sh            # builds both modes, flashes Bluetooth to app0 and WiFi to app1
./flash.sh --no-build # flash the last build
```

The Core2's USB-serial chip is unreliable at 921600 baud, so flashing and the serial
link use 460800.

## Music on the SD card

FAT32 microSD. Albums are folders, at the card root or inside `/music`:

```
/music/Fleetwood Mac - Rumours/01 Dreams.mp3
/music/Fleetwood Mac - Rumours/cover.jpg       (or .png; optional)
/podcasts/<Show>/2025-10-21 Episode title.mp3  (managed by the podcast sync)
```

Song titles are cleaned for display (track numbers, "Artist -" prefixes and
"(Remastered)"-style tags are dropped). Covers are decoded once and cached next to
them as `.cover.rgb565`.

Ways to add music:

- **Card reader:** copy folders, then `upload.py --prepare /Volumes/<card>` makes small covers.
- **WiFi mode web page** (`http://<device-ip>`): drag in MP3s and a cover, organize,
  rename, delete, run Clean up, and manage podcast feeds.
- **`upload.py`** (USB, or WiFi when the device is in WiFi mode):

```sh
PY=~/.venvs/esp/bin/python
$PY upload.py "~/Music/Some Artist - Some Album"   # upload an album folder
$PY upload.py --ls                                 # list the card
$PY upload.py --wifi MyNetwork                     # save WiFi credentials (prompts for the password)
$PY upload.py --mode wifi                          # reboot into WiFi (or bt) mode
$PY upload.py --name Ava                           # name the owner: "Hi, Ava!", ava-tunes.local
```

Every `upload.py` run also sets the device clock from the computer.

## Files

- `tiny-tunes.ino`: the whole firmware (both modes, `#if CORE2_WIFI`), including the
  web page and the USB serial protocol, which is documented above `handleSerial()`.
- `upload.py`: host tool for uploads, WiFi setup, mode switching and card prep.
- `flash.sh`: builds both modes and flashes them.
- `CHANGELOG.md`: what changed in each version.
- `AGENTS.md`: working notes and constraints for AI coding agents.
