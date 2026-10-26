# Changelog

All notable changes to Tiny Tunes. The version on the device (grown-ups → Stats,
the web page footer, `upload.py --ls`) is `VERSION` + build number + commit,
e.g. `0.3.0+6.cb61af8`.

## Unreleased

### Fixed
- The speaker's amplifier is switched off while Bluetooth plays (it stayed on, drawing
  power just as the radio started sending: a likely cause of restarts on connecting to
  a speaker, on battery).

### Added
- Grown-ups → Stats (third page) shows the last crash: a crash, a freeze or a power
  dip, whether Bluetooth was on and something was playing, and how many times. The
  serial port reports the reset reason at boot (`BOOT reset ...`).

### Added
- Color themes, picked on the grown-ups screen (Settings → Colors, tap either side to
  step through): Forest (new, the default: deep green with berry, teal and honey
  shelves and a gold play button), Navy (the colors until now), Ocean, Sunset and
  Midnight.

## 0.11.0 — 2026-10-26

### Fixed
- Home's battery mark no longer flickers: the reading is smoothed, and it turns red
  under 20% and only goes back to white over 23%.
- Opening an album no longer writes to the card while music plays (it could make it
  skip); the carousel's "last opened" is saved once it's quiet, or at power-off.
- Keep listening's title keeps clear of the play button.

### Changed
- Home's shelves sit side by side under the Keep listening card: a square-ish card each
  with its picture over its name, and new podcast episodes as a yellow number in the
  corner. With nothing loaded, they sit in the middle of the screen.
- The Podcasts shelf is called "Pods", so the name fits its card.
- The now-playing button in the top bar isn't on Home any more (the Keep listening card
  does that job). Leaving Home for a shelf, the card's round green button moves up into
  the top bar's corner and becomes it.
- Keep listening's round button has the same dancing bars as the now-playing button
  while something plays (a play triangle when paused).
- The top bar is in the shelf's own color, as on Home (Music blue, Pods purple, Books
  orange), on its albums, songs and player.
- Home's top bar: whose it is ("Ellie") after the house, the time in the middle ("2:31 PM",
  once the clock has been set), and on the right headphones while they're connected (a WiFi
  mark in WiFi mode) and the battery: white, red under 20%, green with a bolt while
  charging.
- The top bar's words (the name, the time, the shelf) are in the smaller font.
- Home's cards are flat (no white edge).
- Albums are A to Z again (the carousel goes round, so order matters less), and a
  shelf opens on the album opened or played most recently there. Opening an album's
  songs now counts as recent, not only playing it.
- The player shows just the title (a book's name for books), next to the cover: no
  artist or album line.
- Keep listening's title scrolls like a marquee when it's too long to fit.
- The Home button does nothing on Home (it used to repaint the screen).

## 0.10.0 — 2026-10-26

### Added
- Home has a "Keep listening" card at the top when something's loaded: its cover and
  title, and a play button. One tap carries on playing and opens the player. The shelves
  get shorter to make room; with nothing loaded, Home is as before.
- Tests for the title clean-up and the ID3 tag reader that run on a computer
  (`test/run.sh`). Those functions moved from `tiny-tunes.ino` into `tt_text.h`.
- `tags.py`: fix song tags with a spreadsheet. `export` writes every song's title,
  artist, album, album artist, track and disc to a CSV; `apply` shows what would change
  and writes the edits back into the MP3s (keeping their ID3 version and cover art).

### Changed
- When Bluetooth headphones disconnect while something's playing, it pauses instead of
  carrying on out loud. The snackbar says "Headphones off — paused".

## 0.9.9 — 2026-10-26

### Changed
- The album carousel shows just the album's name, not the artist.

## 0.9.8 — 2026-10-26

### Fixed
- An album folder with two covers of the same kind (cover.png and cover.jpg) uses the
  more recently changed one, not whichever the card lists first.

### Added
- A `COVERS` serial command: each album's cover in use, and every picture in its folder
  with its size in pixels and its date (for finding covers that aren't square).

## 0.9.7 — 2026-10-26

### Changed
- The player's five buttons are evenly spaced (the same gap between each); taps still
  go to the nearest one, anywhere in the row.

## 0.9.6 — 2026-10-26

### Fixed
- The player's buttons were hard to tap, the volume ones especially (5 mm targets
  against the screen's edges). The row is now five equal 8 mm columns, the full height of
  the row, so a tap anywhere lands on a button; the buttons are a bit bigger and the
  volume ones moved in from the edges. A tapped button lights up for a moment, and a tap
  may move a little more before it counts as a swipe.

## 0.9.5 — 2026-10-26

### Fixed
- Opening an album could leave three thin vertical lines across its name and artist on
  the song list (left over from the cover's shrinking animation).

## 0.9.4 — 2026-10-26

### Changed
- The album carousel sits a little lower, with more room between the cover and the name.
- The song list's titles are bigger; three songs show under the cover (five scrolled).

## 0.9.3 — 2026-10-26

### Added
- `screenshot.py`: screenshots read back from the device's display over USB (one, or the
  README's set with `--tour`), using new `LOAD`, `SHOW` and `SHOT` serial commands.

## 0.9.2 — 2026-10-25

### Changed
- The song list's header (cover, name, artist) scrolls away with the list: scrolled
  down, six songs fit (four podcast episodes); back at the top, the header returns.

### Fixed
- The cover sat a couple of pixels differently on the song list and the player; it now
  stays put between them.

## 0.9.1 — 2026-10-25

### Changed
- Books open straight into the player, where they left off (no chapter list): one
  progress bar for the whole book with a tick between chapters (drag across chapters),
  "Ch 3 of 22" and the minutes left in the chapter. Back goes to the books.
- The player shows the cover again (when there is one), with the song, artist and
  album beside it (books: book, author, chapter; podcasts: episode and show).
- Previous / next buttons are back on the player, between the quieter / louder ones.

## 0.9.0 — 2026-10-25

### Changed
- Album art is square. The album picker is a carousel: one big cover in the middle
  with the album's name across the screen below it (and its artist), the albums either
  side peeking in. Swipe for the next / previous; it goes round and round (three or more
  albums). Tap a side cover to turn to it, the middle to open it. No page dots.
- The song list has a header with the album's cover (it shrinks there from the
  carousel), its name and artist, and full-width song rows below. Podcasts too.
- Song titles from tags lose a leading track number ("01 - The Cyclone" -> "The
  Cyclone").
- The library index (every album's songs and tags) and the carousel's covers are made
  at boot, so song lists open instantly and swiping never waits on the SD card. After
  changing the card in a computer, power-cycle (changes made through the device rescan
  by themselves).

### Fixed
- Albums without a cover show the shelf's picture on their square, not their name twice.

### Changed
- Songs are named by their ID3 tags everywhere: the song list, the web page and the
  player (titles, artist, album artist), with album names from the album tag. Songs
  play in disc / track number order when every song has one. UTF-16 tags are read
  too. File and folder names are still used where a tag is missing; podcasts keep their
  show and episode names.
- An album without a cover image uses the picture embedded in its songs.
- The album picker lists the albums played most recently first (then A to Z). The
  device remembers them in `/.recent` on the card.

## 0.8.3 — 2026-10-24

### Added
- A Stop button for the podcast sync on the web page (and `POST /podcast/stop`).
  Finished episodes stay; a half-downloaded one is dropped.

## 0.8.2 — 2026-10-24

### Changed
- Player volume: a quieter button left of play and a louder one right of it, one step
  per tap, with the level as five dashes under play. The volume panel is gone.

## 0.8.1 — 2026-10-24

### Changed
- The player keeps the now-playing button at the top right, like every other screen;
  the volume button moves to the right of the play button.
- Books and podcasts drop the back 15 s / ahead 30 s buttons (the progress bar seeks).

## 0.8.0 — 2026-10-24

### Changed
- Everything is flat: no shadows under buttons, panels, the snackbar or text.
- The top bar always shows the shelf's name (Music, Podcasts, Books) next to the back
  button; the album picker has a back button too (to Home). Only Home shows the house.
- The now-playing button is always three sound bars: bouncing on green while music
  plays, still and grey when paused.

### Fixed
- The song list no longer redraws the whole screen (card and cover included) when the
  next song starts, or when scrolling; just the rows.
- Book and podcast lists read each file's size once instead of on every redraw.

## 0.7.7 — 2026-10-24

### Changed
- Album cards are completely flat: no second card peeking out on the right.
- The player no longer shows "Song 3 of 12" (books and podcasts still show the minutes
  left).

## 0.7.6 — 2026-10-24

### Changed
- Cards are flat: no shadow under the album and shelf cards.

## 0.7.5 — 2026-10-24

### Changed
- The top bar is pictures only: no names or album art. Home shows the house, the album
  picker the shelf's picture, and the song list and player the back button.
- The player shows the artist under the title: from the song's tag, else from an
  "Artist - Album" folder or "Artist - Title" file names (podcasts: the show). Books and
  podcasts show the minutes left next to "Chapter 3 of 22".

## 0.7.4 — 2026-10-23

### Changed
- Podcast episode lists drop the card: episodes take the whole width, with up to three
  lines of title each.
- Tapping the song that's already playing in a list opens its page (instead of
  pausing it).
- The player's progress bar is bigger, and seeking is back: drag the knob, or tap a
  spot on the bar.

## 0.7.3 — 2026-10-23

### Changed
- The green now-playing button's bars dance while music plays.
- The song list's album card runs down to the bottom (it grows as it slides over),
  and song titles are a smaller size.

### Fixed
- The left album's card moved a few pixels when sliding over to the song list; it now
  stays put.

## 0.7.2 — 2026-10-23

### Changed
- The green now-playing bar at the bottom is gone. Its job moved to the button at the
  top right: green with an equalizer while something plays, grey when paused; tap it
  for the player. On the player that button is the volume.
- With the room back: album page dots sit at the very bottom (taller cards), the song
  list shows five rows, and Home's cards fill the screen.

## 0.7.1 — 2026-10-23

### Changed
- The top bar is simpler and bigger (36 px): a round back button on the left (on the
  song list and the player), a little picture and the name of where you are, and a
  bigger volume button. The picture breadcrumbs were too small to tap.

## 0.7.0 — 2026-10-23

### Changed
- A top bar on every screen, the same height and place: picture breadcrumbs (a house
  for Home, then the shelf, the album and the song, the current one lit green with its
  name; tap one to go there) and the volume button, so volume works from anywhere.
- The album card is the same size and height on the album picker and the song list,
  so it only slides sideways; album page dots are below the cards.
- The player loses its "< album" line and big volume button (the top bar has both).
- Going back is the top bar: the song list no longer goes back on a sideways swipe.

### Fixed
- The buzz for tapping an album lasted as long as the card's slide; every tap now
  buzzes the same short pulse.

## 0.6.5 — 2026-10-23

### Changed
- The album picker's page dots are at the top.
- The song list has more room around each title (four rows above the now-playing bar).
- The player no longer shows the album card: "< album" and "Song 3 of 12" sit at the
  top left (tap the album to go back to its songs), then the title, the play button,
  the volume button and the progress bar.
- Tapping a song in the list plays it and stays on the list (tap the playing one to
  pause or carry on); the now-playing bar goes to the player.

### Fixed
- Going back from the player to the song list (or any screen change while music
  played) could take seconds, with the buzz motor running the whole time: the audio
  code waited for room in the speaker's queue in the middle of drawing. It now only
  hands the speaker audio when there's room, so nothing waits.

## 0.6.4 — 2026-10-23

### Changed
- Home: no show/album counts, and smaller pictures with room around them.
- The song list: the album's tall card sits on the left (it slides over from the album
  picker when you tap it), with the songs beside it. The header with the back button,
  name and song count is gone: swipe sideways to go back.
- The player: the album's tall card is on the left, like on the album picker and
  the song list; tap it to go back to the songs. No previous / next buttons: swipe
  left for the next song and right for the previous one, leaving one big play button
  (books and podcasts keep back 15 s / ahead 30 s beside it).

### Fixed
- The music could stutter for a moment when changing screens: drawing a screen held
  up the MP3 decoder (the screen and the SD card share one bus). It now keeps decoding
  between the slow steps, and the audio buffer is twice as long.

## 0.6.3 — 2026-10-23

### Changed
- Only the middle touch button does something by itself: Home. The left and right
  ones only count held together (3 s, the grown-ups screen).
- A green now-playing bar is back along the bottom of Home, the albums and the song
  list: what's playing (or paused); tap it for the player.
- The player has a round back button (to the song list) and a progress bar along
  the bottom again (it only shows how far along; no seeking).
- Home is a list of wide cards, top to bottom: Podcasts, Music, Books.
- Album names on the cards are all one font size (long ones wrap onto more lines).

## 0.6.2 — 2026-10-23

### Changed
- Albums: no shelf name at the top; the cards move up and the page dots sit
  centered underneath them.
- The song list has a round back button at the top left.

## 0.6.1 — 2026-10-23

### Changed
- The screen is the right way up again: the three touch buttons (Back, Home,
  Playing) are now below it, and the icon strip for them is gone, giving the
  screens that room back (taller album and shelf cards, bigger song rows).
- Home always shows Music, Podcasts and Books, even when the card has nothing for
  one of them yet ("Nothing here yet!").
- Song titles in the list are all one size; long ones wrap onto a second line.

### Removed
- The Play button at the top of the song list (tap a song instead).

### Added
- The power button: a quick tap turns the screen off (the music keeps playing, and
  touches do nothing) and another tap turns it back on. Holding it for a second
  switches Tiny Tunes off, keeping the place in a book or podcast.

## 0.6.0 — 2026-10-23

### Changed
- The three touch buttons above the screen are Back, Home and Playing again, with
  an icon for each along the top of the screen, right under it (the icons can be
  tapped too). Holding two together for 3 s still opens the grown-ups screen.
- Albums are tall cards, three at a time; swipe sideways for more.
- The song list has full-width rows, with a small cover, the name and a big Play
  button on top (books and podcasts: Play picks up where they stopped).
- The player has no cover, card or progress bar: a line about the album, a big title,
  and bigger buttons. Swipe the title for the next / previous song.
- Volume: the volume button shows the level as 5 little bars; tapping it opens 5 big
  bars to pick from (was "How loud?" with three animals). "Loud is" is now "Loudest".

### Removed
- Seeking (the progress bar) and the sleep timer button. Books and podcasts still
  skip back 15 s / ahead 30 s.
- The on-screen back and now-playing buttons (the touch buttons do that now).

## 0.5.0 — 2026-10-23

### Changed
- A new look, Yoto-style: the album's tall card on the left of the player and the
  song list, with the cover on top and a color picked from it. Shelves are tall cards too.
- No top bar. Battery and headphone news come as a snackbar at the bottom instead:
  "Charge me!" at 20, 10 and 5% (with a buzz at 5%), "Charging", "All charged!",
  "Headphones on", "Playing out loud". A tap dismisses it.
- Volume: the speaker button on the player asks "How loud?" with three big choices
  (a mouse, a cat, a lion). "Loud" is the grown-ups' limit (renamed "Loud is").
- Everything is on the screen: a round back button on the player and song list, a
  green now-playing button on the albums and shelves screens. The touch buttons
  above the screen now only open the grown-ups screen: hold two of them together for
  3 seconds (holding the top of the screen no longer does).
- Books and podcasts: back 15 s / ahead 30 s instead of previous / next, how many
  minutes are left, and a sleep timer button (15 to 60 minutes, fades out).
- Track dots under the progress bar show where you are in an album (up to 16 songs).
- Long song titles wrap onto two lines in the song list.

### Added
- The owner's name, set on the web page's new Settings section or with
  `upload.py --name`: "Hi, Ava!" on the splash, "Ava's Tiny Tunes" on the web page,
  and `ava-tunes.local` on the network in WiFi mode (tells two devices apart).
- A dot on podcast covers with new episodes; a WiFi mark on the shelves in WiFi mode.

### Removed
- The Play place and its games, and the Sleep place (clock, rain/ocean sounds,
  night-light, wake-up time). The sleep timer lives on in the book/podcast player.

## 0.4.2 — 2026-10-23

### Added
- Seek within a song: drag the (now bigger) progress bar, or tap a spot on it.

### Fixed
- Podcast feeds with long show or episode descriptions (e.g. Yoto Daily) were
  read as having "no MP3 episodes".
- Saved headphones switched on later now reconnect by themselves (before, only
  in the first moments after power-on).

### Changed
- Battery: Bluetooth looks for new devices continuously only while the grown-ups
  Bluetooth tab is open; otherwise it checks in a short burst once a minute.
  Measured idle on battery: ~80–140 mA, down from ~120–180 mA in 0.4.0 (together
  with 0.4.1's idle CPU rest).

## 0.4.1 — 2026-10-23

### Fixed
- `upload.py` (uploads, moves, deletes) no longer starts music by itself afterwards.
- A possible crash: changing the library (web uploads, Clean up, podcast sync)
  while the grown-ups screen was open, then pressing Done.
- The nightly podcast sync waits until nobody has touched the device for 10
  minutes, so it can't reboot it mid-game at night.
- Podcast shelves are recognized by type, not by the folder being spelled
  `/podcasts` exactly (newest-first order, titles, episode layout).
- A damaged spot in an MP3 skips past the damage instead of ending the track
  (and no longer marks a book chapter as finished).
- Web uploads, renames and deletes no longer stop the music unless they touch the
  song that's playing; a podcast sync never deletes the episode that's playing.
- A song file that can't be opened is skipped instead of "playing" silence.
- Resume files forget episodes that have been deleted.

### Changed
- Battery: the main loop rests a few milliseconds per pass when nothing needs fast
  service (no music, sound or download), instead of running the CPU flat out.
- The web Library is sent album by album, so a large library can't run the WiFi
  firmware out of memory.
- Serial: `PWR` (power draw) and `REST 0|1` (idle rest off/on, for measuring).

## 0.4.0 — 2026-10-22

### Added
- Listen has shelves: Music, Podcasts, Books, plus any other top-level folder of
  albums on the card (e.g. `/stories`). Podcasts shows how many episodes are new.
- Books and podcasts resume where they stopped, per track, even after power-off.
  Their lists show a NEW dot, a progress bar for part-heard, or a check when done.
- Web page: the Library is grouped by shelf, and uploads choose a shelf (Music,
  Books, ...). `upload.py --shelf books` does the same.

### Changed
- Play: page through the games by swiping left/right (was up/down).
- The Listen button goes up one level: player → its track list → albums → shelves.
  At the shelves it stays put; from Play or Sleep it goes to the player.
- The albums screen has a header naming the shelf ("◀ Music"; tap it to go back),
  with the page dots, and pages sideways like the games. Grids page sideways,
  lists scroll up/down, and swipe-right-back is only on the song list.
- Hold the Listen button (about half a second) to jump straight to the player
  from anywhere.
- The player has just previous / play-pause / next; volume is in the top bar.
- Tapping the song card no longer opens the albums (swiping it still changes songs).
- The top-bar speaker no longer shows a level meter, and sits next to the battery
  (the WiFi/headphones icon, which comes and goes, moved to its left).
- A book or podcast stops at its last chapter/episode instead of starting over.

### Fixed
- The charging bolt no longer stays on after unplugging (USB is now detected from
  the battery current; the Core2's USB flag stays on with the cable out).
- Opening the web page no longer freezes the device (and its music) for ~10
  seconds: album covers on the page are small 160×160 images instead of the
  original files, and music keeps playing during slow web requests.

## 0.3.0 — 2026-10-22

### Added
- Volume on every screen: a speaker with a 3-bar meter in the top bar. Tapping it
  opens a big volume panel over whatever is showing; it closes after 3 seconds or
  a tap outside.

### Fixed
- Album names on cover-less tiles wrap whole words ("Fleetwood / Mac") instead of
  cutting them off. Any title with a word too wide now shrinks the font.

## 0.2.1 — 2026-10-22

### Changed
- The Listen (left) button opens the album picker from anywhere, on the page with
  the playing album. Pressing it again goes to the player. (It used to toggle
  play/pause on the player.)

### Added
- Startup splash: "Tiny Tunes" in rainbow letters with music notes and the
  version, shown for 5 seconds while the card loads. Skipped on the dark
  nightly-sync reboots.

## 0.2.0 — 2026-10-22

### Added
- Games: Simon, Memory Match, What Comes Next?, Tilt Maze, Whack-a-Mole.
- Nightly podcast sync: between midnight and 5 am, when plugged in and idle, the
  device switches to WiFi mode, syncs podcasts and switches back, keeping the
  screen dark (or the night-light glowing).
- Podcast episode lists: taller rows with a date badge and titles wrapped over up
  to three lines; the show name is dropped from episode titles.
- Grown-ups Stats: four pages (device, listening and fun games, learning games,
  technical).
- `IMU` serial command for checking the Tilt Maze axes.

### Changed
- The Play picker pages 6 games at a time and no longer shows levels.
- Podcasts sync nightly instead of every 6 hours.

## 0.1.0 — 2026-10-22

First versioned release.

### Added
- Listen / Play / Sleep on the three touch buttons.
- Listen: SD-card albums with cover art (JPG or PNG, decoded once and cached),
  played to Bluetooth headphones or the speaker; song titles cleaned for display.
- Play: Missing Letter, Right Spelling, Telling Time and Math, levelled in rounds
  of 5 questions.
- Sleep: big clock with a moon/sun "OK to wake", sleep timer with fade-out,
  rain/ocean sounds, night-light.
- Grown-ups screen (hold the top bar 3 s): max volume, brightness, auto-off,
  wake-up time, Bluetooth pairing, stats.
- WiFi mode web page: add music and covers, organize albums, Clean up, podcasts
  (RSS or Apple Podcasts links), playback remote.
- `upload.py` for uploads over USB or WiFi, WiFi setup, mode switching and card
  prep; `flash.sh` builds and flashes both modes with a version stamp.

### Fixed
- Music no longer plays at the wrong speed after a sleep sound.
- Power-on selects the last song without playing it.
- The charging bolt follows the real USB state.
