# Notes for agents working on Tiny Tunes

Tiny Tunes is a kids' audio player (music, podcasts, audiobooks) for the M5Stack Core2,
used by a **5-year-old and a 7-year-old**. Keep UI text and controls right for a
pre-reader through a 2nd grader: pictures over words, big targets. Of the touch
buttons below the screen only the middle one (Home) does anything alone; two held for
3 s = grown-ups. See README.md for setup, build
and the SD card layout.

## Working agreements

- **Build and flash only when asked.** Making code changes is fine; don't run
  `flash.sh` (or compile) unless the user says to.
- **Keep CHANGELOG.md up to date.** Every user-visible change gets a line under
  `## Unreleased` in the same change that makes it (Added / Changed / Fixed). When
  cutting a release, rename `Unreleased` to the new version and date, and add a new
  empty `Unreleased` section.
- **Versioning:** `VERSION` holds the human version (semver: minor for features,
  patch for fixes). `flash.sh` stamps `VERSION+<commit count>.<hash>` into the
  firmware (`-dirty` with uncommitted changes), so commit before building a release.
- Commit messages end with the co-author trailer used in the history.

## Constraints that bite

- **Two firmwares, one sketch.** `CORE2_WIFI=0` is Bluetooth mode, `=1` is WiFi
  mode; both live in the two app slots. Compile both to check a change.
- **The Bluetooth build is at its IRAM limit** (~130 KB of ~130 KB). Things that
  pulled code into IRAM and had to be avoided: libc time formatting (`strftime`,
  `localtime_r`, `gmtime_r`; replaced with local versions), `tolower`/`toupper`/
  `strncasecmp` (local helpers), `rand()` (xorshift), 1-byte `std::atomic`,
  `M5.Power.powerOff()` (use `M5.Power.Axp192.powerOff()`), M5Unified's `Mic` class
  (direct I2S PDM instead). After adding code, check the BT build still links.
- **Arduino auto-prototypes:** structs and enums used in function signatures must
  be declared near the top of `tiny-tunes.ino`, or the build fails.
- **Serial runs at 460800**: the Core2's USB-serial chip garbles 921600. Opening the
  port resets the device.
- **Settings namespace** is `core2mp3` (the project's first name). Renaming it would
  reset everyone's settings.
- The screen is in normal landscape (`ROTATION = 1`), touch buttons below it. They're
  read in raw panel coordinates, so turning the screen upside down (`ROTATION = 3`)
  swaps the left and right ones.

## Tests

`test/run.sh` builds and runs `test/test_text.cpp` on the computer (any C++17
compiler; `test/arduino_string.h` stands in for Arduino's `String`). It covers
`tt_text.h`: title clean-up, ASCII folding and the ID3 reader. Run it after touching
those, and put new pure helpers there (not in the `.ino`) when they're worth testing.

## Screenshots

`screenshot.py` reads the screen back over USB (`SHOT`) and can open screens and load
albums for it (`SHOW`, `LOAD`). After a visible UI change, refresh the README's set with
`~/.venvs/esp/bin/python screenshot.py --tour docs/screenshots --music "<album>"` (a
device on the new firmware must be plugged in).
