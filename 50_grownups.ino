// ================= Grown-ups (hold two of the touch buttons for 3 s) =================
// Header with Done, then three tabs: Settings, Bluetooth (WiFi in WiFi mode), Stats.
static int guTab = 0, statsPage = 0;
static Screen beforeGrownups = Screen::Player;
static int brightIdx = 1, autoOffIdx = 0;        // saved in prefs
static const uint8_t BRIGHTNESS[3] = {60, 140, 230};
static const uint16_t AUTO_OFF_MIN[3] = {15, 30, 0};  // 0 = never

static constexpr int GU_TABS_Y = 40, GU_TAB_H = 28, GU_BODY_Y = 76;
static constexpr int SEG_X = 92, SEG_W = 220, SEG_H = 26;  // settings: 3-way choices on the right
static constexpr int GU_ROWS = 3, STATS_PAGES = 3;
static const int GU_ROW_Y[GU_ROWS] = {80, 112, 144};
static constexpr int GU_THEME_Y = 176;  // Colors: < name >, a tap on either half steps through
static constexpr int GU_MODE_Y = 206;  // full-width mode switch button

static void drawSegments(int x, int y, int w, int h, const char* const* labels, int n, int sel, uint16_t on) {
  auto& d = M5.Display;
  d.fillRoundRect(x, y, w, h, h / 2, C_ROW);
  int sw = w / n;
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  for (int i = 0; i < n; i++) {
    if (i == sel) d.fillRoundRect(x + i * sw + 2, y + 2, sw - 4, h - 4, (h - 4) / 2, on);
    d.setTextColor(i == sel ? C_SHELF_INK : C_DIM);  // the picked one sits on a shelf color
    d.drawString(labels[i], x + i * sw + sw / 2, y + h / 2 + 1);
  }
}

static int segmentHit(int x, int y, int w, int h, int n, int tx, int ty) {
  if (tx < x || tx >= x + w || ty < y - 2 || ty >= y + h + 2) return -1;
  return (tx - x) * n / w;
}

// "1h 23m", "4m"
static String duration(uint32_t sec) {
  uint32_t h = sec / 3600, m = sec / 60 % 60;
  return h ? String(h) + "h " + String(m) + "m" : String(m) + "m";
}

// "3.98" from millivolts, "1.4" from tenths, without printf floats
static String fixed(int32_t v, int div, int decimals) {
  String s = v < 0 ? "-" : "";
  v = abs(v);
  s += String(v / div);
  if (decimals) {
    int frac = v % div;
    for (int k = div / 10; decimals-- > 0 && k > 0; k /= 10) { s += k == div / 10 ? "." : ""; s += String(frac / k % 10); }
  }
  return s;
}

static String bytesText(uint64_t b) {
  if (b >= 1000000000ULL) return fixed(b / 100000000ULL, 10, 1) + " GB";
  if (b >= 1000000ULL) return fixed(b / 100000ULL, 10, 1) + " MB";
  return String((uint32_t)(b / 1000)) + " KB";
}

static void drawStatTile(int col, int row, const String& label, const String& value, uint16_t accent, bool wide = false) {
  auto& d = M5.Display;
  int x = 6 + col * 157, y = GU_BODY_Y + row * 36, w = wide ? 308 : 151, h = 33;
  d.fillRoundRect(x, y, w, h, 8, C_ROW);
  d.fillRoundRect(x, y, 4, h, 2, accent);
  d.setTextDatum(top_left);
  d.setFont(&fonts::DejaVu9);
  d.setTextColor(C_DIM);
  d.drawString(label, x + 10, y + 3);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(C_WHITE);
  d.drawString(fitLine(value, w - 16), x + 10, y + 14);
}

// ----- why it last restarted -----
// A crash, a watchdog (frozen) or a brownout (the battery dipping) is remembered with
// what was going on, for grown-ups -> Stats; the serial port says it at boot too.
static String restartText() {
  int why = prefs.getUChar("rstwhy", 0), n = prefs.getUShort("rstn", 0), crumb = prefs.getUChar("rstcrumb", 0);
  if (!n) return "None";
  const char* what = why == ESP_RST_BROWNOUT ? "Power dip" : why == ESP_RST_PANIC ? "Crash" : "Froze";
  String s = what;
  if (crumb & 1) s += ", Bluetooth on";
  if (crumb & 2) s += ", playing";
  return s + " (" + String(n) + "x)";
}

static void noteLastRestart() {
  esp_reset_reason_t why = esp_reset_reason();
  bool bad = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT ||
             why == ESP_RST_BROWNOUT;
  uint32_t crumb = (rtcCrumb & 0xFFFF0000) == CRUMB_MAGIC ? rtcCrumb & 0xFF : 0;
  if (bad) {
    prefs.putUChar("rstwhy", why);
    prefs.putUChar("rstcrumb", crumb);
    prefs.putUShort("rstn", prefs.getUShort("rstn", 0) + 1);
  }
  Serial.printf("BOOT reset %d%s crumb %u\n", (int)why, bad ? " (bad)" : "", (unsigned)crumb);
  rtcCrumb = CRUMB_MAGIC;
}

static void drawStats() {
  auto& d = M5.Display;
  d.fillRect(0, GU_BODY_Y, 320, 240 - GU_BODY_Y, C_BG);
  if (statsPage == 0) {  // device
    int pct = M5.Power.getBatteryLevel(), mv = M5.Power.getBatteryVoltage(), ma = M5.Power.getBatteryCurrent();
    bool usb = usbPower();
    String power;
    if (ma > 5) power = "Charging +" + String(ma) + " mA";
    else if (ma < -5) power = String(-ma) + " mA, ~" + duration((uint32_t)(390UL * 36 * pct / -ma)) + " left";  // 390 mAh cell
    else power = usb ? "On USB, full" : "Idle";
    float imuTemp = NAN;
    M5.Imu.getTemp(&imuTemp);
    String temp = fixed((int)(M5.Power.Axp192.getInternalTemperature() * 10), 10, 0) + " C power chip";
    if (!isnan(imuTemp)) temp += ", " + fixed((int)(imuTemp * 10), 10, 0) + " C inside";
    static uint64_t sdUsed = 0, sdTotal = 0;
    if (songCountCache < 0) {  // slow-ish (lists every album, scans the FAT), so once per library change
      songCountCache = 0;
      for (size_t a = 0; a < albums.size(); a++) songCountCache += (int)a == playAlbum ? tracks.size() : countTracks(a);
      sdUsed = SD.usedBytes();
      sdTotal = SD.totalBytes();
    }
    drawStatTile(0, 0, "BATTERY", String(pct) + "%  " + fixed(mv, 1000, 2) + " V", pct > 20 ? C_GREEN : rgb(255, 70, 70));
    drawStatTile(1, 0, "POWER", power, C_YELLOW);
    drawStatTile(0, 1, "TEMPERATURE", temp, C_ORANGE);
    drawStatTile(1, 1, "UPTIME", duration(millis() / 1000), C_BLUE);
    drawStatTile(0, 2, "SD CARD", bytesText(sdUsed) + " of " + bytesText(sdTotal), rgb(138, 80, 236));
    drawStatTile(1, 2, "LIBRARY", String(albums.size()) + " albums, " + String(songCountCache) + " songs", rgb(255, 70, 160));
    drawStatTile(0, 3, "VERSION", String(TT_VERSION) + (CORE2_WIFI ? "  (WiFi mode)" : "  (Bluetooth mode)"), C_GREEN, true);
  } else if (statsPage == 1) {  // listening
    drawStatTile(0, 0, "LISTENING TIME", duration(listenSec), C_GREEN);
    drawStatTile(1, 0, "SONGS PLAYED", String(songsPlayed), C_GREEN);
#if CORE2_WIFI
    drawStatTile(0, 1, "WIFI", WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm signal" : String("Not connected"), C_BLUE);
#else
    xSemaphoreTake(btLock, portMAX_DELAY);
    String bt = btConnected ? (btName.length() ? btName : String("Connected")) : String("Speaker");
    xSemaphoreGive(btLock);
    drawStatTile(0, 1, "PLAYING TO", bt, C_BLUE);
#endif
    drawStatTile(1, 1, "VOLUME", String(volStep()) + " of 5 (top = " + String(maxVolPct) + "%)", C_ORANGE);
    drawStatTile(0, 2, "OWNER", owner.length() ? owner : String("Not set"), C_YELLOW, true);
  } else {  // technical
    const esp_partition_t* run = esp_ota_get_running_partition();
    drawStatTile(0, 0, "CHIP", "ESP32 rev " + String(ESP.getChipRevision()) + ", " + String(ESP.getCpuFreqMHz()) + " MHz", C_DIM);
    drawStatTile(1, 0, "LOWEST FREE RAM", String(ESP.getMinFreeHeap() / 1024) + " KB", C_DIM);
    drawStatTile(0, 1, "FLASH", String(ESP.getFlashChipSize() >> 20) + " MB, running " + String(run ? run->label : "?"), C_DIM);
    drawStatTile(1, 1, "PSRAM", fixed(ESP.getPsramSize() / 100000, 10, 1) + " MB total", C_DIM);
    drawStatTile(0, 2, "LAST CRASH", restartText(), C_DIM);
    drawStatTile(1, 2, "BUILT", String(__DATE__) + " " + String(__TIME__).substring(0, 5), C_DIM);
    drawStatTile(0, 3, "MEMORY FREE", String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) + " KB + " +
                 fixed(ESP.getFreePsram() / 100000, 10, 1) + " MB PSRAM", C_BLUE, true);
  }
  for (int p = 0; p < STATS_PAGES; p++) d.fillCircle(144 + p * 16, 236, 3, p == statsPage ? C_WHITE : C_DIM);
}

static void drawGrownupsBody() {
  auto& d = M5.Display;
  d.fillRect(0, GU_BODY_Y, 320, 240 - GU_BODY_Y, C_BG);
  if (guTab == 0) {
    static const char* const vol[] = {"50%", "70%", "100%"};
    static const char* const bright[] = {"Dim", "Normal", "Bright"};
    static const char* const off[] = {"15 min", "30 min", "Never"};
    const char* const* opts[GU_ROWS] = {vol, bright, off};
    static const char* names[GU_ROWS] = {"Loudest", "Screen", "Auto-off"};
    int sel[GU_ROWS] = {maxVolPct == 50 ? 0 : maxVolPct == 70 ? 1 : 2, brightIdx, autoOffIdx};
    for (int r = 0; r < GU_ROWS; r++) {
      d.setFont(&fonts::FreeSansBold9pt7b);
      d.setTextDatum(middle_left);
      d.setTextColor(C_WHITE);
      d.drawString(names[r], 10, GU_ROW_Y[r] + SEG_H / 2);
      drawSegments(SEG_X, GU_ROW_Y[r], SEG_W, SEG_H, opts[r], 3, sel[r], C_BLUE);
    }
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextDatum(middle_left);
    d.setTextColor(C_WHITE);
    d.drawString("Colors", 10, GU_THEME_Y + SEG_H / 2);
    d.fillRoundRect(SEG_X, GU_THEME_Y, SEG_W, SEG_H, SEG_H / 2, C_ROW);
    iconArrow(SEG_X + 16, GU_THEME_Y + SEG_H / 2, 0, C_DIM);
    iconArrow(SEG_X + SEG_W - 16, GU_THEME_Y + SEG_H / 2, 1, C_DIM);
    for (int k = 0; k < 4; k++)  // a swatch of the theme: Pods, Music, Books, play
      d.fillCircle(SEG_X + 40 + k * 13, GU_THEME_Y + SEG_H / 2, 5, k == 0 ? C_BERRY : k == 1 ? C_BLUE : k == 2 ? C_ORANGE : C_ACCENT);
    d.setTextDatum(middle_center);
    d.drawString(THEMES[themeIdx].name, SEG_X + 130, GU_THEME_Y + SEG_H / 2 + 1);
    d.fillRoundRect(6, GU_MODE_Y, 308, 26, 13, TH(modeBtn));
    d.setTextDatum(middle_center);
    d.setTextColor(C_WHITE);
    d.drawString(CORE2_WIFI ? "Switch to Bluetooth mode  >" : "Switch to WiFi mode  >", 160, GU_MODE_Y + 14);
  } else if (guTab == 1) {
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextDatum(middle_left);
#if CORE2_WIFI
    d.setTextColor(C_WHITE);
    d.drawString(wifiStatusText(), 10, GU_BODY_Y + 12);
    if (webUp) {
      d.setTextColor(C_DIM);
      d.drawString("Signal " + String(WiFi.RSSI()) + " dBm. Open this in a browser:", 10, GU_BODY_Y + 36);
      d.setFont(&fonts::FreeSansBold12pt7b);
      d.setTextColor(C_YELLOW);
      d.drawString("http://" + WiFi.localIP().toString(), 10, GU_BODY_Y + 66);
      d.setFont(&fonts::FreeSansBold9pt7b);
      d.setTextColor(C_DIM);
      d.drawString("Add albums, organize, clean up,", 10, GU_BODY_Y + 98);
      d.drawString("and control the music from there.", 10, GU_BODY_Y + 118);
    }
#else
    xSemaphoreTake(btLock, portMAX_DELAY);
    d.setTextColor(btConnected ? C_GREEN : C_WHITE);
    d.drawString(btConnected ? "Playing to " + (btName.length() ? btName : String("headphones"))
                             : String("Not connected: using the speaker"), 10, GU_BODY_Y + 10);
    if (btFound.empty()) {
      d.setTextColor(C_DIM);
      d.drawString("Looking for headphones and speakers...", 10, GU_BODY_Y + 40);
      d.drawString("Put yours in pairing mode.", 10, GU_BODY_Y + 60);
    }
    for (int r = 0; r < 3 && r < (int)btFound.size(); r++) {
      auto& dev = btFound[r];
      int y = GU_BODY_Y + 24 + r * 31;
      bool tgt = btHasTarget && !memcmp(dev.addr, btTarget, ESP_BD_ADDR_LEN);
      d.fillRoundRect(6, y, 308, 28, 8, tgt ? C_BLUE : C_ROW);
      d.setTextColor(C_WHITE);
      d.drawString(fitLine(dev.name, 200), 14, y + 14);
      d.setTextDatum(middle_right);
      d.setTextColor(tgt ? C_WHITE : C_DIM);
      d.drawString(tgt ? (btConnected ? "connected" : "connecting...") : "tap to pair", 306, y + 14);
      d.setTextDatum(middle_left);
    }
    xSemaphoreGive(btLock);
    d.fillRoundRect(6, GU_MODE_Y, 308, 28, 14, TH(modeBtn));
    d.setTextDatum(middle_center);
    d.setTextColor(C_WHITE);
    d.drawString("Forget headphones", 160, GU_MODE_Y + 15);
#endif
  } else {
    drawStats();
  }
}

static void drawGrownups() {
  auto& d = M5.Display;
  d.fillScreen(C_BG);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(C_WHITE);
  d.setTextDatum(middle_left);
  d.drawString("Grown-ups", 10, 19);
  d.fillRoundRect(236, 6, 76, 27, 13, C_ACCENT);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  d.setTextColor(C_ACCENT_INK);
  d.drawString("Done", 274, 20);
  d.setTextColor(C_WHITE);
  static const char* const tabs[] = {"Settings", CORE2_WIFI ? "WiFi" : "Bluetooth", "Stats"};
  drawSegments(6, GU_TABS_Y, 308, GU_TAB_H, tabs, 3, guTab, C_BERRY);
  drawGrownupsBody();
}

static void applyBrightness() { M5.Display.setBrightness(BRIGHTNESS[brightIdx]); }

static void handleGrownups(const m5::touch_detail_t& t) {
  if (statsPage >= 0 && guTab == 2 && t.wasFlicked() && abs(t.distanceX()) > 40) {
    buzz(); statsPage = (statsPage + 1) % STATS_PAGES; drawStats(); return;
  }
  if (!t.wasClicked()) return;
  if (t.y < GU_TABS_Y - 4) {  // header
    if (t.x >= 226) { buzz(); showScreen(beforeGrownups); }
    return;
  }
  int tab = segmentHit(6, GU_TABS_Y, 308, GU_TAB_H, 3, t.x, t.y);
  if (tab >= 0 && t.y < GU_BODY_Y - 2) {
    if (tab != guTab) { buzz(); guTab = tab; statsPage = 0; drawGrownups(); }
    return;
  }
  if (guTab == 0) {
    for (int r = 0; r < GU_ROWS; r++) {
      int i = segmentHit(SEG_X, GU_ROW_Y[r], SEG_W, SEG_H, 3, t.x, t.y);
      if (i < 0) continue;
      buzz();
      if (r == 0) { maxVolPct = i == 0 ? 50 : i == 1 ? 70 : 100; prefs.putUChar("maxvol", maxVolPct); applyVolume(); }
      if (r == 1) { brightIdx = i; prefs.putUChar("bright", i); applyBrightness(); }
      if (r == 2) { autoOffIdx = i; prefs.putUChar("autooff", i); }
      drawGrownupsBody();
      return;
    }
    if (t.y >= GU_THEME_Y - 4 && t.y < GU_THEME_Y + SEG_H + 4 && t.x >= SEG_X) {  // Colors: back or on
      buzz();
      themeIdx = (themeIdx + (t.x < SEG_X + SEG_W / 2 ? THEME_COUNT - 1 : 1)) % THEME_COUNT;
      prefs.putUChar("theme", themeIdx);
      drawGrownups();  // the whole screen in the new colors
      return;
    }
    if (t.y >= GU_MODE_Y - 4) { buzz(); switchMode(); }
  } else if (guTab == 1) {
#if !CORE2_WIFI
    if (t.y >= GU_MODE_Y - 4) {  // forget: drop link and saved device, use speaker
      buzz();
      btHasTarget = false;
      a2dp.disconnect();
      a2dp.clean_last_connection();
      connectDirty = true;
      return;
    }
    int r = (t.y - (GU_BODY_Y + 24)) / 31;
    if (t.y < GU_BODY_Y + 24 || r >= 3) return;
    xSemaphoreTake(btLock, portMAX_DELAY);
    if (r < (int)btFound.size()) {
      buzz();
      if (btConnected) a2dp.disconnect();
      memcpy(btTarget, btFound[r].addr, ESP_BD_ADDR_LEN);
      btHasTarget = true;  // connects on the next discovery hit (a few seconds)
    }
    xSemaphoreGive(btLock);
    connectDirty = true;
#endif
  } else {  // stats: tap to flip pages
    buzz();
    statsPage = (statsPage + 1) % STATS_PAGES;
    drawStats();
  }
}

// Listening time and songs played, for the stats page.
static void trackListening() {
  static uint32_t lastSec = 0, unsaved = 0;
  uint32_t now = millis() / 1000;
  if (now == lastSec) return;
  lastSec = now;
  if (musicIsPlaying()) { listenSec++; unsaved++; }
  if (unsaved >= 60) { unsaved = 0; prefs.putUInt("listensec", listenSec); }
}

// The library was rescanned: album indexes changed, so nothing may keep pointing at the old ones.
static void onLibraryChanged() {
  shelfAlbums.clear();
  viewTracks.clear();
  viewResume.clear();
  viewSizes.clear();
  bookSizesFor = "";  // chapter sizes may have changed
  viewAlbum = -1;
  auto stale = [](Screen s) { return s == Screen::Albums || s == Screen::Tracks || s == Screen::Shelves; };
  if (stale(screen)) screen = Screen::Player;
  if (stale(beforeGrownups)) beforeGrownups = Screen::Player;
  dirtyAll = true;
}
