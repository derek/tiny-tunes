// ---------- serial commands ----------
// Line protocol at 460800 baud (driven by upload.py):
//   PING                 -> PONG bt | PONG wifi [<ip>]
//   BAT                  -> BAT <percent> <charging|discharging> <millivolts> <milliamps>
//   VER                  -> VER <version> <bt|wifi>
//   PWR                  -> PWR usb=<mA> bat=<mA> rest=<0|1>  (power draw)
//   REST <0|1>           -> idle CPU rest off/on (for measuring), "OK"
//                           (milliamps: + charging, - draining)
//   LS                   -> one "F <path> <size>" per file (up to two folders deep), then "END"
//   PUT <path> <size>    -> OK, then host sends raw bytes in UP_CHUNK blocks,
//                           device replies "A" after each block, "DONE" at end
//   MV <from>\t<to>      -> moves/renames a file or folder, "OK"
//   RM <path>            -> deletes a file, or a folder and the files in it, "OK"
//   RESCAN               -> rescans the card, replies "OK <albums>"
//   TIME Y M D h m s w   -> sets the RTC to this local time (w = weekday, 0 = Sunday), "OK"
//   WIFI <ssid>\t<pass>  -> saves credentials, replies "OK"
//   MODE <bt|wifi>       -> "OK", rebooting into that mode if not already in it
//   NAME [<name> | -]    -> alone: "NAME <name>" (the owner's name, empty if none);
//                           with a name: sets it; "-" clears it; "OK"
//   LOAD <album>         -> loads the first album whose name contains <album> (paused), "OK"
//   SHOW <screen>        -> home | albums [<shelf>] | songs | player | grownups, "OK"
//   THEME [<name>]       -> switches to that color theme (kept, like Settings -> Colors), "OK";
//                           alone: "THEME <current> <all names>"
//   SHOT                 -> "SHOT 320 240", the screen as raw RGB565 rows, then "END"
//                           (LOAD, SHOW and SHOT are for screenshot.py)
//   COVERS               -> per album "C <folder>\t<cover in use>", then one
//                           "I <image>\t<bytes>\t<w>x<h>\t<modified>" per picture in it; "END"
// The WiFi build prints "IP <addr>" when it joins the network.
static constexpr size_t UP_CHUNK = 2048;

static void showUploadStatus(const String& name, uint32_t done, uint32_t total) {
  auto& d = M5.Display;
  int y = 60;
  d.fillRoundRect(10, y, 300, 120, 18, C_OFF);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(C_WHITE);
  d.setTextDatum(middle_center);
  d.drawString("Adding songs...", 160, y + 30);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.drawString(fitLine(name.substring(name.lastIndexOf('/') + 1), 270), 160, y + 60);
  d.fillRoundRect(26, y + 84, 268, 14, 7, C_ROW);
  d.fillRoundRect(26, y + 84, std::max<int>(14, total ? (int64_t)268 * done / total : 0), 14, 7, C_YELLOW);
}

static void receiveFile(const String& path, uint32_t size) {
  stopPlayback();
  paused = true;
  int slash = path.lastIndexOf('/');
  if (slash > 0) SD.mkdir(path.substring(0, slash));
  File f = SD.open(path, FILE_WRITE);
  if (!f) { Serial.println("ERR open"); return; }
  Serial.println("OK");
  static uint8_t buf[UP_CHUNK];
  uint32_t got = 0, lastDraw = 0;
  while (got < size) {
    size_t want = std::min<uint32_t>(UP_CHUNK, size - got), n = 0;
    uint32_t deadline = millis() + 5000;
    while (n < want && millis() < deadline) {
      int avail = Serial.available();
      if (avail > 0) n += Serial.readBytes(buf + n, std::min<size_t>(avail, want - n));
      else delay(1);
    }
    if (n < want) { f.close(); SD.remove(path); Serial.println("ERR timeout"); return; }
    if (f.write(buf, n) != n) { f.close(); SD.remove(path); Serial.println("ERR write"); return; }
    got += n;
    Serial.println("A");
    if (millis() - lastDraw > 250) { lastDraw = millis(); showUploadStatus(path, got, size); }
  }
  f.close();
  Serial.println("DONE");
}

static void listFiles(const String& dirPath, int depth) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) return;
  while (File f = dir.openNextFile()) {
    String p = joinPath(dirPath, f.name());
    if (isHidden(f.name())) {}
    else if (f.isDirectory()) { if (depth > 0) listFiles(p, depth - 1); }
    else Serial.printf("F %s %u\n", p.c_str(), (unsigned)f.size());
    f.close();
  }
  dir.close();
}

static bool removePath(const String& path) {  // recursive
  File f = SD.open(path);
  if (!f) return false;
  bool isDir = f.isDirectory();
  std::vector<String> children;
  if (isDir)
    while (File g = f.openNextFile()) { children.push_back(joinPath(path, g.name())); g.close(); }
  f.close();
  if (!isDir) return SD.remove(path);
  for (auto& c : children) if (!SD.remove(c)) removePath(c);  // files, then subfolders
  return SD.rmdir(path);
}

static bool idleRest = true;  // "REST 0" over serial turns it off, to compare power draw

static void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line == "PWR") {  // total draw while plugged in (and charged), battery current, idle rest on/off
    M5.Power.Axp192.bitOn(0x82, 0x0C);  // ADC enable 1: VBUS voltage + current (off by default)
    delay(80);                          // let the ADC take a sample
    Serial.printf("PWR usb=%dmA bat=%dmA rest=%d\n", (int)M5.Power.Axp192.getVBUSCurrent(), (int)M5.Power.getBatteryCurrent(), idleRest);
  } else if (line.startsWith("REST ")) {
    idleRest = line.substring(5).toInt() != 0;
    Serial.println("OK");
  } else if (line == "VER") Serial.printf("VER %s %s\n", TT_VERSION, CORE2_WIFI ? "wifi" : "bt");
  else if (line == "PING") {
#if CORE2_WIFI
    if (WiFi.status() == WL_CONNECTED) Serial.printf("PONG wifi %s\n", WiFi.localIP().toString().c_str());
    else Serial.println("PONG wifi");
#else
    Serial.println("PONG bt");
#endif
  } else if (line == "BAT") {
    Serial.printf("BAT %d %s %dmV %dmA\n", M5.Power.getBatteryLevel(),
                  M5.Power.isCharging() ? "charging" : "discharging", M5.Power.getBatteryVoltage(),
                  (int)M5.Power.getBatteryCurrent());
  } else if (line == "DF") {  // card diagnostics: type, size, used, raw root listing
    Serial.printf("DF type=%d size=%lluMB used=%lluMB\n", (int)SD.cardType(), SD.cardSize() >> 20, SD.usedBytes() >> 20);
    File root = SD.open("/");
    Serial.printf("DF root open=%d dir=%d\n", (bool)root, root ? root.isDirectory() : 0);
    while (File f = root.openNextFile()) {
      Serial.printf("DF %s %s %u\n", f.isDirectory() ? "d" : "f", f.name(), (unsigned)f.size());
      f.close();
    }
    root.close();
    Serial.println("END");
  } else if (line == "LS") {
    listFiles("/", 2);
    Serial.println("END");
  } else if (line == "RESCAN") {  // keeps the player's place; never starts music by itself
    rescanKeepingPlace();
    Serial.printf("OK %u\n", (unsigned)albums.size());
  } else if (line.startsWith("MV ")) {
    int tab = line.indexOf('\t');
    if (tab < 0) { Serial.println("ERR bad args"); return; }
    String from = line.substring(3, tab), to = line.substring(tab + 1);
    if (touchesPlaying(from)) stopPlayback();
    int slash = to.lastIndexOf('/');
    if (slash > 0) SD.mkdir(to.substring(0, slash));
    Serial.println(SD.rename(from, to) ? "OK" : "ERR move failed");
  } else if (line.startsWith("RM ")) {
    if (touchesPlaying(line.substring(3))) stopPlayback();
    Serial.println(removePath(line.substring(3)) ? "OK" : "ERR remove failed");
  } else if (line.startsWith("TIME ")) {  // local time from the host, e.g. upload.py
    int v[7] = {0};
    if (sscanf(line.c_str() + 5, "%d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7) {
      Serial.println("ERR bad args");
      return;
    }
    M5.Rtc.setDateTime(m5::rtc_datetime_t{{(int16_t)v[0], (int8_t)v[1], (int8_t)v[2], (int8_t)v[6]}, {(int8_t)v[3], (int8_t)v[4], (int8_t)v[5]}});
    Serial.println("OK");
  } else if (line.startsWith("LOAD ")) {  // an album, paused (screenshots)
    String want = lowerStr(line.substring(5));
    want.trim();
    int a = -1;
    for (size_t i = 0; i < albums.size() && a < 0; i++)
      if (lowerStr(albums[i].name).indexOf(want) >= 0) a = i;
    if (a < 0) { Serial.println("ERR no such album"); return; }
    stopPlayback();
    playAlbum = a;
    tracks = listTracks(a);
    current = tracks.empty() ? -1 : isBook(a) ? bookResumeChapter(a) : 0;
    if (current >= 0) { title = trackTitle(tracks[current], a); artist = guessArtist(a, tracks[current]); }
    paused = true;
    dirtyAll = true;
    Serial.println("OK");
  } else if (line.startsWith("SHOW ")) {  // a screen (screenshots)
    String what = lowerStr(line.substring(5));
    what.trim();
    if (what == "home") showScreen(Screen::Shelves);
    else if (what.startsWith("albums")) {
      String sh = what.substring(6);
      sh.trim();
      int k = playAlbum >= 0 ? albums[playAlbum].shelf : 0;
      for (size_t i = 0; i < shelves.size(); i++)
        if (sh.length() && (lowerStr(shelves[i].name) == sh || lowerStr(shelves[i].path) == "/" + sh)) k = i;  // "pods" or "podcasts"
      openShelf(k, playAlbum);
    } else if (what == "songs" && playAlbum >= 0) {
      shrinkFromCarousel = screen == Screen::Albums;  // from the carousel: with its animation, like a tap
      openTracks(playAlbum);
    }
    else if (what == "player") showScreen(Screen::Player);
    else if (what == "grownups") { guTab = 0; beforeGrownups = screen; showScreen(Screen::Grownups); }
    else { Serial.println("ERR bad screen"); return; }
    Serial.println("OK");
  } else if (line == "THEME" || line.startsWith("THEME ")) {
    String want = lowerStr(line.substring(5));
    want.trim();
    if (!want.length()) {
      Serial.printf("THEME %s", THEMES[themeIdx].name);
      for (auto& t : THEMES) Serial.printf(" %s", t.name);
      Serial.println();
      return;
    }
    for (int k = 0; k < THEME_COUNT; k++)
      if (lowerStr(THEMES[k].name) == want) {
        themeIdx = k;
        prefs.putUChar("theme", k);
        dirtyAll = true;
        Serial.println("OK");
        return;
      }
    Serial.println("ERR no such theme");
  } else if (line == "COVERS") {  // which picture each album uses, and how big they all are
    for (auto& al : albums) {
      Serial.printf("C %s\t%s\n", al.path.c_str(), al.coverFile.length() ? al.coverFile.c_str() : al.artFile.length() ? "(in the songs' tags)" : "(none)");
      File dir = SD.open(al.path);
      while (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        if (!f) break;
        String n = f.name();
        String lower = lowerStr(n);
        time_t mt = f.getLastWrite();
        uint32_t size = f.size();
        f.close();
        if (n[0] == '.' || !(lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png"))) continue;
        int w = 0, h = 0;
        String path = joinPath(al.path, n.c_str());
        bool ok = lower.endsWith(".png") ? pngSize(path, w, h) : jpegSize(path, w, h);
        tm t;
        gmtime_r(&mt, &t);
        Serial.printf("I %s\t%u\t%dx%d%s\t%04d-%02d-%02d %02d:%02d\n", n.c_str(), (unsigned)size, w, h, ok ? "" : " (can't read)",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
      }
      if (dir) dir.close();
    }
    Serial.println("END");
  } else if (line == "SHOT") {  // what's on the screen, read back from the display
    static uint16_t row[320];
    Serial.println("SHOT 320 240");
    for (int y = 0; y < 240; y++) {
      M5.Display.readRect(0, y, 320, 1, row);
      Serial.write((const uint8_t*)row, sizeof row);
    }
    Serial.println("END");
  } else if (line == "NAME") {  // just asking: never clears it
    Serial.printf("NAME %s\n", owner.c_str());
  } else if (line.startsWith("NAME ")) {
    String name = line.substring(5);
    name.trim();
    setOwner(name == "-" ? String() : name);
    Serial.println("OK");
  } else if (line.startsWith("WIFI ")) {
    int tab = line.indexOf('\t');
    if (tab < 0) { Serial.println("ERR bad args"); return; }
    prefs.putString("ssid", line.substring(5, tab));
    prefs.putString("pass", line.substring(tab + 1));
    Serial.println("OK");
#if CORE2_WIFI
    WiFi.disconnect();
    wifiStart();
#endif
  } else if (line.startsWith("MODE ")) {
    bool wantWifi = line.substring(5) == "wifi";
    if (wantWifi == (bool)CORE2_WIFI) Serial.println("OK");
    else switchMode();
  } else if (line.startsWith("PUT ")) {
    int sp = line.lastIndexOf(' ');
    String path = line.substring(4, sp);
    uint32_t size = line.substring(sp + 1).toInt();
    if (!path.startsWith("/") || sp <= 4) { Serial.println("ERR bad args"); return; }
    receiveFile(path, size);
    dirtyAll = true;
  }
}
