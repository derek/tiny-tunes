#if !CORE2_WIFI
// ---------- Bluetooth device discovery (written from BT task) ----------
struct BtDevice { String name; esp_bd_addr_t addr; int rssi; };
static std::vector<BtDevice> btFound;
static SemaphoreHandle_t btLock;
static esp_bd_addr_t btTarget;
static volatile bool btHasTarget = false, btStateDirty = false;
static String btName;  // connected device name (guarded by btLock)

static bool onBtDeviceFound(const char* name, esp_bd_addr_t addr, int rssi) {
  if (!name || !*name) return false;
  xSemaphoreTake(btLock, portMAX_DELAY);
  auto it = std::find_if(btFound.begin(), btFound.end(),
                         [&](const BtDevice& d) { return !memcmp(d.addr, addr, ESP_BD_ADDR_LEN); });
  if (it == btFound.end() && btFound.size() < 7) {
    BtDevice d{String(name), {}, rssi};
    memcpy(d.addr, addr, ESP_BD_ADDR_LEN);
    btFound.push_back(d);
  } else if (it != btFound.end()) {
    it->rssi = rssi;
  }
  bool connect = btHasTarget ? !memcmp(addr, btTarget, ESP_BD_ADDR_LEN) : a2dp.isSaved(addr);
  if (connect) btName = name;
  xSemaphoreGive(btLock);
  connectDirty = true;
  return connect;  // true tells the library to connect to this device
}

static void onBtState(esp_a2d_connection_state_t state, void*) {
  bool up = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
  if (up != btConnected) {
    btConnected = up;
    ringFlushReq = true;
    btStateDirty = true;
  }
}

#else
// ---------- WiFi: clock sync + web upload/remote ----------
static WebServer web(80);
static bool webUp = false;
static volatile bool ntpSynced = false;
static File webFile;
static String webPath;

static String wifiStatusText() {
  if (!prefs.isKey("ssid")) return "Not set up. Run: upload.py --wifi <name>";
  if (WiFi.status() == WL_CONNECTED) return "Connected to " + prefs.getString("ssid");
  return "Connecting to " + prefs.getString("ssid") + "...";
}

static String hostName();
static void wifiStart() {
  if (!prefs.isKey("ssid")) return;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName().c_str());
  WiFi.begin(prefs.getString("ssid").c_str(), prefs.getString("pass").c_str());
  sntp_set_time_sync_notification_cb([](timeval*) { ntpSynced = true; });
  configTzTime(prefs.getString("tz", DEFAULT_TZ).c_str(), "pool.ntp.org", "time.google.com");
}

// <owner>-tunes.local in a browser (Macs, iPhones, most others), as well as the IP address.
static void startMdns() {
  static bool started = false;
  if (started) return;
  started = MDNS.begin(hostName().c_str());
  if (started) MDNS.addService("http", "tcp", 80);
}

static uint32_t wifiUpAt = 0;  // when WiFi connected (podcast sync waits ~30 s after)
static void podcastLoop();
static void wifiLoop() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !webUp) {
    web.begin();
    webUp = true;
    Serial.printf("IP %s\n", WiFi.localIP().toString().c_str());
    startMdns();
    if (screen == Screen::Shelves) dirtyAll = true;  // its WiFi mark
    connectDirty = true;
  } else if (!up && webUp) {
    web.stop();
    webUp = false;
    if (screen == Screen::Shelves) dirtyAll = true;
    connectDirty = true;
  }
  if (webUp) { web.handleClient(); podcastLoop(); }
  else wifiUpAt = 0;
  if (ntpSynced) {  // the RTC holds local time; the BT build reads it directly
    ntpSynced = false;
    time_t now = time(nullptr);
    tm local;
    localtime_r(&now, &local);
    M5.Rtc.setDateTime(&local);
  }
}

static String jsonEscape(const String& in) {
  String o;
  for (char c : in) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

#include "tt_web_page.h"  // INDEX_HTML: web/index.html, made by flash.sh

// Folder name from user input: no slashes or characters FAT can't store.
static String safeName(String s) {
  String o;
  for (char c : s) o += strchr("/\\:*?\"<>|", c) ? '_' : c;
  o.trim();
  return o;
}

// New albums go in /music if the card uses that layout, else the card root.
static String libraryRoot() { return SD.exists("/music") ? "/music" : "/"; }

// Only paths inside the card, no "..".
static bool okPath(const String& p) { return p.startsWith("/") && p.indexOf("..") < 0 && p.length() > 1; }

// ---------- podcasts (WiFi mode) ----------
// Shows are listed in /podcasts/feeds.txt, one "keep<TAB>url<TAB>name" per line.
// In WiFi mode the Core2 checks them ~30 s after connecting, every 6 hours, and on
// "Sync now". It downloads the newest `keep` MP3 episodes (and the show's artwork)
// into /podcasts/<Show>/ and deletes older ones, so in Bluetooth mode each show is
// just an album, newest episode first. Work happens a few KB per loop() pass, so
// music and the web page keep running (opening each connection still blocks 1-3 s).
// HTTPS certificates aren't checked: these are public podcast files.
struct Episode { String title, url, date, path; };
enum class SyncState { Idle, Feed, Download };

static std::vector<Feed> feeds;
static constexpr const char* FEEDS_FILE = "/podcasts/feeds.txt";
static SyncState syncState = SyncState::Idle;
static int syncFeed = -1;
static std::vector<Episode> syncQueue;  // what to download for the current show
static size_t syncIdx = 0;
static String syncStatus = "Not synced yet", syncError;
static uint32_t syncLastRun = 0, syncLastData = 0;
static bool syncChanged = false, syncRequested = false, syncEverRan = false;
static NetworkClientSecure* syncTls = nullptr;
static NetworkClient* syncPlain = nullptr;
static bool rescanPending = false;  // a sync changed files; rescan once the music is paused
static HTTPClient syncHttp;
static File syncFile;
static int32_t syncTotal = 0, syncGot = 0;

static struct {
  String tag, text, chanTitle, chanImage;
  bool inTag = false, inItem = false, inImage = false, done = false;
  char prev1 = 0, prev2 = 0;  // last two characters of the current tag (spots "]]>" past the buffer cap)
  Episode cur;
  std::vector<Episode> items;
  int want = 5;
} rss;

static String showFolder(const Feed& f, int i) {
  String n = safeName(asciiFold(f.name.length() ? f.name : "Podcast " + String(i + 1)));
  if (n.length() > 50) n.remove(50);
  n.trim();
  return "/podcasts/" + n;
}

static void loadFeeds() {
  feeds.clear();
  File f = SD.open(FEEDS_FILE);
  while (f && f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int a = line.indexOf('\t'), b = a < 0 ? -1 : line.indexOf('\t', a + 1);
    if (a < 0) continue;
    feeds.push_back({line.substring(a + 1, b < 0 ? line.length() : b), b < 0 ? "" : line.substring(b + 1),
                     constrain((int)line.substring(0, a).toInt(), 1, 20)});
  }
  if (f) f.close();
}

static void saveFeeds() {
  SD.mkdir("/podcasts");
  File f = SD.open(FEEDS_FILE, FILE_WRITE);
  if (!f) return;
  for (auto& fd : feeds) f.printf("%d\t%s\t%s\n", fd.keep, fd.url.c_str(), fd.name.c_str());
  f.close();
}

// &amp; &#8217; ... -> UTF-8 (asciiFold() later turns curly quotes into ASCII)
static String decodeEntities(String s) {
  String o;
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] != '&') { o += s[i]; continue; }
    int semi = s.indexOf(';', i);
    if (semi < 0 || semi - i > 8) { o += '&'; continue; }
    String e = s.substring(i + 1, semi);
    uint32_t cp = 0;
    if (e == "amp") cp = '&'; else if (e == "lt") cp = '<'; else if (e == "gt") cp = '>';
    else if (e == "quot") cp = '"'; else if (e == "apos") cp = '\'';
    else if (e.startsWith("#x")) cp = strtoul(e.c_str() + 2, nullptr, 16);
    else if (e.startsWith("#")) cp = e.substring(1).toInt();
    if (!cp) { o += '&'; continue; }
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    i = semi;
  }
  o.trim();
  return o;
}

static String xmlAttr(const String& tag, const char* name) {
  for (char q : {'"', '\''}) {
    String key = String(name) + "=" + q;
    int a = tag.indexOf(key);
    if (a < 0) continue;
    a += key.length();
    int b = tag.indexOf(q, a);
    if (b > a) return decodeEntities(tag.substring(a, b));
  }
  return "";
}

// "Tue, 21 Oct 2025 07:00:00 GMT" -> "2025-10-21" (sorts by date as a filename prefix)
static String isoDate(const String& rfc) {
  static const char* M = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int day = 0, year = 0, mon = 0;
  char m[4] = {0};
  const char* p = strchr(rfc.c_str(), ',');
  if (sscanf(p ? p + 1 : rfc.c_str(), "%d %3s %d", &day, m, &year) != 3) return "";
  const char* f = strstr(M, m);
  if (!f) return "";
  mon = (f - M) / 3 + 1;
  char b[12];
  snprintf(b, sizeof b, "%04d-%02d-%02d", year, mon, day);
  return b;
}

static void rssTag(String tag) {
  if (tag.startsWith("![CDATA[")) { rss.text += tag.substring(8, tag.endsWith("]]") ? tag.length() - 2 : tag.length()); return; }
  bool closing = tag.startsWith("/");
  String name = closing ? tag.substring(1) : tag;
  int sp = name.indexOf(' ');
  if (sp >= 0) name.remove(sp);
  if (name.endsWith("/")) name.remove(name.length() - 1);
  if (!closing) {
    if (name == "item") { rss.inItem = true; rss.cur = Episode(); }
    else if (name == "image") rss.inImage = true;
    else if (name == "enclosure" && rss.inItem) {
      String url = xmlAttr(tag, "url"), type = xmlAttr(tag, "type");
      String lurl = lowerStr(url);
      if (type.indexOf("mpeg") >= 0 || type.indexOf("mp3") >= 0 || lurl.indexOf(".mp3") >= 0) rss.cur.url = url;
    } else if (name == "itunes:image" && !rss.inItem && !rss.chanImage.length()) rss.chanImage = xmlAttr(tag, "href");
    rss.text = "";
    return;
  }
  String text = decodeEntities(rss.text);
  rss.text = "";
  if (name == "title") { if (rss.inItem) rss.cur.title = text; else if (!rss.inImage && !rss.chanTitle.length()) rss.chanTitle = text; }
  else if (name == "pubDate" && rss.inItem) rss.cur.date = isoDate(text);
  else if (name == "url" && rss.inImage && !rss.chanImage.length()) rss.chanImage = text;
  else if (name == "image") rss.inImage = false;
  else if (name == "item") {
    rss.inItem = false;
    if (rss.cur.url.length()) rss.items.push_back(rss.cur);
    if ((int)rss.items.size() >= rss.want) rss.done = true;  // feeds list newest first
  }
}

static void rssChar(char c) {
  if (rss.done) return;
  if (rss.inTag) {
    // A CDATA block (often a long HTML description) ends at "]]>", which may come long
    // after the buffer stops growing, so watch the last two characters separately.
    bool cdata = rss.tag.startsWith("![CDATA[");
    if (c == '>' && (!cdata || (rss.prev1 == ']' && rss.prev2 == ']'))) { rssTag(rss.tag); rss.tag = ""; rss.inTag = false; }
    else if (rss.tag.length() < 1500) rss.tag += c;
    rss.prev2 = rss.prev1;
    rss.prev1 = c;
    return;
  }
  if (c == '<') { rss.inTag = true; rss.tag = ""; rss.prev1 = rss.prev2 = 0; return; }
  if (rss.text.length() < 400) rss.text += c;
}

static void syncClose() {
  syncHttp.end();
  delete syncTls; syncTls = nullptr;
  delete syncPlain; syncPlain = nullptr;
}

// Opens a GET; returns the body stream or nullptr (and sets syncError).
static Stream* syncOpen(const String& url) {
  syncClose();
  bool tls = url.startsWith("https://");
  if (tls) { syncTls = new NetworkClientSecure(); syncTls->setInsecure(); }
  else syncPlain = new NetworkClient();
  syncHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  syncHttp.setRedirectLimit(8);
  syncHttp.useHTTP10(true);  // no chunked encoding to unpick
  syncHttp.setTimeout(15000);
  syncHttp.setUserAgent("TinyTunes/1.0 (podcast player)");
  if (!syncHttp.begin(tls ? (NetworkClient&)*syncTls : *syncPlain, url)) { syncError = "Bad link"; return nullptr; }
  int code = syncHttp.GET();
  if (code != 200) { syncError = "Server said " + String(code); syncClose(); return nullptr; }
  syncLastData = millis();
  return syncHttp.getStreamPtr();
}

// Apple Podcasts page link -> the show's RSS feed, via Apple's lookup API.
static String resolveFeedUrl(String url) {
  url.trim();
  if (url.indexOf("podcasts.apple.com") < 0) return url;
  int id = url.indexOf("/id");
  if (id < 0) return url;
  String digits;
  for (int i = id + 3; i < (int)url.length() && isdigit(url[i]); i++) digits += url[i];
  NetworkClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  if (!http.begin(tls, "https://itunes.apple.com/lookup?entity=podcast&id=" + digits) || http.GET() != 200) return url;
  String body = http.getString();
  http.end();
  int a = body.indexOf("\"feedUrl\":\"");
  if (a < 0) return url;
  a += 11;
  String feed = body.substring(a, body.indexOf('"', a));
  feed.replace("\\/", "/");
  return feed;
}

static void beginFeed();
static void finishSync() {
  syncClose();
  syncState = SyncState::Idle;
  syncLastRun = millis();
  syncEverRan = true;
  syncStatus = syncError.length() ? "Synced with problems" : "Up to date";
  if (syncChanged) rescanPending = true;  // picked up on the next loop (music keeps playing)
  syncChanged = false;
  if (nightSync) nightSyncDone();
}

static void nextFeed() {
  syncClose();
  if (++syncFeed < (int)feeds.size()) beginFeed();
  else finishSync();
}

static void beginFeed() {
  Feed& f = feeds[syncFeed];
  syncStatus = "Checking " + (f.name.length() ? f.name : f.url);
  rss.tag = rss.text = rss.chanTitle = rss.chanImage = "";
  rss.inTag = rss.inItem = rss.inImage = rss.done = false;
  rss.items.clear();
  rss.want = f.keep;
  if (!syncOpen(f.url)) { nextFeed(); return; }
  syncState = SyncState::Feed;
}

static void startDownload();
// Feed read: decide what to fetch and what to delete for this show.
static void finishFeed() {
  syncClose();
  Feed& f = feeds[syncFeed];
  if (!f.name.length() && rss.chanTitle.length()) { f.name = rss.chanTitle; saveFeeds(); }
  if (rss.items.empty()) { syncError = "No MP3 episodes in " + (f.name.length() ? f.name : f.url); nextFeed(); return; }
  String dir = showFolder(f, syncFeed);
  SD.mkdir("/podcasts");
  SD.mkdir(dir);
  syncQueue.clear();
  std::vector<String> keepNames;
  bool hasCover = SD.exists(dir + "/cover.jpg") || SD.exists(dir + "/cover.png");
  if (!hasCover && rss.chanImage.length())
    syncQueue.push_back({"cover", rss.chanImage, "", dir + (lowerStr(rss.chanImage).indexOf(".png") >= 0 ? "/cover.png" : "/cover.jpg")});
  for (size_t i = 0; i < rss.items.size(); i++) {
    Episode e = rss.items[i];
    String t = safeName(asciiFold(e.title.length() ? e.title : "Episode"));
    if (t.length() > 60) t.remove(60);
    t.trim();
    String name = (e.date.length() ? e.date : "0000-00-" + String(10 + i)) + " " + t + ".mp3";
    keepNames.push_back(name);
    e.path = dir + "/" + name;
    if (!SD.exists(e.path)) syncQueue.push_back(e);
  }
  // older episodes and half-finished downloads go
  File d = SD.open(dir);
  std::vector<String> drop;
  while (d) {
    File g = d.openNextFile();
    if (!g) break;
    String n = g.name();
    bool isEp = n.endsWith(".mp3") || n.endsWith(".part");
    if (isEp && std::find(keepNames.begin(), keepNames.end(), n) == keepNames.end()) drop.push_back(dir + "/" + n);
    g.close();
  }
  if (d) d.close();
  for (auto& p : drop)
    if (!touchesPlaying(p)) { SD.remove(p); syncChanged = true; }  // the playing one goes next time
  // forget resume spots for episodes that are gone
  auto resume = loadResume(dir);
  size_t before = resume.size();
  resume.erase(std::remove_if(resume.begin(), resume.end(),
                              [&](const ResumeEntry& e) { return !SD.exists(dir + "/" + e.file); }), resume.end());
  if (resume.size() != before) saveResume(dir, resume);
  syncIdx = 0;
  if (syncQueue.empty()) nextFeed();
  else startDownload();
}

static void startDownload() {
  while (syncIdx < syncQueue.size()) {
    Episode& e = syncQueue[syncIdx];
    syncStatus = "Downloading " + (feeds[syncFeed].name.length() ? feeds[syncFeed].name + ": " : String()) + e.title;
    if (syncOpen(e.url)) {
      syncTotal = syncHttp.getSize();
      syncGot = 0;
      syncFile = SD.open(e.path + ".part", FILE_WRITE);
      if (syncFile) { syncState = SyncState::Download; return; }
      syncError = "Couldn't write to the card";
    }
    syncIdx++;
  }
  nextFeed();
}

static void finishDownload(bool ok) {
  Episode& e = syncQueue[syncIdx];
  syncFile.close();
  syncClose();
  if (ok) { SD.remove(e.path); SD.rename(e.path + ".part", e.path); syncChanged = true; }
  else { SD.remove(e.path + ".part"); syncError = "Download failed: " + e.title; }
  syncIdx++;
  startDownload();
}

static void requestSync() { syncRequested = true; }

// The web page's Stop: ends a sync in the middle. Episodes already downloaded stay; a
// half-downloaded one is dropped. It doesn't start again by itself until the next WiFi
// connect, the nightly sync or Sync now.
static void stopSync() {
  if (syncState == SyncState::Idle) return;
  if (syncState == SyncState::Download && syncIdx < (int)syncQueue.size()) {
    syncFile.close();
    SD.remove(syncQueue[syncIdx].path + ".part");
  }
  syncError = "";
  finishSync();
  syncStatus = "Stopped";
}

// Called from wifiLoop() while connected.
static void podcastLoop() {
  uint32_t now = millis();
  if (!wifiUpAt) wifiUpAt = now;
  if (syncState == SyncState::Idle) {
    if (rescanPending) { rescanPending = false; rescanKeepingPlace(); }
    bool due = syncRequested || (!syncEverRan && now - wifiUpAt > (nightSync ? 2000 : 30000));
    if (!due || feeds.empty()) { syncRequested = false; return; }
    syncRequested = false;
    syncError = "";
    syncChanged = false;
    syncFeed = 0;
    beginFeed();
    return;
  }
  Stream* st = syncHttp.getStreamPtr();  // null once the server has closed and the data is drained
  static uint8_t buf[4096];
  int avail = st ? st->available() : 0;
  if (avail > 0) {
    int n = st->readBytes(buf, std::min<int>(avail, sizeof buf));
    syncLastData = now;
    if (syncState == SyncState::Feed) {
      for (int i = 0; i < n && !rss.done; i++) rssChar(buf[i]);
      if (rss.done) finishFeed();
    } else {
      if (syncFile.write(buf, n) != (size_t)n) { syncError = "Card full?"; finishDownload(false); return; }
      syncGot += n;
      if (syncTotal > 0 && syncGot >= syncTotal) finishDownload(true);
    }
    return;
  }
  bool ended = !st || !syncHttp.connected();
  if (ended || now - syncLastData > 20000) {  // server finished (or went quiet)
    if (syncState == SyncState::Feed) finishFeed();
    else finishDownload(ended && syncGot > 0 && (syncTotal <= 0 || syncGot >= syncTotal));
  }
}

static String podcastJson() {
  auto num = [](int v) { return String(v); };
  String j = "{\"state\":\"" + String(syncState == SyncState::Idle ? "idle" : "busy") + "\",\"status\":\"" + jsonEscape(syncStatus) +
             "\",\"error\":\"" + jsonEscape(syncError) + "\",\"pct\":" +
             num(syncState == SyncState::Download && syncTotal > 0 ? (int)(100LL * syncGot / syncTotal) : -1) +
             ",\"ago\":" + num(syncEverRan ? (millis() - syncLastRun) / 60000 : -1) + ",\"feeds\":[";
  for (size_t i = 0; i < feeds.size(); i++) {
    audioPump();
    String dir = showFolder(feeds[i], i);
    j += (i ? ",{" : "{");
    j += "\"url\":\"" + jsonEscape(feeds[i].url) + "\",\"name\":\"" + jsonEscape(feeds[i].name) + "\",\"keep\":" + num(feeds[i].keep) +
         ",\"folder\":\"" + jsonEscape(dir) + "\",\"episodes\":[";
    std::vector<std::pair<String, uint32_t>> eps;
    File d = SD.open(dir);
    while (d) {
      File g = d.openNextFile();
      if (!g) break;
      if (isMp3(g.name())) eps.push_back({g.name(), (uint32_t)g.size()});
      g.close();
    }
    if (d) d.close();
    std::sort(eps.begin(), eps.end(), [](auto& a, auto& b) { return a.first > b.first; });  // newest first
    for (size_t k = 0; k < eps.size(); k++)
      j += (k ? ",{" : "{") + String("\"file\":\"") + jsonEscape(eps[k].first) + "\",\"size\":" + String(eps[k].second) + "}";
    j += "]}";
  }
  return j + "]}";
}

static void setupPodcastWeb() {
  loadFeeds();
  web.on("/podcasts", HTTP_GET, [] { web.send(200, "application/json", podcastJson()); });
  web.on("/podcast/add", HTTP_POST, [] {
    String url = resolveFeedUrl(web.arg("url"));
    if (!url.startsWith("http")) { web.send(400, "text/plain", "That doesn't look like a link"); return; }
    for (auto& f : feeds) if (f.url == url) { web.send(409, "text/plain", "Already added"); return; }
    feeds.push_back({url, "", constrain((int)web.arg("keep").toInt(), 1, 20)});
    saveFeeds();
    requestSync();
    web.send(200, "text/plain", "OK");
  });
  web.on("/podcast/keep", HTTP_POST, [] {
    int i = web.arg("i").toInt();
    if (i < 0 || i >= (int)feeds.size()) { web.send(400, "text/plain", "bad index"); return; }
    feeds[i].keep = constrain((int)web.arg("keep").toInt(), 1, 20);
    saveFeeds();
    requestSync();
    web.send(200, "text/plain", "OK");
  });
  web.on("/podcast/remove", HTTP_POST, [] {
    int i = web.arg("i").toInt();
    if (i < 0 || i >= (int)feeds.size() || syncState != SyncState::Idle) { web.send(409, "text/plain", "Try again when the sync is done"); return; }
    if (web.arg("files") == "1") { if (touchesPlaying(showFolder(feeds[i], i))) stopPlayback(); removePath(showFolder(feeds[i], i)); }
    feeds.erase(feeds.begin() + i);
    saveFeeds();
    rescanKeepingPlace();
    web.send(200, "text/plain", "OK");
  });
  web.on("/podcast/sync", HTTP_POST, [] { requestSync(); web.send(200, "text/plain", "OK"); });
  web.on("/podcast/stop", HTTP_POST, [] { stopSync(); web.send(200, "text/plain", "OK"); });
}

static void setupWeb() {
  setupPodcastWeb();
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html", INDEX_HTML); });
  web.on("/state", HTTP_GET, [] {
    String albumName = playAlbum >= 0 ? albums[playAlbum].name : "";
    String j = "{\"current\":" + String(current) + ",\"paused\":" + (paused ? "true" : "false") +
               ",\"volume\":" + String(volLevel * 10) + ",\"version\":\"" TT_VERSION "\",\"owner\":\"" + jsonEscape(owner) +
               "\",\"host\":\"" + hostName() + "\",\"album\":\"" + jsonEscape(albumName) +
               "\",\"tracks\":[";
    for (size_t i = 0; i < tracks.size(); i++) {
      if (i) j += ',';
      j += "\"" + jsonEscape(trackTitle(tracks[i], playAlbum)) + "\"";
    }
    web.send(200, "application/json", j + "]}");
  });
  // Everything the organize/cleanup UI needs: albums, songs with display titles, cover status, stray files.
  web.on("/library", HTTP_GET, [] {  // streamed album by album, so a big library doesn't fill RAM
    static const char* KIND[] = {"music", "podcasts", "books", "other"};
    web.setContentLength(CONTENT_LENGTH_UNKNOWN);
    web.send(200, "application/json", "");
    String j = "{\"root\":\"" + jsonEscape(libraryRoot()) + "\",\"playing\":" + String(playAlbum) + ",\"shelves\":[";
    for (size_t i = 0; i < shelves.size(); i++)
      j += String(i ? ",{" : "{") + "\"name\":\"" + jsonEscape(shelves[i].name) + "\",\"path\":\"" + jsonEscape(shelves[i].path) +
           "\",\"kind\":\"" + KIND[(int)shelves[i].kind] + "\"}";
    j += "],\"albums\":[";
    web.sendContent(j);
    for (size_t a = 0; a < albums.size(); a++) {
      audioPump();
      std::vector<String> list = (int)a == playAlbum ? tracks : listTracks(a);
      const String& cf = albums[a].coverFile;
      uint32_t coverSize = cf.length() ? fileSize(joinPath(albums[a].path, cf.c_str())) : 0;
      j = a ? ",{" : "{";
      j += "\"name\":\"" + jsonEscape(albums[a].name) + "\",\"folder\":\"" + jsonEscape(albums[a].path) +
           "\",\"cover\":\"" + jsonEscape(cf) + "\",\"art\":" + (albums[a].artFile.length() && !cf.length() ? "true" : "false") +
           ",\"coverSize\":" + String(coverSize) + ",\"shelf\":" + String(albums[a].shelf) +
           ",\"tracks\":[";
      for (size_t i = 0; i < list.size(); i++) {
        String fname = list[i].substring(list[i].lastIndexOf('/') + 1);
        j += (i ? ",{" : "{");
        j += "\"file\":\"" + jsonEscape(fname) + "\",\"title\":\"" + jsonEscape(trackTitle(list[i], a)) + "\"}";
      }
      j += "],\"other\":[";
      File dir = SD.open(albums[a].path);
      bool first = true;
      while (File f = dir.openNextFile()) {
        const char* n = f.name();
        if (!f.isDirectory() && !isMp3(n) && albums[a].coverFile != n && strcmp(n, COVER_CACHE) && strcmp(n, ".resume") &&
            !(albums[a].path == "/" && n[0] == '.')) {
          j += (first ? "\"" : ",\"") + jsonEscape(n) + "\"";
          first = false;
        }
        f.close();
      }
      dir.close();
      j += "]}";
      web.sendContent(j);
    }
    web.sendContent("]}");
    web.sendContent("");  // end of the chunked reply
  });
  // Raw file download; ?n= limits it to the first n bytes (for reading embedded cover art).
  web.on("/file", HTTP_GET, [] {
    String path = web.arg("path");
    File f = okPath(path) ? SD.open(path) : File();
    if (!f || f.isDirectory()) { web.send(404, "text/plain", "not found"); return; }
    size_t n = f.size();
    if (web.hasArg("n")) n = std::min<size_t>(n, web.arg("n").toInt());
    String lower = lowerStr(path);
    const char* type = lower.endsWith(".jpg") || lower.endsWith(".jpeg") ? "image/jpeg"
                     : lower.endsWith(".png") ? "image/png" : lower.endsWith(".webp") ? "image/webp"
                     : lower.endsWith(".mp3") ? "audio/mpeg" : "application/octet-stream";
    web.setContentLength(n);
    web.sendHeader("Cache-Control", "max-age=3600");
    web.send(200, type, "");
    static uint8_t buf[4096];
    for (size_t sent = 0; sent < n;) {
      size_t r = f.read(buf, std::min(sizeof buf, n - sent));
      if (!r) break;
      web.client().write(buf, r);
      sent += r;
      audioPump();
    }
    f.close();
  });
  // An album's cover as a small BMP made from the decoded 160x160 copy the device keeps
  // (.cover.rgb565), instead of the original image (which can be megabytes).
  web.on("/cover", HTTP_GET, [] {
    int a = web.arg("i").toInt();
    File f = a >= 0 && a < (int)albums.size() ? SD.open(joinPath(albums[a].path, COVER_CACHE)) : File();
    CoverCacheHeader h;
    if (!f || f.read((uint8_t*)&h, sizeof h) != sizeof h || memcmp(h.magic, "C2C1", 4)) {
      if (f) f.close();
      web.send(404, "text/plain", "no cover yet");
      return;
    }
    const uint32_t px = COVER_MASTER, img = px * px * 2, off = 14 + 40 + 12;
    uint8_t hdr[off] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { for (int k = 0; k < 4; k++) hdr[at + k] = v >> (8 * k); };
    put32(2, off + img); put32(10, off);
    put32(14, 40); put32(18, px); put32(22, (uint32_t)-(int32_t)px);  // negative height: rows top-down
    hdr[26] = 1; hdr[28] = 16; put32(30, 3); put32(34, img);          // 16 bpp, BI_BITFIELDS
    put32(54, 0xF800); put32(58, 0x07E0); put32(62, 0x001F);          // RGB565 masks
    web.setContentLength(off + img);
    web.sendHeader("Cache-Control", "max-age=3600");
    web.send(200, "image/bmp", "");
    web.client().write(hdr, off);
    static uint8_t buf[4096];
    for (uint32_t sent = 0; sent < img;) {
      size_t r = f.read(buf, std::min<uint32_t>(sizeof buf, img - sent));
      if (!r) break;
      for (size_t k = 0; k + 1 < r; k += 2) std::swap(buf[k], buf[k + 1]);  // sprite is big-endian 565
      web.client().write(buf, r);
      sent += r;
      audioPump();
    }
    f.close();
  });
  web.on("/mv", HTTP_POST, [] {
    String from = web.arg("from"), to = web.arg("to");
    if (!okPath(from) || !okPath(to) || !SD.exists(from)) { web.send(400, "text/plain", "bad path"); return; }
    if (SD.exists(to)) { web.send(409, "text/plain", "something with that name already exists"); return; }
    if (touchesPlaying(from)) stopPlayback();
    int slash = to.lastIndexOf('/');
    if (slash > 0) SD.mkdir(to.substring(0, slash));
    if (SD.rename(from, to)) web.send(200, "text/plain", "OK");
    else web.send(500, "text/plain", "rename failed");
  });
  web.on("/rm", HTTP_POST, [] {
    String path = web.arg("path");
    if (!okPath(path) || path == "/music") { web.send(400, "text/plain", "bad path"); return; }
    if (touchesPlaying(path)) stopPlayback();
    if (removePath(path)) web.send(200, "text/plain", "OK");
    else web.send(500, "text/plain", "delete failed");
  });
  web.on("/mkdir", HTTP_POST, [] {
    String path = web.arg("path");
    if (!okPath(path)) { web.send(400, "text/plain", "bad path"); return; }
    if (SD.exists(path) || SD.mkdir(path)) web.send(200, "text/plain", "OK");
    else web.send(500, "text/plain", "mkdir failed");
  });
  web.on("/cmd", HTTP_POST, [] {
    String c = web.arg("c");
    web.send(200, "text/plain", "OK");
    if (c == "prev") playTrack(current - 1);
    else if (c == "next") playTrack(current + 1);
    else if (c == "play") togglePause();
    else if (c == "volup") setVolLevel(volLevel + 1);
    else if (c == "voldown") setVolLevel(volLevel - 1);
    else if (c == "track") playTrack(web.arg("i").toInt());
    else if (c == "album") playAlbumTrack(web.arg("i").toInt(), 0);
    else if (c == "btmode") switchMode();
  });
  web.on("/owner", HTTP_POST, [] {
    setOwner(web.arg("name"));
    web.send(200, "text/plain", "OK");
  });
  web.on("/rescan", HTTP_POST, [] {
    rescanKeepingPlace();
    web.send(200, "text/plain", "OK");
  });
  // ?dir=/full/folder (web page) or ?album=Name (upload.py; relative to the library root)
  web.on("/upload", HTTP_POST,
         [] { web.send(webPath.length() ? 200 : 500, "text/plain", webPath.length() ? "OK" : "write failed"); },
         [] {
           HTTPUpload& up = web.upload();
           if (up.status == UPLOAD_FILE_START) {  // music keeps playing; /rescan picks the new files up
             String dir = web.hasArg("dir") ? web.arg("dir") : web.arg("album").length() ? joinPath(libraryRoot(), safeName(web.arg("album")).c_str()) : libraryRoot();
             if (dir != "/" && !okPath(dir)) { webPath = ""; return; }
             if (dir != "/") SD.mkdir(dir);
             webPath = joinPath(dir, safeName(up.filename.substring(up.filename.lastIndexOf('/') + 1)).c_str());
             webFile = SD.open(webPath, FILE_WRITE);
             if (!webFile) webPath = "";
           } else if (up.status == UPLOAD_FILE_WRITE && webFile) {
             audioPump();
             if (webFile.write(up.buf, up.currentSize) != up.currentSize) {
               webFile.close();
               SD.remove(webPath);
               webPath = "";
             }
           } else if (up.status == UPLOAD_FILE_END && webFile) {
             webFile.close();
           } else if (up.status == UPLOAD_FILE_ABORTED && webFile) {
             webFile.close();
             SD.remove(webPath);
             webPath = "";
           }
         });
}
#endif
