// ================= kid-friendly UI =================
// The middle touch button below the screen is Home; the other two only count held
// together (3 s, the grown-ups screen). Every screen has the same top bar: back (song
// list, player), where you are, and top right the now-playing button (green while
// playing; it opens the player).
// Shelves:  Podcasts, Music, Books (and any other shelf) as a list of wide cards.
// Albums:   a carousel of square covers (round and round), the name across the screen.
// Songs:    a header with the cover, name and artist; full-width song rows below.
// Player:   the cover with the song, artist and album (books: book, author, chapter),
//           quieter / previous / play / next / louder, and a seekable progress bar (books:
//           the whole book). Swipe for the next / previous track too.
// Snackbars pop up at the bottom for the battery and headphones.

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return M5.Display.color565(r, g, b); }
// (the colors are themes: see "colors" near the top)

static uint16_t albumColor(int i) {
  static const uint8_t pal[][3] = {
      {255, 89, 94}, {255, 146, 76}, {46, 196, 120}, {25, 150, 220},
      {138, 80, 236}, {255, 70, 160}, {0, 170, 170}, {230, 160, 0}};
  auto& c = pal[(i < 0 ? 0 : i) % 8];
  return rgb(c[0], c[1], c[2]);
}

// An album's color: picked from its cover once that's loaded, else from the palette.
static uint16_t albumTint(int a) {
  return a >= 0 && a < (int)albums.size() && albums[a].tint ? albums[a].tint : albumColor(a);
}

// The bar along the top of every screen: back, where you are, and the now-playing
// button. See "the top bar".
static constexpr int NAV_H = 36;
// Album covers are squares: big in the middle of the carousel, smaller beside it, small
// in the song list's header.
static constexpr int CV_BIG = 116, CV_SIDE = 76, CV_HEAD = 60;
// Player: the cover (when there is one) with what's playing beside it: song, artist,
// album / episode, show / book, author, chapter. Then quieter, previous, play, next and
// louder in one row, the volume level and (books, podcasts) where you are and how long is
// left, and a progress bar to drag: the song, or for books the whole book (a tick between
// chapters). Swipe for the next / previous track too.
static constexpr int P_COVER_X = 12, P_COVER_Y = NAV_H + 8, P_COVER = CV_HEAD, P_CTRL_Y = 158;  // = the song list's header
// Quieter, previous, play, next, louder, with the same 16 px gap between each. A tap
// anywhere in the row, its full height, goes to the nearest button (columns ~64 px wide).
static constexpr RoundButton B_QUIET{31, 21}, B_PREV{91, 23}, B_PLAY{160, 30}, B_NEXT{229, 23}, B_LOUD{289, 21};
static constexpr int P_ROW_TOP = P_CTRL_Y - 42, P_ROW_BOTTOM = P_CTRL_Y + 42;
static constexpr int VOL_DOTS_Y = 204, P_LABEL_Y = 205;               // the level under play, labels either side
static constexpr int PB_X = 22, PB_Y = 220, PB_W = 276, PB_H = 14;  // progress: drag or tap to jump
static constexpr int VB_X = 244, VB_Y = 5, VB_W = 68, VB_H = 26;  // the top bar's now-playing button
static constexpr int KL_X = 8, KL_Y = NAV_H + 6, KL_W = 304, KL_H = CV_HEAD + 8;  // Home's Keep listening card...
static constexpr int KL_BX = KL_X + KL_W - 34, KL_BY = KL_Y + KL_H / 2, KL_BR = 21;  // ...and its round button

static uint32_t lastTouchMs = 0;
static bool dimmed = false;
static bool screenOff = false;  // the power button's tap: screen off, sound on (see handleInput)
static int viewAlbum = -1, tracksTop = 0;
static std::vector<String> viewTracks;

// A short pulse; loop() switches it off (and audioPump(), inside slow drawing like the
// card slide, so every tap buzzes the same). The motor needs ~100 ms to be felt.
// pulses: 1 for a tap, 2 (on-off-on) for a warning. Each edge flips the motor.
static uint32_t buzzNextMs = 0;
static int buzzEdges = 0;  // flips still to do
static bool buzzOn = false;
static void buzz(int pulses = 1) {
  buzzOn = true;
  M5.Power.setVibration(200);
  buzzEdges = pulses * 2 - 1;
  buzzNextMs = millis() + 150;
}
static void buzzService() {
  if (!buzzEdges || (int32_t)(millis() - buzzNextMs) < 0) return;
  buzzOn = !buzzOn;
  M5.Power.setVibration(buzzOn ? 200 : 0);
  buzzEdges--;
  buzzNextMs = millis() + (buzzOn ? 150 : 100);
}

static bool hitAt(int cx, int cy, int r, int x, int y) {
  int dx = x - cx, dy = y - cy;
  r += 8;  // fingers are bigger than the buttons look
  return dx * dx + dy * dy <= r * r;
}

static bool musicIsPlaying() { return !paused && mp3 && mp3->isRunning(); }

// Word-wraps text into at most maxLines lines that fit width w with the current font.
static std::vector<String> wrapText(const String& text, int w, int maxLines) {
  auto& d = M5.Display;
  std::vector<String> lines;
  String line;
  int pos = 0;
  while (pos <= (int)text.length()) {
    int sp = text.indexOf(' ', pos);
    if (sp < 0) sp = text.length();
    String word = text.substring(pos, sp);
    String trial = line.length() ? line + " " + word : word;
    if (d.textWidth(trial) <= w || !line.length()) line = trial;
    else { lines.push_back(line); line = word; }
    pos = sp + 1;
  }
  if (line.length()) lines.push_back(line);
  if ((int)lines.size() > maxLines) {
    lines.resize(maxLines);
    String& last = lines.back();
    while (last.length() > 1 && d.textWidth(last + "...") > w) last.remove(last.length() - 1);
    last += "...";
  }
  for (auto& l : lines)  // a single long word can still be too wide
    while (l.length() > 1 && d.textWidth(l) > w) l.remove(l.length() - 1);
  return lines;
}

// Draws centered, wrapped text, picking the biggest font where
// every word fits whole (too many lines or a word too wide both mean "smaller").
static void drawTitle(const String& text, int cx, int cy, int w, int h, uint16_t fg, bool small = false) {
  auto& d = M5.Display;
  const lgfx::IFont* big[] = {&fonts::FreeSansBold18pt7b, &fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b, &fonts::DejaVu12};
  const lgfx::IFont* little[] = {&fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b, &fonts::DejaVu12};
  auto* list = small ? little : big;
  int count = small ? 3 : 4;
  auto letters = [](const String& t) { int n = 0; for (char c : t) n += c != ' '; return n; };
  int want = letters(text);
  std::vector<String> lines;
  int lh = 0;
  for (int f = 0; f < count; f++) {
    d.setFont(list[f]);
    lh = d.fontHeight();
    lines = wrapText(text, w, std::max(1, h / lh));
    int got = 0;
    bool fits = true;
    for (auto& l : lines) { fits &= !l.endsWith("..."); got += letters(l); }
    if (fits && got == want) break;  // nothing cut off
  }
  d.setTextDatum(middle_center);
  int y0 = cy - (int)(lines.size() - 1) * lh / 2;
  d.setTextColor(fg);
  for (size_t i = 0; i < lines.size(); i++) d.drawString(lines[i], cx, y0 + i * lh);
}

// One line of text, shortened with "..." to fit.
static String fitLine(const String& text, int w) {
  auto lines = wrapText(text, w, 1);
  return lines.empty() ? String() : lines[0];
}

// ----- icons (all centered on x, y) -----
static void iconPlay(int x, int y, int s, uint16_t c) {
  // s wide, its box centered a twelfth of s right of x: dead center looks pushed left,
  // centering its weight looks pushed right
  int h = s * 3 / 5, left = x - s * 5 / 12, tip = x + s * 7 / 12;
  M5.Display.fillTriangle(left, y - h, left, y + h, tip, y, c);
}
static void iconPause(int x, int y, int s, uint16_t c) {
  M5.Display.fillRoundRect(x - s / 2, y - s / 2, s / 3, s, 3, c);
  M5.Display.fillRoundRect(x + s / 2 - s / 3, y - s / 2, s / 3, s, 3, c);
}
static void iconSkip(int x, int y, int s, bool fwd, uint16_t c) {
  int d = fwd ? 1 : -1, h = s / 2;
  M5.Display.fillTriangle(x - d * s / 2, y - h, x - d * s / 2, y + h, x, y, c);
  M5.Display.fillTriangle(x, y - h, x, y + h, x + d * s / 2, y, c);
}
static void iconSpeaker(int x, int y, uint16_t c) {  // spans x-8 .. x+7
  M5.Display.fillRect(x - 8, y - 5, 7, 10, c);
  M5.Display.fillTriangle(x - 3, y, x + 7, y - 10, x + 7, y + 10, c);
  M5.Display.fillRect(x - 3, y - 5, 5, 10, c);
}
static void iconBattery(int x, int y, int pct, bool usb) {  // top-left at x, y; 29 x 14
  auto& d = M5.Display;
  uint16_t fill = pct > 50 ? rgb(80, 220, 100) : pct > 20 ? C_YELLOW : rgb(255, 70, 70);
  d.drawRoundRect(x, y, 26, 14, 3, C_PURE);
  d.fillRect(x + 26, y + 4, 3, 6, C_PURE);
  d.fillRect(x + 2, y + 2, 22, 10, C_BAR);
  d.fillRect(x + 2, y + 2, std::max(2, 22 * constrain(pct, 0, 100) / 100), 10, fill);
  if (usb) {  // lightning bolt
    d.fillTriangle(x + 15, y + 1, x + 9, y + 8, x + 14, y + 8, C_PURE);
    d.fillTriangle(x + 12, y + 13, x + 18, y + 6, x + 13, y + 6, C_PURE);
  }
}
static void iconHeadphones(int x, int y, uint16_t c) {  // top at y
  M5.Display.fillArc(x, y + 9, 9, 7, 180, 360, c);
  M5.Display.fillRoundRect(x - 10, y + 7, 5, 8, 2, c);
  M5.Display.fillRoundRect(x + 6, y + 7, 5, 8, 2, c);
}
static void iconArrow(int x, int y, int dir, uint16_t c) {  // dir: 0 left, 1 right, 2 up, 3 down
  int s = 8;
  if (dir == 0) M5.Display.fillTriangle(x - s, y, x + s / 2, y - s, x + s / 2, y + s, c);
  else if (dir == 1) M5.Display.fillTriangle(x + s, y, x - s / 2, y - s, x - s / 2, y + s, c);
  else if (dir == 2) M5.Display.fillTriangle(x, y - s, x - s, y + s / 2, x + s, y + s / 2, c);
  else M5.Display.fillTriangle(x, y + s, x - s, y - s / 2, x + s, y - s / 2, c);
}
static void iconPlaying(int x, int y, uint16_t c) {  // three little equalizer bars
  M5.Display.fillRect(x - 6, y - 1, 3, 6, c);
  M5.Display.fillRect(x - 1, y - 5, 3, 10, c);
  M5.Display.fillRect(x + 4, y - 3, 3, 8, c);
}

// A flat round button.
static void drawRound(int x, int y, int r, uint16_t color) {
  M5.Display.fillCircle(x, y, r, color);
}

// USB plugged in? On the Core2 the AXP192's VBUS flag and voltage stay up on battery
// (the 5 V boost feeds that line), so go by the battery current instead: on USB it
// charges or sits near 0; on its own the Core2 draws 100+ mA from it. Smoothed so a
// moment of heavy load doesn't flicker it.
static bool usbPower() {
  static uint32_t lastRead = 0;
  static int drainCount = 0;
  static bool usb = true;
  if (millis() - lastRead >= 500 || !lastRead) {
    lastRead = millis();
    int ma = M5.Power.getBatteryCurrent();  // + charging, - draining
    drainCount = ma < -40 ? std::min(drainCount + 1, 4) : std::max(drainCount - 1, 0);
    usb = drainCount < 2;
  }
  return usb;
}

static bool rtcNow(int& h, int& m) {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.isEnabled() || !M5.Rtc.getDateTime(&dt) || dt.date.year < 2024) return false;
  h = dt.time.hours;
  m = dt.time.minutes;
  return true;
}

// ----- shelves look -----
static uint16_t shelfColor(const Shelf& sh) {
  switch (sh.kind) {
    case ShelfKind::Music: return C_BLUE;
    case ShelfKind::Podcasts: return C_BERRY;
    case ShelfKind::Books: return C_ORANGE;
    default: return TH(other);
  }
}

static ShelfKind albumKind(int a) {
  return a >= 0 && a < (int)albums.size() && albums[a].shelf >= 0 && albums[a].shelf < (int)shelves.size()
             ? shelves[albums[a].shelf].kind : ShelfKind::Music;
}

// The shelf's picture, about 56 x 52 at pct 100.
static void drawShelfIcon(ShelfKind kind, int cx, int cy, uint16_t c, uint16_t bg, int pct = 100) {
  auto& d = M5.Display;
  auto S = [pct](int v) { return v * pct / 100; };
  if (kind == ShelfKind::Music) {  // two beamed notes
    d.fillEllipse(cx - S(14), cy + S(14), S(9), S(7), c);
    d.fillEllipse(cx + S(16), cy + S(8), S(9), S(7), c);
    d.fillRect(cx - S(7), cy - S(18), std::max(2, S(4)), S(32), c);
    d.fillRect(cx + S(23), cy - S(24), std::max(2, S(4)), S(32), c);
    d.fillTriangle(cx - S(7), cy - S(18), cx + S(27), cy - S(24), cx + S(27), cy - S(16), c);
    d.fillTriangle(cx - S(7), cy - S(18), cx - S(7), cy - S(10), cx + S(27), cy - S(16), c);
  } else if (kind == ShelfKind::Podcasts) {  // microphone
    d.fillRoundRect(cx - S(10), cy - S(26), S(20), S(32), S(10), c);
    d.fillArc(cx, cy - S(4), S(19), S(15), 0, 180, c);
    d.fillRect(cx - S(2), cy + S(14), std::max(2, S(5)), S(10), c);
    d.fillRoundRect(cx - S(12), cy + S(23), S(25), std::max(2, S(4)), 2, c);
  } else if (kind == ShelfKind::Books) {  // an open book
    d.fillTriangle(cx - S(28), cy - S(16), cx - 1, cy - S(10), cx - S(28), cy + S(18), c);
    d.fillTriangle(cx - 1, cy - S(10), cx - 1, cy + S(22), cx - S(28), cy + S(18), c);
    d.fillTriangle(cx + S(28), cy - S(16), cx + 1, cy - S(10), cx + S(28), cy + S(18), c);
    d.fillTriangle(cx + 1, cy - S(10), cx + 1, cy + S(22), cx + S(28), cy + S(18), c);
    for (int k = 0; k < 3; k++) {  // lines of text
      d.drawLine(cx - S(22), cy - S(6) + S(k * 8), cx - S(7), cy - S(3) + S(k * 8), bg);
      d.drawLine(cx + S(7), cy - S(3) + S(k * 8), cx + S(22), cy - S(6) + S(k * 8), bg);
    }
  } else {  // a folder
    d.fillRoundRect(cx - S(26), cy - S(18), S(22), S(10), 3, c);
    d.fillRoundRect(cx - S(26), cy - S(12), S(52), S(34), 5, c);
  }
}

// ----- cards and album art -----
// A flat card (Home's shelves and Keep listening).
static void drawCardFrame(int x, int y, int w, int h, uint16_t fill) {
  M5.Display.fillRoundRect(x, y, w, h, 13, fill);  // flat: no edge
}

// Centered text in one font size (bold 9 pt), wrapped over up to
// maxLines lines ("..." past that).
static void drawWrapped(const String& text, int cx, int cy, int w, int maxLines, uint16_t fg) {
  auto& d = M5.Display;
  d.setFont(&fonts::FreeSansBold9pt7b);
  auto lines = wrapText(text, w, std::max(1, maxLines));
  int lh = 17, y0 = cy - (int)(lines.size() - 1) * lh / 2;
  d.setTextDatum(middle_center);
  d.setTextColor(fg);
  for (size_t i = 0; i < lines.size(); i++) d.drawString(lines[i], cx, y0 + i * lh);
}

// An album's cover as a square at x, y (dimmed for the carousel's sides), or its color
// with the shelf's picture when it has no cover.
static void drawAlbumArt(int a, int x, int y, int size, bool dim = false) {
  auto& d = M5.Display;
  if (M5Canvas* cv = getCover(a, size, dim)) { cv->pushSprite(&d, x, y, COVER_KEY); return; }
  uint16_t c = albumTint(a);
  if (dim) {  // halfway to the background
    uint8_t r = ((c >> 11) << 3), g = (((c >> 5) & 63) << 2), b = ((c & 31) << 3);
    uint32_t bg = THEMES[themeIdx].bg;
    c = rgb((r + (bg >> 16)) / 2, (g + ((bg >> 8) & 255)) / 2, (b + (bg & 255)) / 2);
  }
  d.fillRoundRect(x, y, size, size, size / 8, c);
  drawShelfIcon(albumKind(a), x + size / 2, y + size / 2, dim ? TH(dimInk) : C_PURE, c, size * 100 / 120);
}

// Something's loaded: the now-playing button has somewhere to go.
static bool haveNowPlaying() { return current >= 0 && !tracks.empty(); }

// ----- volume -----
// On the player: a quieter button left of play and a louder one right of it, one step
// a tap, with the level as 5 dashes under play. The top step is the grown-ups' limit.
static int volStep() { return (volLevel + 1) / 2; }  // 0..5 from volLevel's 0..10

// A speaker with `waves` sound waves (0 quieter, 2 louder), centered on x, y.
static void iconSpeakerWaves(int x, int y, int waves, uint16_t c) {
  auto& d = M5.Display;
  int sx = x - (waves ? 6 : 2);
  d.fillRect(sx - 9, y - 5, 7, 11, c);
  d.fillTriangle(sx - 4, y, sx + 6, y - 11, sx + 6, y + 11, c);
  d.fillRect(sx - 4, y - 5, 6, 11, c);
  for (int k = 1; k <= waves; k++) {  // the waves: arcs on the right, either side of 3 o'clock
    d.fillArc(sx + 6, y, 4 + k * 5, 2 + k * 5, 300, 360, c);
    d.fillArc(sx + 6, y, 4 + k * 5, 2 + k * 5, 0, 60, c);
  }
}

static void drawVolumeDots() {
  auto& d = M5.Display;
  d.fillRect(110, VOL_DOTS_Y - 4, 100, 10, C_BG);
  for (int i = 0; i < 5; i++) d.fillRoundRect(118 + i * 18, VOL_DOTS_Y - 2, 14, 5, 2, i < volStep() ? C_YELLOW : TH(offDash));
}

static void changeVolume(int by) {
  int step = constrain(volStep() + by, 1, 5);  // never all the way off: that just looks broken
  setVolLevel(step * 2);
  prefs.putUChar("lvl", volLevel);
  drawVolumeDots();
  if (!btConnected && !musicIsPlaying()) M5.Speaker.tone(660, 120, 1, true);  // so they hear how loud
}

// ----- player -----
static bool storyMode() { return isResumable(playAlbum); }

static bool isBook(int a) { return albumKind(a) == ShelfKind::Books; }

// Books: every chapter's size, for one bar across the whole book (looked up once per book).
static std::vector<uint32_t> bookSizes;
static String bookSizesFor;
static uint64_t bookTotal = 0;
static void loadBookSizes() {
  if (playAlbum < 0 || bookSizesFor == albums[playAlbum].path) return;
  bookSizesFor = albums[playAlbum].path;
  bookSizes.clear();
  bookTotal = 0;
  for (auto& t : tracks) { audioPump(); bookSizes.push_back(fileSize(t)); bookTotal += bookSizes.back(); }
}
static uint64_t bookBefore(int chapter) {  // bytes in the chapters before this one
  uint64_t n = 0;
  for (int k = 0; k < chapter && k < (int)bookSizes.size(); k++) n += bookSizes[k];
  return n;
}

// Books and podcasts, either side of the volume level: "Ch 3 of 22" (books) and
// "12 min left" in this chapter or episode (from the file size and its bitrate).
static void drawTimeLeft(bool force) {
  static String shown;
  String left, right;
  if (storyMode()) {
    if (isBook(playAlbum)) left = "Ch " + String(current + 1) + " of " + String(tracks.size());
    if (file && file->getSize() > file->getPos()) {
      uint32_t sec = (uint64_t)(file->getSize() - file->getPos()) * 8 / (trackKbps * 1000);
      right = sec < 60 ? String("Almost done") : String((sec + 30) / 60) + " min left";
    }
  }
  if (left + "|" + right == shown && !force) return;
  shown = left + "|" + right;
  auto& d = M5.Display;
  d.fillRect(0, P_LABEL_Y - 8, 108, 16, C_BG);
  d.fillRect(212, P_LABEL_Y - 8, 108, 16, C_BG);
  d.setFont(&fonts::DejaVu12);
  d.setTextColor(C_DIM);
  d.setTextDatum(middle_left);
  d.drawString(left, PB_X, P_LABEL_Y);
  d.setTextDatum(middle_right);
  d.drawString(right, PB_X + PB_W, P_LABEL_Y);
}

// One of the row's buttons (0 quieter, 1 previous, 2 play, 3 next, 4 louder); `lit`
// draws it lighter for a moment so a tap shows it landed.
static void drawControl(int i, bool lit) {
  static const RoundButton* B[5] = {&B_QUIET, &B_PREV, &B_PLAY, &B_NEXT, &B_LOUD};
  const RoundButton& b = *B[i];
  if (i == 2) {
    drawRound(b.x, P_CTRL_Y, b.r, lit ? TH(litPlay) : C_ACCENT);
    if (paused || tracks.empty()) iconPlay(b.x, P_CTRL_Y, b.r * 8 / 9, C_ACCENT_INK);
    else iconPause(b.x, P_CTRL_Y, b.r * 8 / 10, C_ACCENT_INK);
    return;
  }
  bool vol = i == 0 || i == 4;
  drawRound(b.x, P_CTRL_Y, b.r, lit ? (vol ? TH(litVol) : TH(litCtrl)) : (vol ? C_OFF : C_BLUE));
  if (vol) iconSpeakerWaves(b.x, P_CTRL_Y, i == 4 ? 2 : 0, C_WHITE);
  else iconSkip(b.x, P_CTRL_Y, 22, i == 3, C_SHELF_INK);  // on Music's color
}

static int litControl = -1;      // lit by a tap, until litUntil
static uint32_t litUntil = 0;

static void drawControls() {
  M5.Display.fillRect(0, P_CTRL_Y - B_PLAY.r - 3, 320, B_PLAY.r * 2 + 6, C_BG);
  for (int i = 0; i < 5; i++) drawControl(i, false);
  litControl = -1;
  drawVolumeDots();
}

// The top bar, then the cover with what's playing beside it (the full width without a
// cover): song / artist / album, episode / show, or book / author / chapter.
static void drawNavBar(int level);
static void drawPlayerInfo() {
  auto& d = M5.Display;
  d.fillRect(0, 0, 320, P_CTRL_Y - B_PLAY.r - 3, C_BG);
  drawNavBar(3);
  bool art = hasCover(albums[playAlbum]) && getCover(playAlbum, P_COVER);
  if (art) drawAlbumArt(playAlbum, P_COVER_X, P_COVER_Y, P_COVER);
  int tx = art ? P_COVER_X + P_COVER + 12 : P_COVER_X, tw = 312 - tx;
  String big = isBook(playAlbum) ? albums[playAlbum].name : title;  // just the title (a book's own name): calm
  d.setFont(&fonts::FreeSansBold12pt7b);
  auto lines = wrapText(big, tw, 2);
  int y = P_COVER_Y + P_COVER / 2 - ((int)lines.size() - 1) * 23 / 2;  // centered by the cover
  d.setTextDatum(middle_left);
  d.setTextColor(C_WHITE);
  for (auto& l : lines) { d.drawString(l, tx, y); y += 23; }
  drawTimeLeft(true);
}

// How far into the song (or chapter, or episode), with a knob to drag.
static bool seekDragging = false;  // a finger is on the progress bar

static bool bookBar() { return isBook(playAlbum) && tracks.size() > 1; }

static void drawPlayProgressAt(int p) {  // p: the knob's offset into the bar, 0..PB_W
  auto& d = M5.Display;
  p = constrain(p, 0, PB_W);
  d.fillRect(0, PB_Y - 9, 320, PB_H + 20, C_BG);
  d.fillRoundRect(PB_X, PB_Y, PB_W, PB_H, PB_H / 2, C_OFF);
  if (p > 0) d.fillRoundRect(PB_X, PB_Y, std::max(p, PB_H), PB_H, PB_H / 2, albumTint(playAlbum));
  if (bookBar() && bookTotal) {  // a tick between chapters
    uint64_t at = 0;
    for (size_t k = 0; k + 1 < bookSizes.size(); k++) {
      at += bookSizes[k];
      d.fillRect(PB_X + (int)(PB_W * at / bookTotal), PB_Y + 3, 2, PB_H - 6, TH(dimInk));
    }
  }
  d.fillCircle(PB_X + p, PB_Y + PB_H / 2, seekDragging ? 12 : 10, C_WHITE);
}

static void drawPlayProgress() {
  if (seekDragging) return;  // the finger's preview wins
  uint32_t start = trackAudioStart, size = file ? file->getSize() : 0, pos = file ? file->getPos() : 0;
  if (bookBar()) {  // books: how far into the whole book
    loadBookSizes();
    drawPlayProgressAt(bookTotal ? (int)(PB_W * (bookBefore(current) + pos) / bookTotal) : 0);
    return;
  }
  drawPlayProgressAt(size > start ? (int64_t)PB_W * (pos - std::min(pos, start)) / (size - start) : 0);
}

// Jumps to `frac` (0..1) of the current song's audio, and plays. Books: of the whole book.
static void seekTo(float frac) {
  if (current < 0 || tracks.empty()) return;
  if (bookBar() && bookTotal) {
    uint64_t at = (uint64_t)(constrain(frac, 0.0f, 0.995f) * bookTotal);
    int k = 0;
    while (k + 1 < (int)bookSizes.size() && at >= bookSizes[k]) { at -= bookSizes[k]; k++; }
    forceStartPos = std::max<uint32_t>(1, (uint32_t)at);  // the decoder finds the next frame (past any tag)
    recoverTries = 0;
    playTrack(k);
    return;
  }
  uint32_t size = file ? file->getSize() : fileSize(tracks[current]);
  uint32_t start = file ? trackAudioStart : 0;
  if (size <= start) return;
  forceStartPos = std::max<uint32_t>(start + 1, start + (uint32_t)(constrain(frac, 0.0f, 0.995f) * (size - start)));
  recoverTries = 0;
  playTrack(current);
}

static void drawPlayer() {
  auto& d = M5.Display;
  d.fillScreen(C_BG);
  if (tracks.empty()) {
    drawNavBar(0);
    d.fillRoundRect(16, 44, 288, 160, 18, C_OFF);
    drawTitle("No songs yet! Ask a grown-up to add some.", 160, 124, 260, 140, C_WHITE);
    return;
  }
  drawPlayerInfo();
  drawControls();
  drawPlayProgress();
}


// ----- snackbars -----
// A bar at the bottom for a few seconds: low battery (at 20, 10 and 5%), charging, full,
// headphones on/off. A tap dismisses it (and doesn't press what's under it). While the
// screen is dimmed it waits, and gives up after a minute.
static Snack snackShown = Snack::None, snackPending = Snack::None;
static uint32_t snackUntil = 0, snackPendingAt = 0;
static int snackPct = 0;
static bool snackPaused = false;  // Speaker: the headphones went and the music was paused
static constexpr int SN_X = 8, SN_Y = 186, SN_W = 304, SN_H = 46;

static bool snackUp() { return snackShown != Snack::None; }
static void showSnack(Snack s) { snackPending = s; snackPendingAt = millis(); }

static void drawSnack() {
  auto& d = M5.Display;
  uint16_t bg, edge;
  String text, detail;
  switch (snackShown) {
    case Snack::LowBattery: bg = rgb(58, 20, 32); edge = rgb(240, 80, 110); text = "Charge me!"; detail = String(snackPct) + "% left"; break;
    case Snack::Charging: bg = rgb(15, 52, 36); edge = C_GREEN; text = "Charging"; detail = String(snackPct) + "%"; break;
    case Snack::Full: bg = rgb(15, 52, 36); edge = C_GREEN; text = "All charged!"; break;
    case Snack::Headphones: bg = TH(snackBg); edge = C_BLUE; text = "Headphones on"; break;
    case Snack::Speaker: bg = TH(snackBg); edge = C_BLUE; text = "Headphones off"; if (snackPaused) detail = "paused"; break;
    default: return;
  }
  d.fillRoundRect(SN_X, SN_Y, SN_W, SN_H, 14, edge);
  d.fillRoundRect(SN_X + 3, SN_Y + 3, SN_W - 6, SN_H - 6, 11, bg);
  int cy = SN_Y + SN_H / 2;
  if (snackShown == Snack::Headphones) iconHeadphones(SN_X + 26, cy - 8, C_PURE);
  else if (snackShown == Snack::Speaker) iconSpeaker(SN_X + 26, cy, C_PURE);
  else iconBattery(SN_X + 12, cy - 7, snackShown == Snack::Full ? 100 : snackPct, snackShown != Snack::LowBattery);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(middle_left);
  d.setTextColor(C_PURE);
  d.drawString(text, SN_X + 52, cy + 1);
  if (detail.length()) {
    int tx = SN_X + 52 + d.textWidth(text) + 10;
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextColor(TH(snackText));
    d.drawString(detail, tx, cy + 2);
  }
}

static void serviceSnack() {
  uint32_t now = millis();
  if (snackUp() && (int32_t)(now - snackUntil) >= 0) { snackShown = Snack::None; dirtyAll = true; }
  if (snackPending == Snack::None || snackUp() || dimmed || screen == Screen::Grownups) return;
  if (now - snackPendingAt > 60000) { snackPending = Snack::None; return; }  // old news
  snackShown = snackPending;
  snackPending = Snack::None;
  snackUntil = now + 4000;
  drawSnack();
}

// Battery and charger news for the snackbar, checked every few seconds (and kept for
// Home's battery mark).
static int batPct = -1;       // -1: not read yet; smoothed (the gauge jumps a few % with the music)
static bool batUsb = false;   // plugged in
static bool batLow = false;   // under 20% (back over 23% to clear: no flicker at the line)
static void watchPower() {
  static uint32_t last = 0;
  static bool first = true, wasUsb = true, fullShown = false;
  static int warned = 101;  // lowest warning given since the last charge
  if (!first && millis() - last < 5000) return;
  last = millis();
  int pct = M5.Power.getBatteryLevel();
  bool usb = usbPower();
  batPct = batPct < 0 || usb != batUsb ? pct : (batPct * 3 + pct + 2) / 4;
  batUsb = usb;
  if (batPct < 20) batLow = true;
  else if (batPct >= 23) batLow = false;
  if (first) { first = false; wasUsb = usb; fullShown = usb && pct >= 100; }
  if (usb) {
    warned = 101;
    if (!wasUsb) { snackPct = pct; showSnack(Snack::Charging); fullShown = false; }
    else if (pct >= 100 && !fullShown) { fullShown = true; showSnack(Snack::Full); }
  } else {
    fullShown = false;
    int level = pct <= 5 ? 5 : pct <= 10 ? 10 : pct <= 20 ? 20 : 101;
    if (level < warned) {
      warned = level;
      snackPct = pct;
      showSnack(Snack::LowBattery);
      if (level == 5) buzz(2);
    }
  }
  wasUsb = usb;
}

// ----- albums -----
// A carousel: one big square cover in the middle with the album's name (just the name)
// across the screen below it, and the albums either side peeking in, dimmed. Swipe for the next / previous
// (it goes round and round with three or more), tap a side cover to turn to it, tap the
// middle to open it. The ones played most recently come first, then the rest A to Z.
// The playing album gets a ring and a badge, podcasts with new episodes a dot.
static constexpr int CV_X = 160 - CV_BIG / 2, CV_Y = NAV_H + 16, CV_GAP = 14;
static constexpr int CV_SIDE_Y = CV_Y + (CV_BIG - CV_SIDE) / 2;
static constexpr int CV_LEFT_X = CV_X - CV_GAP - CV_SIDE, CV_RIGHT_X = CV_X + CV_BIG + CV_GAP;
static constexpr int CV_NAME_Y = CV_Y + CV_BIG + 24;  // the name's first line (a two-line name starts a little higher)

static int viewShelf = 0;
static std::vector<int> shelfAlbums;  // indexes into albums for viewShelf
static int carouselIdx = 0;           // which of shelfAlbums is in the middle

static void showScreen(Screen s);
// Opens a shelf's carousel on `focus` (an album index) if it's there, else the first.
static void openShelf(int sh, int focus) {
  viewShelf = shelves.empty() ? 0 : constrain(sh, 0, (int)shelves.size() - 1);
  shelfAlbums.clear();
  for (size_t i = 0; i < albums.size(); i++)
    if (albums[i].shelf == viewShelf) shelfAlbums.push_back(i);
  // A to Z (it wraps round, so there's no "first"); without a focus, it opens on the
  // album opened or played most recently
  if (std::find(shelfAlbums.begin(), shelfAlbums.end(), focus) == shelfAlbums.end())
    focus = shelfAlbums.empty() ? -1 : *std::min_element(shelfAlbums.begin(), shelfAlbums.end(), [](int x, int y) {
      return recentRank(albums[x].path) < recentRank(albums[y].path);
    });
  auto it = std::find(shelfAlbums.begin(), shelfAlbums.end(), focus);
  carouselIdx = it == shelfAlbums.end() ? 0 : it - shelfAlbums.begin();
  showScreen(Screen::Albums);
}

// The albums left and right of the middle one (-1: none). Two albums don't wrap.
static void carouselSides(int& left, int& right) {
  int n = shelfAlbums.size();
  left = right = -1;
  if (n >= 3) { left = (carouselIdx + n - 1) % n; right = (carouselIdx + 1) % n; }
  else if (n == 2) { if (carouselIdx == 0) right = 1; else left = 0; }
}

static void drawCarousel() {
  auto& d = M5.Display;
  d.fillRect(0, NAV_H, 320, 240 - NAV_H, C_BG);
  int n = shelfAlbums.size();
  if (!n) return;
  carouselIdx = (carouselIdx % n + n) % n;
  int a = shelfAlbums[carouselIdx], left, right;
  carouselSides(left, right);
  if (left >= 0) drawAlbumArt(shelfAlbums[left], CV_LEFT_X, CV_SIDE_Y, CV_SIDE, true);
  if (right >= 0) drawAlbumArt(shelfAlbums[right], CV_RIGHT_X, CV_SIDE_Y, CV_SIDE, true);
  audioPump();
  if (a == playAlbum)
    for (int t = 3; t <= 4; t++) d.drawRoundRect(CV_X - t, CV_Y - t, CV_BIG + 2 * t, CV_BIG + 2 * t, CV_BIG / 8 + t, C_WHITE);
  drawAlbumArt(a, CV_X, CV_Y, CV_BIG);
  if (a == playAlbum) {
    d.fillCircle(CV_X + CV_BIG - 16, CV_Y + CV_BIG - 16, 12, C_ACCENT);
    iconPlaying(CV_X + CV_BIG - 16, CV_Y + CV_BIG - 16, C_ACCENT_INK);
  }
  if (albums[a].newCount > 0) d.fillCircle(CV_X + 14, CV_Y + 14, 7, C_YELLOW);  // new podcast episodes
  // the name across the screen (one size, two lines at most)
  d.setFont(&fonts::FreeSansBold12pt7b);
  auto lines = wrapText(albums[a].name, 296, 2);
  d.setTextDatum(middle_center);
  d.setTextColor(C_WHITE);
  int y = lines.size() > 1 ? CV_NAME_Y - 5 : CV_NAME_Y;
  for (auto& l : lines) { d.drawString(l, 160, y); y += 23; }
}

static uint16_t navBarColor = C_BAR;  // what the top bar was last painted with
static bool flyingFromHome = false;  // Home -> a shelf: Keep listening's cover is on its way to the now-playing button
static void flyToNowPlaying();      // see "shelves"
static void drawNowPlayingButton(bool bounce);
static void drawAlbums() {
  auto& d = M5.Display;
  d.fillScreen(C_BG);
  drawNavBar(1);
  if (flyingFromHome) {  // from Home: the Keep listening cover becomes the now-playing button
    if (haveNowPlaying()) { flyToNowPlaying(); drawNavBar(1); }  // the bar again: the cover crossed its name
    flyingFromHome = false;
    drawNowPlayingButton(false);
  }
  if (shelfAlbums.empty()) {  // Music, Podcasts and Books show even with nothing in them
    d.fillRoundRect(16, NAV_H + 20, 288, 150, 18, C_OFF);
    drawTitle("Nothing here yet! Ask a grown-up to add some.", 160, NAV_H + 95, 260, 130, C_WHITE);
    return;
  }
  drawCarousel();
}

// At boot: the carousel's covers for every album, big and dimmed, and the song list's.
static void prepareCarouselArt() {
  static const int sizes[3] = {CV_BIG, CV_SIDE, CV_HEAD};
  static const bool dims[3] = {false, true, false};
  for (size_t a = 0; a < albums.size(); a++) prepareAlbumArt(a, sizes, dims, 3);
}

// ----- the top bar -----
// A round back button on the left (up one level; Home shows its house there instead) and
// the shelf's name, and on the right the now-playing button.
// Same height and place on every screen.
static constexpr int NB_X = 24, NB_R = 15, NB_TAP_W = 62, NAV_CY = NAV_H / 2;

static void drawHouse(int cx, int cy, uint16_t c, uint16_t bg) {
  auto& d = M5.Display;
  d.fillTriangle(cx - 9, cy, cx, cy - 9, cx + 9, cy, c);
  d.fillRect(cx - 6, cy, 13, 8, c);
  d.fillRect(cx - 2, cy + 3, 4, 5, bg);  // the door
}

static bool navHasBack() { return screen != Screen::Shelves; }

// ----- Home's top bar -----
// After the house: whose it is ("Ellie"), the time in the middle (when the clock has
// been set), and on the right headphones while they're connected (WiFi mode: a WiFi
// mark) and the battery: white, red under 20%, green with a bolt while charging.
static void iconBatteryBig(int x, int y, int pct, bool charging, bool low, uint16_t bg) {  // top-left; 32 x 16
  auto& d = M5.Display;
  uint16_t c = !charging && low ? rgb(240, 80, 110) : C_WHITE;
  d.fillRoundRect(x, y, 29, 16, 4, c);
  d.fillRoundRect(x + 2, y + 2, 25, 12, 3, bg);
  d.fillRoundRect(x + 29, y + 5, 3, 6, 1, c);
  d.fillRoundRect(x + 4, y + 4, std::max(3, 21 * constrain(pct, 0, 100) / 100), 8, 2, charging ? C_GREEN : c);
  if (charging) {  // a bolt across it
    d.fillTriangle(x + 16, y + 1, x + 9, y + 9, x + 15, y + 9, bg);
    d.fillTriangle(x + 13, y + 15, x + 20, y + 7, x + 14, y + 7, bg);
    d.fillTriangle(x + 16, y + 3, x + 11, y + 8, x + 15, y + 8, C_WHITE);
    d.fillTriangle(x + 13, y + 13, x + 18, y + 8, x + 14, y + 8, C_WHITE);
  }
}
static void iconWifi(int cx, int cy, uint16_t c) {  // about 16 x 12, the dot at cx, cy + 5
  auto& d = M5.Display;
  d.fillCircle(cx, cy + 5, 1, c);
  d.fillArc(cx, cy + 5, 5, 3, 225, 315, c);
  d.fillArc(cx, cy + 5, 9, 7, 225, 315, c);
  d.fillArc(cx, cy + 5, 13, 11, 225, 315, c);
}

static bool statusLink() {  // headphones (Bluetooth mode) or WiFi (WiFi mode) to show
#if CORE2_WIFI
  return true;  // WiFi mode always shows its mark (dim while not connected)
#else
  return btConnected;
#endif
}
static uint32_t homeStatusKey() {  // changes when anything in the bar would
  int h = -1, m = 0;
  bool clock = rtcNow(h, m);
#if CORE2_WIFI
  bool link = webUp;
#else
  bool link = btConnected;
#endif
  int fill = batPct < 0 ? 0 : 1 + 21 * constrain(batPct, 0, 100) / 100;  // the icon's fill in pixels: redraw when that moves
  return ((clock ? h * 60 + m + 1 : 0) << 12) ^ (fill << 4) ^ (batLow << 3) ^ (batUsb << 1) ^ link;
}
static uint32_t homeStatusShown = 0;

static void drawHomeStatus(uint16_t bar) {
  auto& d = M5.Display;
  homeStatusShown = homeStatusKey();
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(C_WHITE);
  int h, m, clockW = 0;
  if (rtcNow(h, m)) {
    char t[12];
    snprintf(t, sizeof t, "%d:%02d %s", h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
    d.setTextDatum(middle_center);
    d.drawString(t, 160, NAV_CY + 1);
    clockW = d.textWidth(t);
  }
  if (owner.length()) {
    int nx = NB_X + 20, room = (clockW ? 160 - clockW / 2 - 14 : 220) - nx;
    d.setTextDatum(middle_left);
    d.drawString(fitLine(owner, room), nx, NAV_CY + 1);
  }
  int bx = 312 - 32;
  if (batPct >= 0) iconBatteryBig(bx, NAV_CY - 8, batPct, batUsb, batLow, bar);
  if (statusLink()) {
#if CORE2_WIFI
    iconWifi(bx - 16, NAV_CY - 1, webUp ? C_WHITE : C_DIM);
#else
    iconHeadphones(bx - 20, NAV_CY - 8, C_WHITE);
#endif
  }
}

// Home's bar again when the minute, the battery or the headphones change.
static void tickHomeStatus(uint32_t now) {
  static uint32_t last = 0;
  if (screen != Screen::Shelves || screenOff || now - last < 1000) return;
  last = now;
  if (homeStatusKey() != homeStatusShown) drawNavBar(0);
}

// Top right on every screen: three sound bars, bouncing on green while something plays,
// still and grey when it's paused (or nothing is loaded). Tap it for the player.
// Not on Home: its Keep listening card does that job (and flies up into this button
// when Home is left for a shelf).
static bool homeTile();  // see "shelves"
static uint16_t mix565(uint16_t a, uint16_t b, int f, int n) {  // f/n of the way from a to b
  int r = ((a >> 11) * (n - f) + (b >> 11) * f) / n, g = (((a >> 5) & 63) * (n - f) + ((b >> 5) & 63) * f) / n,
      bl = ((a & 31) * (n - f) + (b & 31) * f) / n;
  return (r << 11) | (g << 5) | bl;
}
static void drawNowPlayingButton(bool bounce = false) {  // bounce: new random bar heights
  if (flyingFromHome) return;
  bool home = screen == Screen::Shelves;  // on Home: the round button on the Keep listening card
  if (home && !homeTile()) return;
  static uint8_t heights[3] = {9, 15, 12};
  static uint32_t seed = 88675123;  // xorshift; libc rand() costs IRAM
  auto& d = M5.Display;
  bool on = musicIsPlaying();
  if (bounce && on)
    for (auto& h : heights) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; h = 5 + seed % 13; }
  int cx = VB_X + VB_W / 2, cy = VB_Y + VB_H / 2;
  if (home) {
    d.fillCircle(KL_BX, KL_BY, KL_BR, C_ACCENT);
    if (!on) { iconPlay(KL_BX, KL_BY, 18, C_ACCENT_INK); return; }
    cx = KL_BX; cy = KL_BY + 1;
  } else {
    d.fillRoundRect(VB_X, VB_Y, VB_W, VB_H, VB_H / 2, on ? C_ACCENT : C_OFF);
  }
  uint16_t ink = on ? C_ACCENT_INK : TH(dimInk);
  for (int i = 0; i < 3; i++) d.fillRoundRect(cx - 11 + i * 8, cy + 8 - heights[i], 5, heights[i], 2, ink);
}

// Back on the left (Home's house instead), the shelf's name, and top right the
// now-playing button.
static void drawNavBar(int level) {  // 0 Home, 1 a shelf's albums, 2 an album's songs, 3 the player
  auto& d = M5.Display;
  int shelf = level == 0 ? -1 : level == 1 ? viewShelf : level == 2 ? (viewAlbum >= 0 ? albums[viewAlbum].shelf : -1)
                                                                   : (playAlbum >= 0 ? albums[playAlbum].shelf : -1);
  bool known = shelf >= 0 && shelf < (int)shelves.size();
  uint16_t bar = known ? shelfColor(shelves[shelf]) : C_BAR;  // the shelf's own color, as on Home
  navBarColor = bar;
  d.fillRect(0, 0, 320, NAV_H, bar);
  if (level == 0) {
    drawHouse(NB_X, NAV_CY, C_WHITE, bar);
    drawHomeStatus(bar);
  } else {
    // on a shelf's color: a darker shade of it behind back (lighter, for dark ink)
    bool darkInk = THEMES[themeIdx].shelfInk != 0xFFFFFF;
    uint16_t ink = known ? C_SHELF_INK : C_WHITE;
    drawRound(NB_X, NAV_CY - 1, NB_R, !known ? C_OFF : mix565(bar, darkInk ? C_PURE : C_SHADE, 1, 3));
    iconArrow(NB_X - 1, NAV_CY - 1, 0, ink);
    if (known) {
      d.setFont(&fonts::FreeSansBold9pt7b);
      d.setTextDatum(middle_left);
      d.setTextColor(ink);
      d.drawString(fitLine(shelves[shelf].name, VB_X - NB_X - NB_R - 24), NB_X + NB_R + 12, NAV_CY + 1);
    }
  }
  drawNowPlayingButton();
}

// A tap in the top bar: back (player -> songs -> albums -> Home), or the right button.
static void openTracks(int a);
static bool handleNavBar(const m5::touch_detail_t& t) {
  if (!t.wasClicked() || t.y >= NAV_H + 6) return false;
  if (t.x >= VB_X - 10) {  // the now-playing button (on the player: already there)
    if (screen == Screen::Shelves) return true;  // Home has no button
    if (screen != Screen::Player && haveNowPlaying()) { buzz(); showScreen(Screen::Player); }
    return true;
  }
  if (t.x < NB_TAP_W && navHasBack()) {
    buzz();
    if (screen == Screen::Player && playAlbum >= 0) {
      if (isBook(playAlbum)) openShelf(albums[playAlbum].shelf, playAlbum);  // books have no chapter list
      else openTracks(playAlbum);
    }
    else if (screen == Screen::Tracks && viewAlbum >= 0) openShelf(albums[viewAlbum].shelf, viewAlbum);
    else if (screen == Screen::Albums) showScreen(Screen::Shelves);
  }
  return true;  // the rest of the bar does nothing
}

// ----- shelves: the library's top level (Home) -----
// A "Keep listening" card at the top when something's loaded (its cover, its title, a
// play button: one tap picks it up again), and below it the shelves side by side
// (Podcasts, Music, Books, then any other; three to a row), each a card with the
// shelf's picture over its name, and a yellow badge with the new podcast episodes.
// With nothing loaded, the shelves sit in the middle of the screen.
static constexpr int SH_TOP = NAV_H + 6, SH_BOTTOM = 234, SH_GAP = 6;
static bool homeTileShown = false;  // whether Home was last drawn with the card

static constexpr int SH_ROW_TOP = KL_Y + KL_H + SH_GAP;  // the shelves' top under the card

static bool homeTile() { return haveNowPlaying() && playAlbum >= 0; }

static void shelfTileRect(int i, int& x, int& y, int& w, int& h) {
  int n = std::max(1, std::min((int)shelves.size(), 6));
  int cols = std::min(n, 3), rows = (n + 2) / 3;
  int area = SH_BOTTOM - SH_ROW_TOP;  // the same size with or without the card
  int top = homeTile() ? SH_ROW_TOP : SH_TOP + (SH_BOTTOM - SH_TOP - area) / 2;
  w = (304 - (cols - 1) * SH_GAP) / cols;
  h = (area - (rows - 1) * SH_GAP) / rows;
  x = 8 + (i % 3) * (w + SH_GAP);
  y = top + (i / 3) * (h + SH_GAP);
}

// The title scrolls like a marquee when it's too long: a pause at the start, round it
// goes, a pause again.
static constexpr int KL_GAP = 48, KL_HOLD_MS = 1500;
static String klTitle;
static int klX = 0, klW = 0, klOff = 0;  // where the title goes, its room, how far it has scrolled
static uint32_t klHoldUntil = 0;

static void drawKeepTitle() {
  auto& d = M5.Display;
  int y = KL_Y + 45;
  d.setFont(&fonts::FreeSansBold12pt7b);
  int tw = d.textWidth(klTitle);
  if (tw <= klW) {  // fits: still
    d.setTextDatum(middle_left);
    d.setTextColor(C_WHITE);
    d.drawString(klTitle, klX, y);
    return;
  }
  static M5Canvas* strip = nullptr;
  if (!strip) {
    strip = new M5Canvas(&d);
    strip->setPsram(true);
    strip->setColorDepth(16);
  }
  if (strip->width() != klW && (!strip->createSprite(klW, 28))) return;
  strip->fillSprite(C_KL);
  strip->setFont(&fonts::FreeSansBold12pt7b);
  strip->setTextDatum(middle_left);
  strip->setTextColor(C_WHITE);
  strip->drawString(klTitle, -klOff, 14);
  strip->drawString(klTitle, tw + KL_GAP - klOff, 14);  // round again
  strip->pushSprite(&d, klX, y - 14);
}

// Called often from loop(): moves an overflowing title along.
static void tickKeepTitle(uint32_t now) {
  static uint32_t last = 0;
  if (screen != Screen::Shelves || !homeTile() || dimmed || screenOff || now - last < 40) return;
  last = now;
  if ((int32_t)(now - klHoldUntil) < 0 || !klTitle.length()) return;
  M5.Display.setFont(&fonts::FreeSansBold12pt7b);
  int tw = M5.Display.textWidth(klTitle);
  if (tw <= klW) return;
  klOff += 2;
  if (klOff >= tw + KL_GAP) { klOff = 0; klHoldUntil = now + KL_HOLD_MS; }
  drawKeepTitle();
}

static void drawKeepListening() {
  if (!homeTile()) return;
  auto& d = M5.Display;
  bool on = musicIsPlaying();
  drawCardFrame(KL_X, KL_Y, KL_W, KL_H, C_KL);
  drawAlbumArt(playAlbum, KL_X + 4, KL_Y + 4, CV_HEAD);
  int tx = KL_X + CV_HEAD + 16, tw = KL_BX - KL_BR - 10 - tx;
  d.setTextDatum(middle_left);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(on ? C_ACCENT : C_DIM);
  d.drawString(on ? "Now playing" : "Keep listening", tx, KL_Y + 21);
  String title = isBook(playAlbum) ? albums[playAlbum].name : trackTitle(tracks[current], playAlbum);
  tx -= 1; tw += 1;  // the bigger font's letters sit a pixel further in: line it up with the label
  if (title != klTitle || tx != klX || tw != klW) {  // a new title starts from the beginning
    klTitle = title; klX = tx; klW = tw; klOff = 0;
    klHoldUntil = millis() + KL_HOLD_MS;
  }
  drawKeepTitle();
  drawNowPlayingButton();  // the round button: dancing bars while it plays
}

static void drawShelves() {
  auto& d = M5.Display;
  d.fillScreen(C_BG);
  drawNavBar(0);
  homeTileShown = homeTile();
  drawKeepListening();
  for (int i = 0; i < (int)shelves.size() && i < 6; i++) {
    audioPump();
    const Shelf& sh = shelves[i];
    int x, y, w, h;
    shelfTileRect(i, x, y, w, h);
    uint16_t c = shelfColor(sh);
    drawCardFrame(x, y, w, h, c);
    int cx = x + w / 2, ny = y + h / 2 + 1;
    if (h >= 90) {  // the picture over the name (two rows of shelves: just names)
      drawShelfIcon(sh.kind, cx, y + h / 2 - 8, C_SHELF_INK, c, std::min(90, w * 80 / 100));
      ny = y + h - 20;
    }
    d.setFont(&fonts::FreeSansBold12pt7b);
    if (d.textWidth(sh.name) > w - 14) d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextDatum(middle_center);
    d.setTextColor(C_SHELF_INK);
    d.drawString(fitLine(sh.name, w - 14), cx, ny);
    if (sh.newCount > 0) {  // new podcast episodes: a badge in the corner
      String n = String(sh.newCount);
      d.setFont(&fonts::FreeSansBold9pt7b);
      int bw = std::max(24, (int)d.textWidth(n) + 14), bx = x + w - bw - 6, by = y + 7;
      d.fillRoundRect(bx, by, bw, 24, 12, C_CREAM);
      d.setTextColor(C_SHADE);
      d.drawString(n, bx + bw / 2, by + 13);
    }
  }
}

// Leaving Home for a shelf: Keep listening's round button moves up into the top bar's
// corner and becomes the now-playing button (in the same steps as the carousel's cover
// shrinking into the song list).
static void flyToNowPlaying() {
  auto& d = M5.Display;
  const int x0 = KL_BX - KL_BR, y0 = KL_BY - KL_BR, w0 = 2 * KL_BR, h0 = 2 * KL_BR;
  uint16_t c1 = musicIsPlaying() ? C_ACCENT : C_OFF;
  auto wipe = [&](int x, int y, int w, int h) {  // background below the bar, the bar's color in it
    int top = y - 2, bottom = y + h + 2, mid = std::min(std::max(top, NAV_H), bottom);
    if (bottom > mid) d.fillRect(x - 2, mid, w + 4, bottom - mid, C_BG);
    if (mid > top) d.fillRect(x - 2, top, w + 4, mid - top, navBarColor);
  };
  d.fillCircle(KL_BX, KL_BY, KL_BR, C_ACCENT);
  int px = x0, py = y0, pw = w0, ph = h0;
  for (int f = 1; f <= 4; f++) {
    int x = x0 + (VB_X - x0) * f / 4, y = y0 + (VB_Y - y0) * f / 4;
    int w = w0 + (VB_W - w0) * f / 4, h = h0 + (VB_H - h0) * f / 4;
    wipe(px, py, pw, ph);
    if (f == 4) break;  // the last step is the button itself
    d.fillRoundRect(x, y, w, h, h / 2, mix565(C_ACCENT, c1, f, 4));
    px = x; py = y; pw = w; ph = h;
    audioPump();
  }
}

static void handleShelves(const m5::touch_detail_t& t) {
  if (!t.wasClicked()) return;
  if (homeTile() && t.y >= KL_Y - 3 && t.y < KL_Y + KL_H + 3) {  // Keep listening: play it, and its page
    buzz();
    if (paused) togglePause();
    showScreen(Screen::Player);
    return;
  }
  for (int i = 0; i < (int)shelves.size() && i < 6; i++) {
    int x, y, w, h;
    shelfTileRect(i, x, y, w, h);
    if (t.x >= x - 3 && t.x < x + w + 3 && t.y >= y - 3 && t.y < y + h + 3) {
      buzz();
      flyingFromHome = homeTileShown;  // drawAlbums() flies the card's cover up to the button
      openShelf(i, -1);  // on the album opened or played last
      return;
    }
  }
}

// ----- songs -----
// A header with the album's cover (it shrinks there from the carousel), its name and its
// artist; the songs below in full-width rows, all in one font size (long titles wrap onto
// a second line). Tap a song to play it (the list stays; tap the playing one for its
// page). Swipe up/down to scroll; the top bar goes back. Podcast episodes have a date badge.
// The header sits at the top of the list like its first item: scrolled down, it's gone
// and the rows use the whole screen under the top bar.
static constexpr int TH_X = P_COVER_X, TH_Y = P_COVER_Y, TL_Y = TH_Y + CV_HEAD + 8, TL_FULL_Y = NAV_H + 6, TL_X = 8, TL_W = 298;
static int trRowH() { return isPodcastAlbum(viewAlbum) ? 42 : 38; }  // three under the header
static int listTopFor(int top) { return top == 0 ? TL_Y : TL_FULL_Y; }    // where the rows start
static int rowsFor(int top) { return (242 - listTopFor(top)) / trRowH(); }  // 4 under the header, 6 without (podcasts 3, 4)
static int listTop() { return listTopFor(tracksTop); }
static int trRows() { return rowsFor(tracksTop); }
static bool shrinkFromCarousel = false;  // set when the carousel's middle cover was tapped
static int tlX() { return TL_X; }
static int tlW() { return TL_W; }

static std::vector<ResumeEntry> viewResume;  // books/podcasts: progress shown in the list
static std::vector<int32_t> viewSizes;        // ...and each file's size, looked up once (-1: not yet)

static void openTracks(int a) {
  noteRecentLater(albums[a].path);  // the shelf's carousel opens on it next time (saved when it's quiet)
  viewAlbum = a;
  viewTracks = (a == playAlbum) ? tracks : listTracks(a);
  if (isResumable(a)) saveResumePos(false);  // so the playing track's bar is current
  viewResume = isResumable(a) ? loadResume(albums[a].path) : std::vector<ResumeEntry>();
  viewSizes.assign(viewTracks.size(), -1);
  int n = viewTracks.size();
  tracksTop = 0;
  if (a == playAlbum && current >= rowsFor(0)) tracksTop = std::max(1, std::min(current - 1, n - rowsFor(1)));
  screen = Screen::Tracks;
  dirtyAll = true;
}

// Books and podcasts: a dot (podcast, never started), a thin bar (part heard), or a
// check (finished). x0..x1 is the row's text area.
static void drawProgressMark(int i, int x0, int x1, int y, int h, bool playing) {
  if (!isResumable(viewAlbum)) return;
  auto& d = M5.Display;
  ResumeEntry* e = findResume(viewResume, fileName(viewTracks[i]));
  if (!e) {
    if (isPodcastAlbum(viewAlbum)) d.fillCircle(x1 - 4, y + 9, 5, C_YELLOW);  // new
    return;
  }
  if (e->done) {  // a check mark
    uint16_t c = playing ? C_PURE : C_GREEN;
    d.drawWideLine(x1 - 14, y + h / 2, x1 - 9, y + h / 2 + 5, 2.5f, c);
    d.drawWideLine(x1 - 9, y + h / 2 + 5, x1, y + h / 2 - 5, 2.5f, c);
    return;
  }
  if (i >= (int)viewSizes.size()) viewSizes.resize(viewTracks.size(), -1);
  if (viewSizes[i] < 0) viewSizes[i] = fileSize(viewTracks[i]);
  uint32_t size = viewSizes[i];
  if (!size || e->pos == 0) return;
  int w = x1 - x0, fill = std::min<int64_t>(w, (int64_t)w * e->pos / size);
  d.fillRoundRect(x0, y + h - 6, w, 3, 1, playing ? rgb(255, 255, 255) : C_OFF);
  d.fillRoundRect(x0, y + h - 6, std::max(3, fill), 3, 1, playing ? C_SHADE : C_YELLOW);
}

// The carousel's middle cover shrinks into the song list's header, in a few steps.
static void shrinkCover(int a) {
  auto& d = M5.Display;
  M5Canvas* big = getCover(a, CV_BIG);
  int px = CV_X, py = CV_Y, ps = CV_BIG;
  for (int f = 1; f <= 4; f++) {
    int s = CV_BIG + (CV_HEAD - CV_BIG) * f / 4, x = CV_X + (TH_X - CV_X) * f / 4, y = CV_Y + (TH_Y - CV_Y) * f / 4;
    d.fillRect(px - 2, py - 2, ps + 4, ps + 4, C_BG);  // a little past the edges: a zoomed draw can spill a pixel
    if (f == 4) break;  // the last step is the header's own cover
    if (big) big->pushRotateZoom(&d, x + s / 2.0f, y + s / 2.0f, 0, (float)s / CV_BIG, (float)s / CV_BIG, COVER_KEY);
    else d.fillRoundRect(x, y, s, s, s / 8, albumTint(a));
    px = x; py = y; ps = s;
    audioPump();
  }
}

// The header: the cover, the album's name (two lines at most), and its artist (or how
// many songs).
static void drawTrackHeader() {
  auto& d = M5.Display;
  drawAlbumArt(viewAlbum, TH_X, TH_Y, CV_HEAD);
  int tx = TH_X + CV_HEAD + 12, tw = 312 - tx;
  d.setFont(&fonts::FreeSansBold12pt7b);
  auto lines = wrapText(albums[viewAlbum].name, tw, 2);
  String sub = albums[viewAlbum].albumArtist;
  if (!sub.length() || isPodcastAlbum(viewAlbum)) {
    int n = viewTracks.size();
    const char* noun = isPodcastAlbum(viewAlbum) ? "episode" : albumKind(viewAlbum) == ShelfKind::Books ? "chapter" : "song";
    sub = String(n) + " " + noun + (n == 1 ? "" : "s");
  }
  int lh = 22, y0 = TH_Y + CV_HEAD / 2 - (int)lines.size() * lh / 2 - 6;
  d.setTextDatum(middle_left);
  d.setTextColor(C_WHITE);
  for (size_t k = 0; k < lines.size(); k++) d.drawString(lines[k], tx, y0 + k * lh + lh / 2);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(C_DIM);
  d.drawString(fitLine(sub, tw), tx, y0 + lines.size() * lh + 9);
}

static void drawTrackRows();
static void drawTracks() {
  auto& d = M5.Display;
  d.fillScreen(C_BG);
  drawNavBar(2);
  if (shrinkFromCarousel && tracksTop == 0) {  // just picked: the big cover shrinks into the header
    drawAlbumArt(viewAlbum, CV_X, CV_Y, CV_BIG);
    shrinkCover(viewAlbum);
  }
  shrinkFromCarousel = false;
  if (tracksTop == 0) drawTrackHeader();
  drawTrackRows();
}

// Just the song rows and the scroll bar (a new song, or a scroll: the card stays).
static void drawTrackRows() {
  auto& d = M5.Display;
  bool podcast = isPodcastAlbum(viewAlbum);
  d.fillRect(tlX() - 2, listTop() - 2, 322 - tlX(), 242 - listTop(), C_BG);
  uint16_t ac = albumTint(viewAlbum);
  int n = viewTracks.size();
  int rowH = trRowH(), rows = trRows();
  for (int r = 0; r < rows; r++) {
    int i = tracksTop + r;
    if (i >= n) break;
    int y = listTop() + r * rowH, h = rowH - 4, my = y + h / 2;
    bool playing = viewAlbum == playAlbum && i == current;
    audioPump();
    d.fillRoundRect(tlX(), y, tlW(), h, 10, playing ? ac : C_ROW);
    if (podcast) {  // date badge + title wrapped over up to three lines
      String f = viewTracks[i].substring(viewTracks[i].lastIndexOf('/') + 1);
      static const char* MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
      int mon = f.length() > 10 && f[4] == '-' ? f.substring(5, 7).toInt() : 0;
      d.fillRoundRect(tlX() + 4, y + 4, 40, h - 8, 8, playing ? C_PURE : C_OFF);
      d.setTextDatum(middle_center);
      d.setTextColor(playing ? ac : C_WHITE);
      if (mon >= 1 && mon <= 12) {
        d.setFont(&fonts::DejaVu9);
        d.drawString(MON[mon - 1], tlX() + 24, my - 6);
        d.setFont(&fonts::FreeSansBold9pt7b);
        d.drawString(String(f.substring(8, 10).toInt()), tlX() + 24, my + 6);
      } else {
        d.setFont(&fonts::FreeSansBold12pt7b);
        d.drawString(String(i + 1), tlX() + 24, my);
      }
      d.setFont(&fonts::FreeSansBold9pt7b);
      int tx = tlX() + 54, tw = tlW() - 54 - 18;
      auto lines = wrapText(trackTitle(viewTracks[i], viewAlbum), tw, 2);
      int lh = 16, ty = my - (int)(lines.size() - 1) * lh / 2;
      d.setTextDatum(middle_left);
      d.setTextColor(playing ? C_PURE : C_WHITE);
      for (size_t k = 0; k < lines.size(); k++) d.drawString(lines[k], tx, ty + k * lh);
      drawProgressMark(i, tx, tlX() + tlW() - 8, y, h, playing);
      continue;
    }
    int nx = tlX() + 17;
    d.fillCircle(nx, my, 12, playing ? C_PURE : C_OFF);
    if (playing) iconPlaying(nx, my, ac);
    else {
      if (n > 99) d.setFont(&fonts::DejaVu12);
      else d.setFont(&fonts::FreeSansBold9pt7b);
      d.setTextDatum(middle_center);
      d.setTextColor(C_WHITE);
      d.drawString(String(i + 1), nx, my);
    }
    int tx = tlX() + 42, tw = tlW() - 42 - (isResumable(viewAlbum) ? 26 : 14);
    d.setFont(&fonts::FreeSansBold9pt7b);  // one size for every title; long ones take two lines
    d.setTextDatum(middle_left);
    d.setTextColor(playing ? C_PURE : C_WHITE);
    auto lines = wrapText(trackTitle(viewTracks[i], viewAlbum), tw, 2);
    int lh = 16, ty = my - (int)(lines.size() - 1) * lh / 2;
    for (size_t k = 0; k < lines.size(); k++) d.drawString(lines[k], tx, ty + k * lh);
    drawProgressMark(i, tx, tlX() + tlW() - 8, y, h, playing);
  }
  if (n > rows) {  // scroll bar
    int trackH = rows * rowH - 4, barH = std::max(16, trackH * rows / n);
    int y0 = listTop(), barY = y0 + (trackH - barH) * tracksTop / std::max(1, n - rowsFor(1));
    d.fillRoundRect(311, y0, 5, trackH, 2, C_ROW);
    d.fillRoundRect(311, std::min(barY, y0 + trackH - barH), 5, barH, 2, C_DIM);
  }
}
