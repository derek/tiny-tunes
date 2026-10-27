// ---------- startup splash ----------
// "Tiny Tunes" in rainbow letters with music notes, shown while the card loads (at least 5 s).
static constexpr uint32_t SPLASH_MS = 5000;

static void drawNote(int x, int y, int s, uint16_t c) {  // an eighth note; (x, y) = note head
  auto& d = M5.Display;
  d.fillEllipse(x, y, s * 6 / 10, s * 45 / 100, c);
  d.fillRect(x + s * 45 / 100, y - s * 2, std::max(2, s / 5), s * 2, c);
  d.fillTriangle(x + s * 45 / 100, y - s * 2, x + s * 45 / 100 + s * 7 / 10, y - s * 13 / 10, x + s * 45 / 100, y - s * 14 / 10, c);
}

static void drawSplash() {
  auto& d = M5.Display;
  d.fillScreen(C_BG);
  static const uint8_t RAINBOW[][3] = {{255, 89, 94}, {255, 146, 76}, {250, 205, 40}, {80, 210, 110}, {25, 170, 230}, {150, 100, 240}, {255, 100, 170}};
  const char* text = "Tiny Tunes";
  d.setFont(&fonts::FreeSansBold24pt7b);
  d.setTextSize(1.25f);
  d.setTextDatum(middle_left);
  int x = 160 - d.textWidth(text) / 2, y = 112, k = 0;
  for (const char* p = text; *p; p++) {
    char ch[2] = {*p, 0};
    if (*p != ' ') {
      const uint8_t* c = RAINBOW[k++ % 7];
      d.setTextColor(rgb(c[0], c[1], c[2]));
      d.drawString(ch, x, y);
    }
    x += d.textWidth(ch);
  }
  d.setTextSize(1);
  drawNote(42, 60, 14, rgb(250, 205, 40));
  drawNote(272, 52, 12, rgb(25, 170, 230));
  drawNote(286, 160, 10, rgb(255, 100, 170));
  drawNote(30, 168, 11, rgb(80, 210, 110));
  if (owner.length()) {
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(middle_center);
    d.setTextColor(C_YELLOW);
    d.drawString(fitLine("Hi, " + owner + "!", 280), 160, 158);
  }
  d.setFont(&fonts::DejaVu9);
  d.setTextDatum(bottom_center);
  d.setTextColor(C_DIM);
  d.drawString(String(TT_VERSION) + (CORE2_WIFI ? "  WiFi mode" : ""), 160, 238);
}

void setup() {
  Serial.setRxBufferSize(UP_CHUNK * 4);
  auto cfg = M5.config();
  cfg.serial_baudrate = 460800;  // the Core2's CH9102 garbles 921600
  M5.begin(cfg);
  M5.Display.setRotation(ROTATION);
  M5.Touch.setFlickThresh(16);  // a tap may move a little before it counts as a swipe (default 8 px)
  prefs.begin("core2mp3");  // settings namespace from the project's first name; renaming it would reset them
  noteLastRestart();
  bool nightBoot = prefs.getUChar("nightboot", 0);  // a quiet reboot for the nightly podcast sync
  M5.Display.setBrightness(nightBoot ? 0 : 140);   // the saved brightness is applied below
  M5.Display.fillScreen(C_BG);
  volLevel = prefs.getUChar("lvl", 6);
  maxVolPct = prefs.getUChar("maxvol", 70);
  owner = prefs.getString("owner", "");
  brightIdx = constrain((int)prefs.getUChar("bright", 1), 0, 2);
  autoOffIdx = constrain((int)prefs.getUChar("autooff", 0), 0, 2);
  themeIdx = constrain((int)prefs.getUChar("theme", 0), 0, THEME_COUNT - 1);
  nightSync = CORE2_WIFI && prefs.getUChar("nightsync", 0);
  listenSec = prefs.getUInt("listensec", 0);
  songsPlayed = prefs.getUInt("played", 0);
  if (!nightBoot) applyBrightness();

  {  // the speaker task does the I2S work; pin it off the decode core
    auto spk = M5.Speaker.config();
    spk.sample_rate = 96000;
    spk.task_pinned_core = APP_CPU_NUM;
    M5.Speaker.config(spk);
  }
  M5.Speaker.begin();

  uint32_t splashAt = millis();
  drawSplash();  // stays up while the card loads

  bool sdOk = false;
  for (int tries = 0; tries < 10 && !(sdOk = SD.begin(SD_CS, SPI, 25000000)); tries++) delay(300);
  if (!sdOk) {
    M5.Display.fillScreen(C_BG);
    Serial.println("ERR no SD card");
    drawTitle("Oops! The music card is missing. Ask a grown-up.", 160, 120, 290, 120, rgb(255, 120, 120));
    while (true) delay(1000);
  }

  loadRecent();
  rescanLibrary();
  prepareCovers();
  prepareCarouselArt();
  while (!nightBoot && millis() - splashAt < SPLASH_MS) delay(10);  // splash for at least 5 s (not on dark night boots)
  M5.Display.fillScreen(C_BG);
  Serial.printf("READY %s %u albums %s\n", CORE2_WIFI ? "wifi" : "bt", (unsigned)albums.size(), TT_VERSION);

#if CORE2_WIFI
  setupWeb();
  wifiStart();
#else
  // Bluetooth: reconnects to the last device automatically, else keeps scanning
  // and reports devices to onBtDeviceFound for the picker.
  ring = (Frame*)heap_caps_malloc(RING_LEN * sizeof(Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  btLock = xSemaphoreCreateMutex();
  a2dp.set_local_name("Tiny Tunes");
  a2dp.set_ssid_callback(onBtDeviceFound);
  a2dp.set_on_connection_state_changed(onBtState);
  a2dp.set_auto_reconnect(true);
  a2dp.set_volume_control(&btVolume);
  a2dp.set_data_callback_in_frames(btGetFrames);
  a2dp.start();
  a2dp.continuousScan(false);  // scan in bursts (see TinyA2DP)
#endif
  applyVolume();

  mp3 = new AudioGeneratorMP3();
  // pick up on the last album and song, but wait for play
  int resumeAlbum = std::max(0, findAlbum(prefs.getString("album", "")));
  int resumeTrack = std::max<int>(0, prefs.getInt("track", 0));
  if (!albums.empty()) {
    playAlbum = resumeAlbum;
    tracks = listTracks(resumeAlbum);
    if (!tracks.empty()) {
      current = std::min<int>(resumeTrack, tracks.size() - 1);
      title = trackTitle(tracks[current], playAlbum); artist = guessArtist(playAlbum, tracks[current]);
      paused = true;  // togglePause() starts it
    }
  }
  lastTouchMs = millis();
  if (nightBoot) {  // stay dark
    prefs.remove("nightboot");
    if (!nightSync) prefs.remove("nightsync");
    dimmed = true;
    M5.Display.setBrightness(0);
  }
}

void loop() {
  M5.update();
  handleSerial();

  if (mp3->isRunning() && !paused && out.hasRoom() && !mp3->loop()) {  // track finished... or hit a damaged spot
    bool nearEnd = !file || file->getPos() + 65536 >= file->getSize();
    if (!nearEnd && recoverTries < 3) {  // jump a little past the damage and carry on
      recoverTries++;
      forceStartPos = file->getPos() + 16384;
      playTrack(current);
      return;
    }
    trackJustFinished = true;
    saveResumePos(true);
    if (isResumable(playAlbum) && current + 1 >= (int)tracks.size()) {  // end of the book / last episode: stop
      stopPlayback();
      paused = true;
      dirtyNow = true;
    } else {
      playTrack(current + 1);
    }
    trackJustFinished = false;
  }
  static uint32_t lastResumeSave = 0;
  if (musicIsPlaying() && isResumable(playAlbum) && millis() - lastResumeSave > 15000) { lastResumeSave = millis(); saveResumePos(false); }

#if CORE2_WIFI
  wifiLoop();
#else
  {  // Bluetooth scanning: continuous on the grown-ups Bluetooth tab, else a burst a minute
    static bool continuous = false;
    static uint32_t lastBurst = 0;
    bool pairing = screen == Screen::Grownups && guTab == 1;
    if (pairing != continuous) { continuous = pairing; a2dp.continuousScan(pairing); }
    if (!pairing && !btConnected && millis() - lastBurst > 60000) { lastBurst = millis(); a2dp.scanBurst(); }
  }
  if (btStateDirty) {
    btStateDirty = false;
    // the speaker's amplifier off while Bluetooth plays (no point drawing power for
    // it just as the radio starts sending), on again when the headphones go
    if (btConnected) { M5.Speaker.stop(SPK_CH); M5.Speaker.end(); }
    else { btHasTarget = false; M5.Speaker.begin(); }
    // Headphones gone (switched off, out of range): pause rather than carry on out loud
    snackPaused = !btConnected && current >= 0 && !paused;
    if (snackPaused) togglePause();
    applyVolume();
    showSnack(btConnected ? Snack::Headphones : Snack::Speaker);
    connectDirty = true;
  }
#endif

  rtcCrumb = CRUMB_MAGIC | (btConnected ? 1 : 0) | (musicIsPlaying() ? 2 : 0);  // for noteLastRestart()
  handleInput();
  buzzService();
  nightlyCheck();
  trackListening();
  watchPower();
  serviceSnack();
  static uint32_t lastStats = 0;
  if (screen == Screen::Grownups && guTab == 2 && millis() - lastStats > 2000) { lastStats = millis(); drawStats(); }
  handleIdle();
  redraw();

  static uint32_t lastTick = 0, lastBounce = 0;
  uint32_t now = millis();
  bool library = screen != Screen::Grownups;
  if (library && musicIsPlaying() && !dimmed && now - lastBounce > 150) {
    lastBounce = now;
    drawNowPlayingButton(true);  // the green button's bars dance while music plays
  }
  tickKeepTitle(now);  // Home: Keep listening's long title scrolls
  tickHomeStatus(now);  // Home: the clock, battery and headphones in the top bar
  if (recentUnsaved && !musicIsPlaying()) saveRecent();  // an album opened while it played
  if (litControl >= 0 && (int32_t)(now - litUntil) >= 0) {
    if (screen == Screen::Player && !dimmed) drawControl(litControl, false);
    litControl = -1;
  }
  if (screen == Screen::Player && !dimmed && !tracks.empty() && now - lastTick > 1000) {
    lastTick = now;
    if (!snackUp()) drawPlayProgress();  // the snackbar sits over it
    if (storyMode()) drawTimeLeft(false);  // books and podcasts: minutes left
  }

  // Nothing needs fast service (no audio to decode, no download): let the CPU rest a
  // few ms per pass instead of spinning flat out. Touch still feels instant at ~150 Hz.
  bool busy = musicIsPlaying();
#if CORE2_WIFI
  busy |= syncState != SyncState::Idle;
#endif
  if (idleRest && !busy) delay(6);
}
