// ----- screen switching / redraw -----
static void showScreen(Screen s) {
  screen = s;
  dirtyAll = true;
}

static void redraw() {
  if (screenOff) return;  // repaints when it's turned back on
  bool drew = false;
  if (dirtyAll) {
    dirtyAll = false;
    dirtyNow = false;
    drew = true;
    if (screen == Screen::Player) drawPlayer();
    else if (screen == Screen::Albums) drawAlbums();
    else if (screen == Screen::Shelves) drawShelves();
    else if (screen == Screen::Tracks) drawTracks();
    else { drawGrownups(); connectDirty = false; }
  }
  if (dirtyNow) {  // new song, pause/play
    dirtyNow = false;
    drew = true;
    if (screen == Screen::Player) {
      if (tracks.empty()) drawPlayer();
      else { drawPlayerInfo(); drawControls(); drawPlayProgress(); }
    } else if (screen == Screen::Tracks) {  // the playing row moved
      drawTrackRows();
      drawNowPlayingButton();
    } else if (screen == Screen::Shelves && homeTile() != homeTileShown) {
      drawShelves();  // the Keep listening card came or went: the shelves move
    } else if (screen == Screen::Shelves || screen == Screen::Albums) {
      if (screen == Screen::Shelves) drawKeepListening();  // its play button, its title
      drawNowPlayingButton();  // green / grey
    }
  }
  if (screen == Screen::Grownups && connectDirty) {  // Bluetooth news: only that tab shows it
    connectDirty = false;
    if (guTab == 1) drawGrownupsBody();
  }
  if (drew && snackUp()) drawSnack();  // keep it on top
}

// ---------- input ----------
static bool flickedX(const m5::touch_detail_t& t, int& dir) {
  if (!t.wasFlicked() || abs(t.distanceX()) < 40 || abs(t.distanceX()) < abs(t.distanceY())) return false;
  dir = t.distanceX() < 0 ? 1 : -1;  // swipe left = forward
  return true;
}
static bool flickedY(const m5::touch_detail_t& t, int& dir) {
  if (!t.wasFlicked() || abs(t.distanceY()) < 30 || abs(t.distanceY()) < abs(t.distanceX())) return false;
  dir = t.distanceY() < 0 ? 1 : -1;  // swipe up = scroll down the list
  return true;
}

// The middle touch button is Home: the shelves. (Back is in the top bar; the
// now-playing button there goes to the player.)
static void goHome() {
  buzz();
  showScreen(Screen::Shelves);
}

static void handlePlayer(const m5::touch_detail_t& t) {
  if (tracks.empty()) return;
  static int seekX = 0;
  if (t.isPressed() && t.base_y >= PB_Y - 10) {  // finger on the progress bar: the knob follows it
    seekDragging = true;
    seekX = t.x;
    drawPlayProgressAt(t.x - PB_X);
    return;
  }
  if (seekDragging) {  // let go: jump there
    seekDragging = false;
    buzz();
    seekTo((float)(seekX - PB_X) / PB_W);
    return;
  }
  int dir;
  if (flickedX(t, dir)) { buzz(); playTrack(current + dir); return; }  // swipe left = next song, right = previous
  if (!t.wasClicked()) return;
  if (t.y < P_ROW_TOP || t.y >= P_ROW_BOTTOM) return;
  static const int CX[5] = {B_QUIET.x, B_PREV.x, B_PLAY.x, B_NEXT.x, B_LOUD.x};
  int i = 0;  // the nearest button
  for (int k = 1; k < 5; k++) if (abs(t.x - CX[k]) < abs(t.x - CX[i])) i = k;
  buzz();
  drawControl(i, true);  // lit for a moment (a new song or play / pause redraws the row anyway)
  litControl = i;
  litUntil = millis() + 160;
  if (i == 0) changeVolume(-1);
  else if (i == 1) playTrack(current - 1);
  else if (i == 2) togglePause();
  else if (i == 3) playTrack(current + 1);
  else changeVolume(1);
}

// Where a book picks up: the first chapter not finished yet (from the start once all are).
static int bookResumeChapter(int a) {
  auto list = listTracks(a);
  auto v = loadResume(albums[a].path);
  for (size_t i = 0; i < list.size(); i++) {
    ResumeEntry* e = findResume(v, fileName(list[i]));
    if (!e || !e->done) return i;
  }
  return 0;
}

static void handleAlbums(const m5::touch_detail_t& t) {
  int n = shelfAlbums.size();
  if (!n) return;
  auto turn = [&](int by) {  // round and round with three or more; two just switch
    int idx = carouselIdx + by;
    idx = n >= 3 ? (idx % n + n) % n : constrain(idx, 0, n - 1);
    if (idx == carouselIdx) return;
    buzz();
    carouselIdx = idx;
    drawCarousel();
    if (snackUp()) drawSnack();
  };
  int dir;
  if (flickedX(t, dir)) { turn(dir); return; }  // swipe left = the next album
  if (!t.wasClicked() || t.y < NAV_H + 4) return;
  int left, right;
  carouselSides(left, right);
  if (t.x >= CV_X - 6 && t.x < CV_X + CV_BIG + 6) {  // the middle (cover or name): open it
    buzz();
    int a = shelfAlbums[carouselIdx];
    if (isBook(a)) {  // books: straight to the player, where it left off
      if (a != playAlbum || current < 0) playAlbumTrack(a, bookResumeChapter(a));
      else if (paused) togglePause();
      showScreen(Screen::Player);
      return;
    }
    shrinkFromCarousel = true;
    openTracks(a);
  } else if (t.y < CV_Y + CV_BIG + 12) {
    if (t.x < CV_X && left >= 0) turn(-1);
    else if (t.x >= CV_X + CV_BIG && right >= 0) turn(1);
  }
}

static void handleTracks(const m5::touch_detail_t& t) {
  auto scroll = [&](int dir) {  // a page at a time, keeping one row for context
    int n = viewTracks.size(), top;
    if (n <= rowsFor(0)) return;  // it all fits under the header
    if (dir > 0) top = std::min(tracksTop + rowsFor(tracksTop) - 1, std::max(1, n - rowsFor(1)));
    else top = tracksTop - (rowsFor(1) - 1) <= 0 ? 0 : tracksTop - (rowsFor(1) - 1);
    if (top == tracksTop) return;
    buzz();
    bool header = (top == 0) != (tracksTop == 0);  // the header comes or goes: redraw it all
    tracksTop = top;
    if (header) drawTracks();
    else drawTrackRows();
    if (snackUp()) drawSnack();
  };
  int dir;
  if (flickedY(t, dir)) { scroll(dir); return; }
  if (!t.wasClicked() || t.x < tlX() - 4 || t.y < listTop() - 4) return;  // the header does nothing
  int r = (t.y - listTop()) / trRowH(), i = tracksTop + r;
  if (r >= trRows() || i >= (int)viewTracks.size()) return;
  buzz();
  if (viewAlbum == playAlbum && i == current) showScreen(Screen::Player);  // the song that's on: its page
  else playAlbumTrack(viewAlbum, i);  // another one: play it and stay on the list
}

// Grown-ups: hold two of the three touch buttons together for 3 s. A yellow strip grows
// along the bottom edge (by the buttons) meanwhile. A kid won't do that by accident, and
// it isn't a tap, so it doesn't also go Home.
static constexpr uint32_t HOLD_MS = 3000;

static void drawHoldProgress(uint32_t heldMs) {
  M5.Display.fillRect(0, 235, 320 * std::min(heldMs, HOLD_MS) / HOLD_MS, 5, C_YELLOW);
}

// The power button: hold it (~1 s) to switch off, saving the place in a book or podcast.
// (Held 4 s, the power chip cuts the power by itself.)
static void powerDown() {
  stopPlayback();  // books/podcasts: remembers the spot
  if (recentUnsaved) saveRecent();
  auto& d = M5.Display;
  d.wakeup();
  applyBrightness();
  d.fillScreen(C_BG);
  drawTitle(owner.length() ? "Bye, " + owner + "!" : String("Bye!"), 160, 120, 280, 80, C_YELLOW);
  delay(600);
  M5.Power.Axp192.powerOff();  // straight to the PMIC; M5.Power.powerOff() drags deep-sleep code into IRAM
}

static void handleInput() {
  // The power button: a tap turns the screen off (the music keeps playing) and on again.
  if (M5.BtnPWR.wasHold()) { powerDown(); return; }
  if (M5.BtnPWR.wasClicked()) {
    buzz();
    screenOff = !screenOff;
    if (screenOff) {
      dimmed = true;  // nothing redraws or pops up meanwhile
      M5.Display.setBrightness(0);
      M5.Display.sleep();
    } else {
      M5.Display.wakeup();
      dimmed = false;
      applyBrightness();
      lastTouchMs = millis();
      dirtyAll = true;
    }
    return;
  }
  if (screenOff) return;  // touches and the touch buttons do nothing while it's off

  auto t = M5.Touch.getDetail();
  if (t.base_y < 0 || t.base_y >= 240) t = m5::touch_detail_t();  // touch-button strip, read as BtnA..C
  int buttonsDown = M5.BtnA.isPressed() + M5.BtnB.isPressed() + M5.BtnC.isPressed();
  bool anyInput = t.isPressed() || buttonsDown;
  static bool wakeTouch = false;  // the touch that woke the screen; ignored until it lifts
  if (anyInput) {
    lastTouchMs = millis();
    if (dimmed) {
      dimmed = false;
      wakeTouch = true;
      applyBrightness();
    }
  }
  if (wakeTouch) {  // otherwise its release would also "click" whatever was under it
    if (!anyInput) wakeTouch = false;
    return;
  }
  if (dimmed) return;

  // two buttons held: grown-ups (and no Back/Home/Playing when they let go)
  static uint32_t holdStart = 0, lastHoldDraw = 0;
  static bool chord = false;
  bool afterChord = chord;
  if (buttonsDown >= 2) chord = true;
  else if (!buttonsDown) chord = false;
  if (buttonsDown >= 2 && screen != Screen::Grownups) {
    if (!holdStart) holdStart = millis();
    uint32_t held = millis() - holdStart;
    if (millis() - lastHoldDraw > 100) { lastHoldDraw = millis(); drawHoldProgress(held); }
    if (held >= HOLD_MS) {
      buzz();
      holdStart = 0;
      snackShown = Snack::None;
      guTab = 0;
      statsPage = 0;
      beforeGrownups = screen == Screen::Tracks ? Screen::Albums : screen;  // the song list re-reads its album on open
      showScreen(Screen::Grownups);
    }
    return;
  }
  if (holdStart) { holdStart = 0; dirtyAll = true; }  // let go early: wipe the strip

  // the middle button: Home (the left and right ones only count held together, above)
  if (!afterChord && !chord && M5.BtnB.wasClicked()) {
    if (screen == Screen::Shelves) return;  // already Home: nothing to do
    if (snackUp()) snackUntil = millis();
    dirtyAll = true;  // repaint even if it stays on the same screen (a snackbar was up)
    goHome();
    return;
  }

  if (snackUp() && t.wasClicked()) { snackUntil = millis(); return; }  // a tap only dismisses the snackbar
  if (screen != Screen::Grownups && handleNavBar(t)) return;  // the top bar

  switch (screen) {
    case Screen::Player: handlePlayer(t); break;
    case Screen::Albums: handleAlbums(t); break;
    case Screen::Shelves: handleShelves(t); break;
    case Screen::Tracks: handleTracks(t); break;
    case Screen::Grownups: handleGrownups(t); break;
  }
}

// Dim after 2 minutes untouched; power off when paused on battery (grown-ups setting).
static void handleIdle() {
  uint32_t idle = millis() - lastTouchMs;
  if (!dimmed && !screenOff && idle > 120000UL) {
    dimmed = true;
    M5.Display.setBrightness(20);
  }
  bool playing = !paused && current >= 0;
  uint32_t offMin = AUTO_OFF_MIN[autoOffIdx];
  if (offMin && !playing && idle > offMin * 60UL * 1000 && !usbPower())
    M5.Power.Axp192.powerOff();  // straight to the PMIC; M5.Power.powerOff() drags deep-sleep code into IRAM
}
