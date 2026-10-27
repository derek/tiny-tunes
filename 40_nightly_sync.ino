// ================= Nightly podcast sync =================
// Once a night between midnight and 5 am, when it's plugged in and nothing is playing,
// the Core2 syncs podcasts. In Bluetooth mode it quietly reboots into WiFi mode, syncs,
// and reboots back. The screen stays dark throughout.
static constexpr uint32_t NIGHT_WIFI_WAIT_MS = 3 * 60000UL, NIGHT_MAX_MS = 90 * 60000UL;

static uint32_t todayStamp() {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.isEnabled() || !M5.Rtc.getDateTime(&dt) || dt.date.year < 2024) return 0;
  return dt.date.year * 10000UL + dt.date.month * 100 + dt.date.date;
}

static bool haveFeeds() {
  File f = SD.open("/podcasts/feeds.txt");
  bool ok = f && f.size() > 0;
  if (f) f.close();
  return ok;
}

// Remembers what's on screen so the reboots come back to it, dark.
static void nightReboot(bool toWifi) {
  prefs.putUChar("nightsync", toWifi);
  prefs.putUChar("nightboot", 1);
  switchMode(true);
}

static bool nightReturnPending = false;  // synced; back to Bluetooth once nobody's using it

static void nightSyncDone() {
  uint32_t today = todayStamp();
  if (today) prefs.putUInt("nightdone", today);
  nightSync = false;
#if CORE2_WIFI
  nightReturnPending = true;
#endif
}

static void nightlyCheck() {
  static uint32_t last = 0;
  uint32_t now = millis();
#if CORE2_WIFI
  if (nightSync) {  // give up if WiFi never comes or the sync drags on
    if ((!webUp && now > NIGHT_WIFI_WAIT_MS) || now > NIGHT_MAX_MS) nightSyncDone();
    return;
  }
  if (nightReturnPending) {  // a kid may have woken it up: don't cut off their music
    if (!musicIsPlaying() && now - lastTouchMs > 60000) { nightReturnPending = false; nightReboot(false); }
    return;
  }
#endif
  if (now - last < 30000) return;
  last = now;
  int h, m;
  if (!rtcNow(h, m) || h >= 5) return;
  uint32_t today = todayStamp();
  if (!today || prefs.getUInt("nightdone", 0) == today) return;
  if (!usbPower() || musicIsPlaying()) return;
  if (now - lastTouchMs < 10 * 60000UL) return;  // someone's using it
  if (!prefs.isKey("ssid") || !haveFeeds()) { prefs.putUInt("nightdone", today); return; }  // nothing to do tonight
#if CORE2_WIFI
  if (syncState == SyncState::Idle) { prefs.putUInt("nightdone", today); requestSync(); }
#else
  nightReboot(true);
#endif
}
