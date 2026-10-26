// Tiny Tunes — a kids' audio player (music, podcasts, audiobooks) for the M5Stack Core2.
//
// Put .mp3 files on a FAT32 microSD card: album folders, optionally inside shelf
// folders (/music, /podcasts, /books, or any other).
//
// This sketch builds two firmwares that live side by side in the two app slots:
//   Bluetooth mode (default build): plays to paired BT headphones/speakers, or
//     the built-in speaker when nothing is connected.
//   WiFi mode (-DCORE2_WIFI=1): built-in speaker, NTP clock sync, podcast downloads,
//     and a web page for uploading music and remote control.
// The precompiled ESP32 core can't fit BT Classic and WiFi in IRAM together, so
// switching modes reboots into the other slot (~2 s). SD card and settings are shared.
// Build/flash both with ./flash.sh.
//
// Kid-friendly controls:
//   The middle touch button below the screen: Home (the left and right ones do nothing alone)
//   Home: shelves (Podcasts, Music, Books) as a list -> albums as a carousel -> songs
//   The top bar: back and the shelf's name; top right, the player (green while playing)
//   Player: the title, the artist, quieter / play / louder (5 volume steps, shown under
//     play), and a progress bar to drag
//   Albums page sideways; lists scroll up/down; on the player, swipe for next / previous song
//   Snackbars at the bottom: low battery, charging, headphones on/off
//   Hold two of the touch buttons for 3 s: grown-ups screen
//   Power button: tap = screen off / on (the sound keeps playing), hold = switch off
//   Screen dims after 2 min untouched; powers off when paused on battery (15/30 min/never).
//
// Files can also be uploaded over USB with upload.py (see serial protocol below).

#ifndef CORE2_WIFI
#define CORE2_WIFI 0
#endif

// Build version, written by flash.sh: VERSION file + build number + commit (e.g. 0.1.0+14.a1b2c3d).
#if __has_include("version.h")
#include "version.h"
#endif
#ifndef TT_VERSION
#define TT_VERSION "dev"
#endif

#include <SD.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <time.h>
#include <M5Unified.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include <atomic>
#include <vector>
#include <algorithm>
#if CORE2_WIFI
#include <WiFi.h>
#include <WebServer.h>
#include <esp_sntp.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ESPmDNS.h>
#else
#include <BluetoothA2DPSource.h>
#endif

// Declared up here so Arduino's auto-generated prototypes can see it.
enum class Screen { Player, Shelves, Albums, Tracks, Grownups };
enum class Snack : uint8_t { None, LowBattery, Charging, Full, Headphones, Speaker };  // see "snackbars"
struct RoundButton { int x, r; };
// What an MP3's ID3v2 tag says (see "tags").
struct Id3Info {
  String title, artist, album, albumArtist;
  int track = 0, disc = 0;          // TRCK / TPOS ("3/12" -> 3); 0 = not given
  uint32_t picOff = 0, picLen = 0;  // the embedded picture (APIC) inside the file
  bool picPng = false;
  uint32_t audioStart = 0;          // where the audio begins, after the tag
};
struct TrackTag { const char* file; const char* title; uint16_t track, disc; };
struct CoverEntry { String path; int size; bool dim; M5Canvas* canvas; uint32_t used; };  // see "album covers"  // one song's tag (text in PSRAM, see arenaDup)
struct Album {
  String name, path, coverFile;  // coverFile "" = none
  int8_t artistPrefixed = -1;    // -1 = not yet known
  int shelf = 0;                 // index into shelves
  int newCount = 0;              // podcast episodes nobody has started
  uint16_t tint = 0;             // card color picked from the cover (0 = not yet known)
  String artFile;                // no cover image: a song with a picture in its tag ("" = none)
  String albumArtist;            // from the tags ("" = not known)
  bool tagsRead = false;         // `tags` filled (once per rescan; see loadTags)
  std::vector<TrackTag> tags;
};
// Listen's top level. Each top-level folder of albums is a shelf: /music, /podcasts and
// /books have their own look; any other folder of albums gets a generic one.
enum class ShelfKind : uint8_t { Music, Podcasts, Books, Other };
struct Shelf { String name, path; ShelfKind kind; int albums = 0, newCount = 0; };
struct ResumeEntry { String file; uint32_t pos; bool done; };  // books/podcasts: see "resume"

// Normal landscape: the three touch buttons sit below the screen. Only the middle one
// does anything by itself (Home); two held together open the grown-ups screen.
static constexpr int ROTATION = 1;  // 3 = upside down

static constexpr int SD_CS = 4;
static constexpr uint8_t SPK_CH = 0;
static constexpr const char* DEFAULT_TZ = "PST8PDT,M3.2.0,M11.1.0";  // US Pacific

static Preferences prefs;
// Flags shared with the BT task. Plain volatile bools: 1-byte std::atomic pulls
// libatomic helpers into IRAM, which the BT build has no room for.
static volatile bool btConnected = false;  // always false in the WiFi build
static volatile bool ringFlushReq = false;

#if !CORE2_WIFI
// The core's libc puts gmtime_r/localtime_r in IRAM (PSRAM cache workaround),
// and the BT build overflows IRAM by ~1 KB. Only FAT file timestamps use them
// here, and BT mode has no timezone, so UTC versions in flash replace them.
extern "C" tm* gmtime_r(const time_t* tp, tm* r) {
  int64_t t = *tp, days = t / 86400, secs = t % 86400;
  if (secs < 0) { secs += 86400; days--; }
  r->tm_hour = secs / 3600;
  r->tm_min = secs / 60 % 60;
  r->tm_sec = secs % 60;
  r->tm_wday = (int)((days % 7 + 11) % 7);  // 1970-01-01 was a Thursday
  // civil-from-days (Howard Hinnant)
  days += 719468;
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  int doe = days - era * 146097;
  int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int mp = (5 * doy + 2) / 153;
  int mon = mp < 10 ? mp + 3 : mp - 9;
  int year = yoe + era * 400 + (mon <= 2);
  r->tm_mday = doy - (153 * mp + 2) / 5 + 1;
  r->tm_mon = mon - 1;
  r->tm_year = year - 1900;
  static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  r->tm_yday = cum[r->tm_mon] + r->tm_mday - 1 + (leap && r->tm_mon > 1);
  r->tm_isdst = 0;
  return r;
}
extern "C" tm* localtime_r(const time_t* tp, tm* r) { return gmtime_r(tp, r); }

static constexpr int BT_RATE = 44100;  // A2DP SBC is fixed at 44.1 kHz stereo
// The library looks for devices non-stop while nothing is connected, which costs a
// steady chunk of battery. Here it scans continuously only while the grown-ups
// Bluetooth tab is open (pairing), and otherwise in one short burst a minute, which
// is still enough for the saved headphones to reconnect when they're switched on.
class TinyA2DP : public BluetoothA2DPSource {
 public:
  void continuousScan(bool on) {  // the library restarts a finished scan unless is_end
    is_end = !on;
    if (on) scanBurst();
  }
  void scanBurst() {  // one ~13 s look-around
    if (discovery_active || is_connected()) return;
    if (s_a2d_state != APP_AV_STATE_DISCOVERING && s_a2d_state != APP_AV_STATE_UNCONNECTED) return;  // busy connecting
    s_a2d_state = APP_AV_STATE_DISCOVERING;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  }
  bool isSaved(const uint8_t* addr) {  // the last headphones/speaker we connected to
    static const uint8_t none[ESP_BD_ADDR_LEN] = {0};
    return memcmp(last_connection, none, ESP_BD_ADDR_LEN) && !memcmp(addr, last_connection, ESP_BD_ADDR_LEN);
  }
};
static TinyA2DP a2dp;

// ---------- ring buffer: decoder (loop task) -> A2DP callback (BT task) ----------
// Single producer / single consumer, so atomics on head/tail are enough.
static constexpr uint32_t RING_LEN = 32768;  // frames, ~740 ms at 44.1 kHz (in PSRAM)
static Frame* ring = nullptr;
static std::atomic<uint32_t> ringHead{0}, ringTail{0};

static uint32_t ringFree() { return RING_LEN - 1 - (ringHead - ringTail) % RING_LEN; }

static void ringPush(int16_t l, int16_t r) {
  uint32_t h = ringHead.load();
  ring[h].channel1 = l;
  ring[h].channel2 = r;
  ringHead.store((h + 1) % RING_LEN);
}

// Called from the BT stack whenever it wants more audio. Always returns a full
// block (silence on underrun) so the stream never stalls.
static int32_t btGetFrames(Frame* data, int32_t len) {
  if (ringFlushReq) { ringFlushReq = false; ringTail.store(ringHead.load()); }
  uint32_t t = ringTail.load(), h = ringHead.load();
  int32_t i = 0;
  for (; i < len && t != h; i++) {
    data[i] = ring[t];
    t = (t + 1) % RING_LEN;
  }
  ringTail.store(t);
  for (; i < len; i++) data[i] = Frame(0);
  return len;
}
#endif

// Routes decoded audio either to the BT ring (resampled to 44.1 kHz) or to
// M5Unified's speaker task via a triple buffer.
class AudioOutputRouter : public AudioOutput {
 public:
  explicit AudioOutputRouter(m5::Speaker_Class* spk, uint8_t ch) : _spk(spk), _ch(ch) {}
  bool begin() override { _pos = 0; return true; }
  bool SetRate(int hz) override { hertz = hz; applyRate(); return true; }
  bool ConsumeSample(int16_t s[2]) override {
#if !CORE2_WIFI
    if (btConnected) return consumeBT(s);
#endif
    return consumeSpeaker(s);
  }
  void flush() override { flushSpeaker(); }
  // Can it take more audio without waiting? M5Unified's playRaw() blocks while the
  // speaker's two queue slots are full, which would stall loop() (touch, drawing, the
  // buzz motor's off switch) for up to a buffer's length.
  bool hasRoom() {
#if !CORE2_WIFI
    if (btConnected) return ringFree() > 4096;
#endif
    return _spk->isPlaying(_ch) < 2;  // one decode pass hands over at most one buffer
  }
  bool stop() override {
    flushSpeaker();
    _spk->stop(_ch);
    ringFlushReq = true;
    return true;
  }

 private:
#if !CORE2_WIFI
  bool consumeBT(int16_t s[2]) {
    if (hertz == BT_RATE) {
      if (!ringFree()) return false;
      ringPush(s[0], s[1]);
      return true;
    }
    // linear-interpolation resampler; 8 kHz input -> up to 6 outputs per input
    if (ringFree() < 8) return false;
    while (_pos < 1.0f) {
      ringPush(_prev[0] + (s[0] - _prev[0]) * _pos, _prev[1] + (s[1] - _prev[1]) * _pos);
      _pos += _step;
    }
    _pos -= 1.0f;
    _prev[0] = s[0];
    _prev[1] = s[1];
    return true;
  }
#endif
  bool consumeSpeaker(int16_t s[2]) {
    if (_idx < BUF_LEN) {
      _buf[_tri][_idx] = s[0];
      _buf[_tri][_idx + 1] = s[1];
      _idx += 2;
      return true;
    }
    flushSpeaker();
    return false;
  }
  void flushSpeaker() {
    if (_idx) {
      _spk->playRaw(_buf[_tri], _idx, hertz, true, 1, _ch);
      _tri = (_tri + 1) % 3;
      _idx = 0;
    }
  }

  void applyRate() {
#if !CORE2_WIFI
    _step = (float)hertz / BT_RATE;
#endif
  }

  static constexpr size_t BUF_LEN = 8192;  // ~280 ms queued, so slow redraws don't stutter
  m5::Speaker_Class* _spk;
  uint8_t _ch;
  int16_t _buf[3][BUF_LEN];
  size_t _idx = 0, _tri = 0;
  float _step = 1.0f, _pos = 0;
  int16_t _prev[2] = {0, 0};
};

// ---------- player state ----------
static AudioOutputRouter out(&M5.Speaker, SPK_CH);
static AudioGeneratorMP3* mp3 = nullptr;
static AudioFileSourceSD* file = nullptr;

// The library is album folders on the SD card: "/Artist - Album/01 Song.mp3",
// with an optional cover.jpg. MP3s in the card's root form an album called "Songs".
// Folders are read straight off the card (no index to go stale); an album's
// tracks are listed only when it's opened.
static std::vector<Album> albums;
static std::vector<Shelf> shelves;
static int playAlbum = -1;          // album whose tracks are in `tracks`
static std::vector<String> tracks;  // tracks of the playing album
static int current = -1;
static bool paused = false;
static uint8_t volume = 0;      // actual output volume 0..255
static int volLevel = 6;        // what the kid sees: 0..10
static int maxVolPct = 70;      // grown-up volume limit, percent
static String title;
static String artist;  // the playing song's artist (tag, or guessed from the names)
static bool dirtyAll = true, dirtyNow = true;
static volatile bool connectDirty = false;  // grown-ups screen needs redraw
static Screen screen = Screen::Player;  // which screen is showing
static int songCountCache = -1;  // stats page; reset when the library changes
static uint32_t listenSec = 0, songsPlayed = 0;  // stats page; saved in prefs
static String owner;           // whose Tiny Tunes this is (grown-ups set it from the web page or upload.py)
static bool nightSync = false;  // this WiFi boot is the nightly podcast sync (see "nightly podcast sync")
static void nightSyncDone();

static void stopPlayback();
static void playTrack(int i);
static void playAlbumTrack(int album, int track);
static void rescanLibrary(bool keepPlaying = false);
static void setVolLevel(int lvl);
static void togglePause();
static void switchMode(bool quiet = false);
static bool flickedX(const m5::touch_detail_t& t, int& dir);
static bool flickedY(const m5::touch_detail_t& t, int& dir);
static String asciiFold(const String& in);
static bool musicIsPlaying();
static void stopPlayback();
static String cleanTitle(String t, bool fromFilename, bool dropArtist);
static String trackTitle(const String& path, int album);
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b);
static uint32_t be32(const uint8_t* b);

// ASCII case helpers. libc's tolower/toupper/strncasecmp live in IRAM in this
// core, and the BT build has no IRAM to spare, so these stay in flash.
static char lowerc(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static String lowerStr(String s) {
  for (int i = 0; i < (int)s.length(); i++) s.setCharAt(i, lowerc(s[i]));
  return s;
}
static bool startsNoCase(const char* s, const char* prefix, int n) {
  for (int i = 0; i < n; i++)
    if (!s[i] || lowerc(s[i]) != lowerc(prefix[i])) return false;
  return true;
}
static std::vector<String> listTracks(int a);
static void loadTags(int a, const std::vector<String>& files);
static bool readId3(File& f, Id3Info& out, bool wantPic);
static void scanAlbumTag(Album& al);
static void arenaReset();
static bool isPodcastAlbum(int a);
static String joinPath(const String& dir, const char* name);
static bool isMp3(const char* name);
static int findAlbum(const String& path);
static bool jpegSize(const String& path, int& w, int& h);
static bool removePath(const String& path);
static uint32_t fileSize(const String& path);
static std::vector<ResumeEntry> loadResume(const String& albumPath);
static void saveResume(const String& albumPath, const std::vector<ResumeEntry>& v);
static void rescanKeepingPlace();
static void setOwner(String name);
static String guessArtist(int a, const String& path);

// Everything runs in loop(), so slow work (a web request, drawing a new screen, listing
// a big album) would starve the MP3 decoder; the SD card and the screen even share one
// SPI bus. Slow code calls this between chunks of work to keep the music going.
static void buzzService();
static void audioPump() {
  buzzService();  // a tap's buzz ends on time even in the middle of a slow screen
  if (!mp3 || !mp3->isRunning() || paused) return;
  for (int i = 0; i < 8 && out.hasRoom() && mp3->loop(); i++) {}
}

// The file that's playing (or loaded), or "".
static String playingPath() { return file && current >= 0 && current < (int)tracks.size() ? tracks[current] : String(); }
// Would changing `path` (a file or folder) pull the rug out from under the playing song?
static bool touchesPlaying(const String& path) {
  String p = playingPath();
  return p.length() && (p == path || p.startsWith(path + "/"));
}

static constexpr const char* COVER_CACHE = ".cover.rgb565";  // decoded cover, see "album covers"
static constexpr int COVER_MASTER = 160;                      // ...160x160 RGB565 after this header:
struct CoverCacheHeader { char magic[4]; uint32_t srcSize; };

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

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tiny Tunes</title><style>
:root{color-scheme:light dark;--bg:#fff;--fg:#111;--dim:#777;--acc:#0a9;--row:#f0f0f0;--line:#ddd;--warn:#c33}
@media(prefers-color-scheme:dark){:root{--bg:#111;--fg:#eee;--dim:#999;--row:#1d1d1d;--line:#333;--warn:#f66}}
body{font:16px system-ui;margin:0 auto;max-width:640px;padding:16px;background:var(--bg);color:var(--fg)}
h1{font-size:22px;margin:0 0 8px}h2{font-size:17px;margin:24px 0 8px}button{font:inherit;padding:6px 12px;cursor:pointer}
.ctl{display:flex;gap:8px;margin:10px 0}.ctl button{flex:1;padding:12px;font-size:18px}
input[type=text]{width:100%;box-sizing:border-box;padding:8px;font-size:16px}
#drop{border:2px dashed var(--dim);border-radius:8px;padding:18px;text-align:center;margin:8px 0}#drop.over{border-color:var(--acc)}
.dim{color:var(--dim);font-size:14px}.warn{color:var(--warn)}progress{width:100%}
details{border:1px solid var(--line);border-radius:8px;margin:8px 0}summary{display:flex;align-items:center;gap:10px;padding:8px;cursor:pointer}
summary img,summary .nocover{width:44px;height:44px;border-radius:6px;object-fit:cover;background:var(--row);flex:none}
summary b{flex:1}.cur>summary{background:var(--row)}
.abtn{display:flex;gap:6px;flex-wrap:wrap;padding:0 8px 8px}
.trk{display:flex;align-items:center;gap:6px;padding:4px 8px;border-top:1px solid var(--line)}
.trk .t{flex:1;min-width:0}.trk .t div{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.trk.playing .t>div:first-child{color:var(--acc);font-weight:600}
.trk select{max-width:130px}.icon{padding:4px 8px}
#plan label{display:flex;gap:8px;padding:4px 0;align-items:flex-start}#plan code{font-size:13px}
</style></head><body><h1 id="title">Tiny Tunes</h1>
<div id="now" class="dim">...</div>
<div class="ctl"><button onclick="cmd('prev')">&#9198;</button><button onclick="cmd('play')">&#9199;</button>
<button onclick="cmd('next')">&#9197;</button></div>
<div class="ctl"><button onclick="cmd('voldown')">Vol &minus;</button><button onclick="cmd('volup')">Vol +</button></div>

<h2>Add music or books</h2>
<div class="ctl"><select id="shelf" onchange="fillAlbumList()" style="flex:1"></select>
<input type="text" id="album" list="albumlist" placeholder="Album or book name" style="flex:3"></div><datalist id="albumlist"></datalist>
<div id="drop">Drop MP3s and a cover picture here, or <input type="file" id="pick" multiple accept=".mp3,audio/mpeg,image/*"></div>
<progress id="prog" value="0" max="1" hidden></progress><div id="msg" class="dim"></div>

<h2>Library</h2>
<div class="ctl"><button onclick="planCleanup()">&#10024; Clean up</button><button onclick="newAlbum()">+ New album</button></div>
<div id="plan"></div>
<div id="lib" class="dim">Loading...</div>

<h2>Podcasts</h2>
<div class="dim">Paste a show's RSS feed or its Apple Podcasts link. New episodes download every night after
midnight while the Tiny Tunes is plugged in (it switches to WiFi and back on its own), and also soon after
WiFi mode connects. They play like albums in Bluetooth mode.</div>
<div class="ctl"><input type="text" id="feedurl" placeholder="https://... feed or Apple Podcasts link" style="flex:3">
<select id="feedkeep" title="Episodes to keep"><option value="3">Keep 3</option><option value="5" selected>Keep 5</option><option value="10">Keep 10</option></select>
<button onclick="addFeed()" style="flex:1">Add</button></div>
<div class="ctl"><button onclick="syncNow()">&#8635; Sync now</button><button id="syncstop" onclick="stopSync()" hidden>&#9632; Stop</button></div>
<div id="syncline" class="dim"></div><progress id="syncprog" value="0" max="100" hidden></progress>
<div id="pods"></div>

<h2>Settings</h2>
<div class="ctl"><input type="text" id="owner" maxlength="16" placeholder="Whose Tiny Tunes is this? (first name)" style="flex:3">
<button onclick="saveOwner()" style="flex:1">Save</button></div>
<div class="dim" id="hostline"></div>

<div class="ctl" style="margin-top:28px"><button onclick="if(confirm('Reboot into Bluetooth mode? This page stops working until you switch back.'))cmd('btmode')">Switch to Bluetooth mode</button></div>
<p class="dim" id="ver" style="text-align:center"></p>
<script>
const $=id=>document.getElementById(id), q=encodeURIComponent;
let lib={root:'/',albums:[]};
const say=t=>$('msg').textContent=t;
const post=async(u)=>{const r=await fetch(u,{method:'POST'});if(!r.ok)throw new Error(await r.text()||r.status);return r};
const safe=s=>s.normalize('NFKD').replace(/[^\x20-\x7e]/g,'').replace(/[\\/:*?"<>|]/g,'_').replace(/\s+/g,' ').trim();
const dirOf=p=>p.slice(0,p.lastIndexOf('/'))||'/', join=(d,n)=>(d=='/'?'':d)+'/'+n;
function esc(s){return s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}

let ownerShown=null;
async function refreshNow(){const s=await (await fetch('/state')).json();
 if(s.version)$('ver').textContent='Tiny Tunes '+s.version;
 if(s.owner!==ownerShown){ownerShown=s.owner;const t=s.owner?s.owner+"'s Tiny Tunes":'Tiny Tunes';
  document.title=t;$('title').textContent=t;if(document.activeElement!=$('owner'))$('owner').value=s.owner;
  $('hostline').textContent=`Also at http://${s.host}.local (the name applies after the next restart).`}
 $('now').textContent=s.tracks.length?(s.paused?'Paused: ':'Playing: ')+(s.tracks[s.current]||'')+' — '+s.album+'  (vol '+s.volume+'%)':'Nothing playing';
 return s}
async function cmd(c){await post('/cmd?c='+c);refreshNow();if(/album|track/.test(c))setTimeout(loadLib,300)}
async function saveOwner(){await post('/owner?name='+q($('owner').value.trim()));ownerShown=null;refreshNow()}

// ---------- library / organize ----------
// Where uploads go: Music uses the library root (/music if the card has it); other shelves their folder.
function shelfDir(){return $('shelf').value}
function fillShelves(){const cur=$('shelf').value;
 const opts=[{name:'Music',path:lib.root}].concat(lib.shelves.filter(sh=>sh.kind!='music'&&sh.kind!='podcasts'));
 if(!opts.some(o=>o.path=='/books'))opts.splice(1,0,{name:'Books',path:'/books'});
 $('shelf').innerHTML=opts.map(o=>`<option value="${esc(o.path)}">${esc(o.name)}</option>`).join('');
 if(cur&&opts.some(o=>o.path==cur))$('shelf').value=cur;fillAlbumList()}
function fillAlbumList(){const base=shelfDir();
 $('albumlist').innerHTML=lib.albums.filter(a=>a.folder!=base&&dirOf(a.folder)==base).map(a=>`<option value="${esc(a.folder.split('/').pop())}">`).join('')}
async function loadLib(){
 lib=await (await fetch('/library')).json();const s=await refreshNow();
 const open=new Set([...document.querySelectorAll('#lib details[open]')].map(d=>d.dataset.path));
 fillShelves();
 $('lib').innerHTML=lib.albums.length?'':'No albums yet.';
 let lastShelf=-1;
 lib.albums.forEach((a,ai)=>{
  const sh=lib.shelves[a.shelf]||{name:'',kind:'music'};
  if(sh.kind=='podcasts')return;  // podcasts have their own section below
  if(a.shelf!=lastShelf){lastShelf=a.shelf;const h=document.createElement('h3');h.textContent=sh.name;h.style.margin='14px 0 4px';$('lib').appendChild(h)}
  const d=document.createElement('details');d.dataset.path=a.folder;if(open.has(a.folder))d.open=true;
  if(ai==lib.playing)d.className='cur';
  const cover=a.cover||a.art?`<img loading="lazy" src="/cover?i=${ai}&v=${a.coverSize}" onerror="this.replaceWith(Object.assign(document.createElement('span'),{className:'nocover'}))">`:'<span class="nocover"></span>';
  const others=lib.albums.map((b,bi)=>bi==ai?'':`<option value="${bi}">${esc(b.name)}</option>`).join('');
  d.innerHTML=`<summary>${cover}<b>${esc(a.name)}</b><span class="dim">${a.tracks.length} songs</span></summary>
   <div class="abtn"><button onclick="cmd('album&i=${ai}')">&#9654; Play</button>
   ${a.folder==lib.root||a.folder=='/'?'':`<button onclick="renameAlbum(${ai})">Rename</button>`}
   <label><button onclick="this.nextElementSibling.click()">Cover...</button><input type="file" accept="image/*" hidden onchange="setCover(${ai},this.files[0])"></label>
   ${a.folder==lib.root||a.folder=='/'?'':`<button class="warn" onclick="delAlbum(${ai})">Delete album</button>`}</div>`+
   a.tracks.map((t,ti)=>`<div class="trk${ai==lib.playing&&ti==s.current?' playing':''}">
    <button class="icon" title="Play" onclick="cmd('album&i=${ai}');setTimeout(()=>cmd('track&i=${ti}'),400)">&#9654;</button>
    <div class="t"><div>${esc(t.title)}</div><div class="dim">${esc(t.file)}</div></div>
    <select onchange="moveTrack(${ai},${ti},this.value)"><option value="">Move to...</option>${others}</select>
    <button class="icon warn" title="Delete" onclick="delTrack(${ai},${ti})">&#10005;</button></div>`).join('')+
   (a.other.length?`<div class="trk dim">Other files: ${a.other.map(esc).join(', ')}</div>`:'');
  $('lib').appendChild(d)})}

async function afterChange(m){await post('/rescan');say(m||'Done');await loadLib()}
async function renameAlbum(ai){const a=lib.albums[ai],n=prompt('New album name',a.name);if(!n||!safe(n))return;
 try{await post(`/mv?from=${q(a.folder)}&to=${q(join(dirOf(a.folder),safe(n)))}`);afterChange('Renamed')}catch(e){alert('Rename failed: '+e.message)}}
async function delAlbum(ai){const a=lib.albums[ai];if(!confirm(`Delete "${a.name}" and its ${a.tracks.length} songs from the card?`))return;
 try{await post('/rm?path='+q(a.folder));afterChange('Deleted '+a.name)}catch(e){alert('Delete failed: '+e.message)}}
async function delTrack(ai,ti){const a=lib.albums[ai],t=a.tracks[ti];if(!confirm(`Delete "${t.title}"?`))return;
 try{await post('/rm?path='+q(join(a.folder,t.file)));afterChange('Deleted '+t.title)}catch(e){alert('Delete failed: '+e.message)}}
async function moveTrack(ai,ti,bi){if(bi==='')return;const a=lib.albums[ai],b=lib.albums[bi],t=a.tracks[ti];
 try{await post(`/mv?from=${q(join(a.folder,t.file))}&to=${q(join(b.folder,t.file))}`);afterChange(`Moved ${t.title} to ${b.name}`)}catch(e){alert('Move failed: '+e.message)}}
async function newAlbum(){const n=prompt('Album name');if(!n||!safe(n))return;
 try{await post('/mkdir?path='+q(join(shelfDir(),safe(n))));$('album').value=safe(n);say(`Created ${safe(n)}. Drop songs above to fill it.`);loadLib()}catch(e){alert(e.message)}}
async function setCover(ai,f){if(!f)return;const a=lib.albums[ai];
 try{const blob=await shrinkCover(f);if(a.cover)await post('/rm?path='+q(join(a.folder,a.cover)));
  await send(blob,'cover.jpg',a.folder);afterChange('Cover updated')}catch(e){alert('Cover failed: '+e)}}

// ---------- uploads ----------
function send(blob,name,dir){return new Promise((ok,bad)=>{const x=new XMLHttpRequest(),fd=new FormData();fd.append('f',blob,name);
 x.upload.onprogress=e=>{$('prog').value=e.loaded/e.total};x.onload=()=>x.status==200?ok():bad(x.responseText);
 x.onerror=()=>bad('network error');x.open('POST','/upload?dir='+q(dir));x.send(fd)})}
// covers are shrunk to a 160x160 baseline JPEG so the Core2 can decode them quickly
function shrinkCover(blob){return new Promise((ok,bad)=>{const img=new Image();img.onload=()=>{const c=document.createElement('canvas');
 c.width=c.height=160;const s=Math.min(img.width,img.height),g=c.getContext('2d');
 g.drawImage(img,(img.width-s)/2,(img.height-s)/2,s,s,0,0,160,160);URL.revokeObjectURL(img.src);c.toBlob(b=>b?ok(b):bad('cover'),'image/jpeg',0.85)};
 img.onerror=()=>bad('not an image');img.src=URL.createObjectURL(blob)})}
async function upload(files){const name=safe($('album').value);files=[...files];const dir=name?join(shelfDir(),name):shelfDir();
 const songs=files.filter(f=>/\.mp3$/i.test(f.name)),cover=files.find(f=>f.type.startsWith('image/'));
 if(cover&&!name){say('Type an album name to add a cover.');return}
 if(!songs.length&&!cover)return;$('prog').hidden=false;
 try{if(cover){say('Uploading cover');await send(await shrinkCover(cover),'cover.jpg',dir)}
  for(const [i,f] of songs.entries()){say(`Uploading ${i+1}/${songs.length}: ${f.name}`);await send(f,safe(f.name)||'song.mp3',dir)}}
 catch(e){say('Failed: '+e);return}
 $('prog').hidden=true;afterChange(`Added ${songs.length} song(s)`+(cover?' and a cover':''))}
const d=$('drop');d.ondragover=e=>{e.preventDefault();d.classList.add('over')};d.ondragleave=()=>d.classList.remove('over');
d.ondrop=e=>{e.preventDefault();d.classList.remove('over');upload(e.dataTransfer.files)};$('pick').onchange=e=>upload(e.target.files);

// ---------- clean up ----------
// Plans fixes from the device's view of the library, shows them as a checklist, then applies the ticked ones.
function prettyName(n){if(!n.includes(' '))n=n.replace(/[-_]+/g,' ');return n.replace(/(^|\s)[a-z]/g,c=>c.toUpperCase())}
function id3Art(buf){const b=new Uint8Array(buf);if(b[0]!=73||b[1]!=68||b[2]!=51||(b[3]!=3&&b[3]!=4))return null;
 const ver=b[3],size=(b[6]<<21)|(b[7]<<14)|(b[8]<<7)|b[9];let p=10;
 while(p+10<=Math.min(10+size,b.length)){const id=String.fromCharCode(b[p],b[p+1],b[p+2],b[p+3]);if(!/^[A-Z0-9]{4}$/.test(id))break;
  const fs=ver==4?(b[p+4]<<21)|(b[p+5]<<14)|(b[p+6]<<7)|b[p+7]:((b[p+4]<<24)|(b[p+5]<<16)|(b[p+6]<<8)|b[p+7])>>>0;
  const body=b.subarray(p+10,p+10+fs);p+=10+fs;if(id!='APIC'||body.length<4)continue;
  const enc=body[0];let i=body.indexOf(0,1)+1;const mime=new TextDecoder().decode(body.subarray(1,i-1));i++;
  if(enc==1||enc==2){while(i+1<body.length&&!(body[i]==0&&body[i+1]==0))i+=2;i+=2}else{while(i<body.length&&body[i]!=0)i++;i++}
  return new Blob([body.subarray(i)],{type:mime||'image/jpeg'})}
 return null}
async function planCleanup(){
 await loadLib();const plan=[];
 for(const a of lib.albums){
  if((lib.shelves[a.shelf]||{}).kind=='podcasts')continue;  // managed by the podcast sync
  const isRoot=a.folder==lib.root||a.folder=='/';
  // song files -> "Clean Title.mp3" (keeping a leading track number if there was one)
  const used=new Set(a.tracks.map(t=>t.file.toLowerCase())),renames=[];
  a.tracks.forEach(t=>{const num=(t.file.match(/^(\d{1,3})[\s._-]/)||[])[1];
   let base=safe(t.title)||'Song',name=(num?num.padStart(2,'0')+' ':'')+base+'.mp3';
   for(let k=2;name.toLowerCase()!=t.file.toLowerCase()&&renames.some(r=>r.to.toLowerCase()==name.toLowerCase());k++)name=(num?num.padStart(2,'0')+' ':'')+base+' '+k+'.mp3';
   if(name!=t.file)renames.push({from:t.file,to:name})});
  if(renames.length)plan.push({on:true,label:`Rename ${renames.length} song file(s) in <b>${esc(a.name)}</b>`,
   detail:renames.map(r=>`<code>${esc(r.from)}</code> &rarr; <code>${esc(r.to)}</code>`).join('<br>'),
   run:async()=>{for(const [i,r] of renames.entries())await post(`/mv?from=${q(join(a.folder,r.from))}&to=${q(join(a.folder,'~tmp'+i+'.mp3'))}`);
    for(const [i,r] of renames.entries())await post(`/mv?from=${q(join(a.folder,'~tmp'+i+'.mp3'))}&to=${q(join(a.folder,r.to))}`)}});
  // cover.jpg from an image in the folder, or art embedded in the songs
  // no picture in the folder: make cover.jpg from art embedded in the songs (JPG/PNG covers are used as is)
  if(!a.cover&&!a.art&&a.tracks.length)plan.push({on:true,label:`Make a cover for <b>${esc(a.name)}</b> from the art inside its songs`,
    run:async()=>{let blob=null;
     for(const t of a.tracks.slice(0,3)){blob=id3Art(await (await fetch(`/file?path=${q(join(a.folder,t.file))}&n=1500000`)).arrayBuffer());if(blob)break}
     if(!blob)throw new Error('no art inside the songs');
     await send(await shrinkCover(blob),'cover.jpg',a.folder)}});
  // macOS junk always; other stray files only if ticked
  const junk=a.other.filter(f=>f.startsWith('._')||f=='.DS_Store'),extra=a.other.filter(f=>!junk.includes(f)&&!f.startsWith('.'));
  if(junk.length)plan.push({on:true,label:`Delete ${junk.length} Mac junk file(s) in <b>${esc(a.name)}</b>`,detail:junk.map(f=>`<code>${esc(f)}</code>`).join(' '),
   run:async()=>{for(const f of junk)await post('/rm?path='+q(join(a.folder,f)))}});
  for(const f of extra)plan.push({on:false,label:`Delete <code>${esc(f)}</code> from <b>${esc(a.name)}</b>`,run:()=>post('/rm?path='+q(join(a.folder,f)))});
  // album folder name last, since it changes the paths above
  const pretty=safe(prettyName(a.folder.split('/').pop()));
  if(!isRoot&&pretty&&pretty!=a.folder.split('/').pop())plan.push({on:true,label:`Rename album <code>${esc(a.folder.split('/').pop())}</code> &rarr; <b>${esc(pretty)}</b>`,
   run:()=>post(`/mv?from=${q(a.folder)}&to=${q(join(dirOf(a.folder),pretty))}`)});
 }
 if(!plan.length){$('plan').innerHTML='<p class="dim">Everything already looks tidy.</p>';return}
 $('plan').innerHTML=`<p>Here's what Clean up would change. Untick anything you want to keep as is.</p>`+
  plan.map((p,i)=>`<label><input type="checkbox" ${p.on?'checked':''} data-i="${i}"><span>${p.label}${p.detail?`<details><summary class="dim">show</summary><div class="dim">${p.detail}</div></details>`:''}</span></label>`).join('')+
  `<div class="ctl"><button id="apply">Apply</button><button onclick="$('plan').innerHTML=''">Cancel</button></div>`;
 $('apply').onclick=async()=>{const todo=[...$('plan').querySelectorAll('input:checked')].map(c=>plan[c.dataset.i]);
  $('prog').hidden=false;let failed=[];
  for(const [i,p] of todo.entries()){$('prog').value=i/todo.length;say(`Cleaning up ${i+1}/${todo.length}...`);
   try{await p.run()}catch(e){failed.push(p.label.replace(/<[^>]+>/g,'')+': '+e.message)}}
  $('prog').hidden=true;$('plan').innerHTML=failed.length?`<p class="warn">Some fixes failed:<br>${failed.map(esc).join('<br>')}</p>`:'';
  afterChange(`Cleaned up (${todo.length-failed.length} fixes)`)}}

// ---------- podcasts ----------
let podTimer=null;
const mb=b=>(b/1e6).toFixed(1)+' MB';
async function loadPods(){clearTimeout(podTimer);let p;try{p=await (await fetch('/podcasts')).json()}catch(e){podTimer=setTimeout(loadPods,10000);return}
 const busy=p.state!='idle';
 $('syncline').innerHTML=esc(p.status)+(p.error?` <span class="warn">${esc(p.error)}</span>`:'')+
  (!busy&&p.ago>=0?` <span class="dim">(checked ${p.ago<1?'just now':p.ago<120?p.ago+' min ago':Math.round(p.ago/60)+' h ago'})</span>`:'');
 $('syncprog').hidden=!(busy&&p.pct>=0);$('syncprog').value=Math.max(p.pct,0);$('syncstop').hidden=!busy;
 const open=new Set([...document.querySelectorAll('#pods details[open]')].map(d=>d.dataset.url));
 $('pods').innerHTML=p.feeds.length?'':'<p class="dim">No podcasts yet.</p>';
 p.feeds.forEach((f,i)=>{const d=document.createElement('details');d.dataset.url=f.url;if(open.has(f.url))d.open=true;
  const total=f.episodes.reduce((s,e)=>s+e.size,0);
  d.innerHTML=`<summary><b>${esc(f.name||'New podcast (syncing...)')}</b><span class="dim">${f.episodes.length} ep, ${mb(total)}</span></summary>
   <div class="abtn"><span class="dim" style="flex-basis:100%;overflow-wrap:anywhere">${esc(f.url)}</span>
   <select onchange="setKeep(${i},this.value)">${[1,3,5,10,20].map(k=>`<option value="${k}"${k==f.keep?' selected':''}>Keep ${k}</option>`).join('')}</select>
   <button class="warn" onclick="removeFeed(${i})">Remove podcast</button></div>`+
   f.episodes.map(e=>`<div class="trk"><div class="t"><div>${esc(e.file.replace(/\.mp3$/i,''))}</div><div class="dim">${mb(e.size)}</div></div>
    <button class="icon warn" title="Delete episode" onclick="delEpisode('${esc(f.folder)}','${esc(e.file).replace(/'/g,"\\'")}')">&#10005;</button></div>`).join('');
  $('pods').appendChild(d)});
 podTimer=setTimeout(loadPods,busy?2000:15000)}
async function addFeed(){const url=$('feedurl').value.trim();if(!url)return;$('syncline').textContent='Adding...';
 const r=await fetch(`/podcast/add?url=${q(url)}&keep=${$('feedkeep').value}`,{method:'POST'});
 if(!r.ok){$('syncline').textContent='Could not add: '+await r.text();return}
 $('feedurl').value='';loadPods()}
async function setKeep(i,k){await post(`/podcast/keep?i=${i}&keep=${k}`);loadPods()}
async function removeFeed(i){if(!confirm('Remove this podcast? Its downloaded episodes are deleted too.'))return;
 try{await post(`/podcast/remove?i=${i}&files=1`);loadPods();loadLib()}catch(e){alert(e.message)}}
async function delEpisode(folder,file){if(!confirm('Delete this episode? (It may download again on the next sync if it is still one of the newest.)'))return;
 await post('/rm?path='+q(join(folder,file)));await post('/rescan');loadPods();loadLib()}
async function syncNow(){await post('/podcast/sync');setTimeout(loadPods,500)}
async function stopSync(){await post('/podcast/stop');setTimeout(loadPods,300)}

loadLib();loadPods();setInterval(refreshNow,5000);
</script></body></html>)HTML";

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
struct Feed { String url, name; int keep; };
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

// ---------- library ----------
static bool isMp3(const char* name) {
  if (name[0] == '.') return false;  // skip macOS "._" files and hidden files
  size_t n = strlen(name);
  return n > 4 && strcasecmp(name + n - 4, ".mp3") == 0;
}

static bool isHidden(const char* name) {
  return name[0] == '.' || !strcasecmp(name, "System Volume Information");
}

static bool lessCI(const String& a, const String& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; }

static String joinPath(const String& dir, const char* name) { return (dir == "/" ? "" : dir) + "/" + name; }

// Lists an album's MP3s and notes whether most are named "Artist - Title".
static std::vector<String> listTracks(int a) {
  std::vector<String> found;
  File dir = SD.open(albums[a].path);
  if (!dir || !dir.isDirectory()) return found;
  int dashed = 0;
  while (File f = dir.openNextFile()) {
    audioPump();
    if (!f.isDirectory() && isMp3(f.name())) {
      found.push_back(joinPath(albums[a].path, f.name()));
      if (strstr(f.name(), " - ")) dashed++;
    }
    f.close();
  }
  dir.close();
  std::sort(found.begin(), found.end(), lessCI);
  albums[a].artistPrefixed = !found.empty() && dashed * 10 >= (int)found.size() * 6;
  if (isPodcastAlbum(a)) {  // date-named: newest first (podcasts go by their names, not tags)
    std::reverse(found.begin(), found.end());
    return found;
  }
  loadTags(a, found);
  // when every song has a track number, play them in disc / track order
  auto& tags = albums[a].tags;
  auto tagOf = [&](const String& path) -> const TrackTag* {
    String f = path.substring(path.lastIndexOf('/') + 1);
    for (auto& t : tags) if (!strcmp(t.file, f.c_str())) return &t;
    return nullptr;
  };
  bool numbered = !found.empty();
  for (auto& p : found) { const TrackTag* t = tagOf(p); numbered &= t && t->track > 0; }
  if (numbered)
    std::stable_sort(found.begin(), found.end(), [&](const String& x, const String& y) {
      const TrackTag *tx = tagOf(x), *ty = tagOf(y);
      return tx->disc != ty->disc ? tx->disc < ty->disc : tx->track < ty->track;
    });
  return found;
}

// How many songs an album has, without reading any tags (the stats page).
static int countTracks(int a) {
  int n = 0;
  File dir = SD.open(albums[a].path);
  if (!dir || !dir.isDirectory()) return 0;
  while (File f = dir.openNextFile()) { n += !f.isDirectory() && isMp3(f.name()); f.close(); }
  dir.close();
  return n;
}

// "yoto-fleetwood" -> "Yoto Fleetwood"; names with spaces are left as typed.
static String prettyAlbumName(String n) {
  if (n.indexOf(' ') < 0) { n.replace('-', ' '); n.replace('_', ' '); }
  for (int i = 0; i < (int)n.length(); i++)
    if ((i == 0 || n[i - 1] == ' ') && n[i] >= 'a' && n[i] <= 'z') n.setCharAt(i, n[i] - 32);
  return n;
}

static void clearCoverCache();

// Cover image preference: cover.* > folder.* > front.* > album.* > any other JPG/PNG.
static int coverRank_(const char* n) {
  if (n[0] == '.') return 99;
  const char* dot = strrchr(n, '.');
  if (!dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") && strcasecmp(dot, ".png"))) return 99;
  int stem = dot - n, rank = 0;
  for (const char* w : {"cover", "folder", "front", "album"}) {
    if (stem == (int)strlen(w) && startsNoCase(n, w, stem)) return rank;
    rank++;
  }
  return 10;
}

// Lists a folder: whether it holds MP3s, its best cover image, and its subfolders.
static bool scanFolder(const String& dirPath, std::vector<String>* subdirs, String& cover) {
  bool hasMp3 = false;
  int coverRank = 99;
  cover = "";
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) return false;
  while (File f = dir.openNextFile()) {
    const char* n = f.name();
    if (f.isDirectory()) { if (subdirs && !isHidden(n)) subdirs->push_back(n); }
    else if (isMp3(n)) hasMp3 = true;
    else if (int r = coverRank_(n); r < coverRank) { coverRank = r; cover = n; }
    f.close();
  }
  dir.close();
  return hasMp3;
}

static int addShelf(const String& name, const String& path, ShelfKind kind) {
  for (size_t i = 0; i < shelves.size(); i++)
    if (shelves[i].path == path) return i;
  shelves.push_back({name, path, kind});
  return shelves.size() - 1;
}

static void clearCoverCache();
static int countNewEpisodes(int a);
static void onLibraryChanged();
// keepPlaying: leave the decoder running (the caller re-finds the playing song).
static void rescanLibrary(bool keepPlaying) {
  if (!keepPlaying) stopPlayback();
  albums.clear();
  arenaReset();
  shelves.clear();
  // the three known shelves first, in this order; they stay even when empty (other
  // shelves only exist when they hold albums)
  addShelf("Podcasts", "/podcasts", ShelfKind::Podcasts);
  int music = addShelf("Music", "/music", ShelfKind::Music);
  addShelf("Books", "/books", ShelfKind::Books);
  std::vector<String> top;
  String cover;
  if (scanFolder("/", &top, cover)) albums.push_back({"Songs", "/", cover, -1, music});  // loose songs
  std::sort(top.begin(), top.end(), lessCI);
  for (auto& t : top) {
    String path = joinPath("/", t.c_str()), lower = lowerStr(t);
    std::vector<String> subs;
    bool loose = scanFolder(path, &subs, cover);
    bool known = lower == "music" || lower == "podcasts" || lower == "books";
    int shelf = known ? addShelf(prettyAlbumName(t), path, lower == "music" ? ShelfKind::Music
                                 : lower == "podcasts" ? ShelfKind::Podcasts : ShelfKind::Books)
                      : -1;
    if (loose) albums.push_back({lower == "music" ? String("Songs") : prettyAlbumName(t), path, cover, -1, known ? shelf : music});
    for (auto& sub : subs) {
      String sp = joinPath(path, sub.c_str());
      if (!scanFolder(sp, nullptr, cover)) continue;
      if (shelf < 0) shelf = addShelf(prettyAlbumName(t), path, ShelfKind::Other);  // a folder of albums = a shelf
      albums.push_back({prettyAlbumName(sub), sp, cover, -1, shelf});
    }
  }
  // drop empty extra shelves (renumbering), then order albums by shelf and name (loose songs last)
  std::vector<int> remap(shelves.size(), -1);
  std::vector<Shelf> kept;
  for (size_t i = 0; i < shelves.size(); i++) {
    bool used = shelves[i].kind != ShelfKind::Other;  // Music, Podcasts, Books always
    for (auto& al : albums) used |= al.shelf == (int)i;
    if (used) { remap[i] = kept.size(); kept.push_back(shelves[i]); }
  }
  shelves = kept;
  for (auto& al : albums) al.shelf = remap[al.shelf];
  for (auto& al : albums)  // names, artists and pictures from the tags (podcasts keep their show names)
    if (al.shelf >= 0 && al.shelf < (int)shelves.size() && shelves[al.shelf].kind != ShelfKind::Podcasts) { audioPump(); scanAlbumTag(al); }
  std::sort(albums.begin(), albums.end(), [](const Album& a, const Album& b) {
    if (a.shelf != b.shelf) return a.shelf < b.shelf;
    bool la = a.name == "Songs", lb = b.name == "Songs";
    return la != lb ? lb : lessCI(a.name, b.name);
  });
  for (size_t i = 0; i < albums.size(); i++) {
    Shelf& sh = shelves[albums[i].shelf];
    sh.albums++;
    if (sh.kind == ShelfKind::Podcasts) sh.newCount += albums[i].newCount = countNewEpisodes(i);
  }
  if (!keepPlaying) {
    playAlbum = -1;
    tracks.clear();
    current = -1;
  }
  clearCoverCache();
  for (size_t i = 0; i < albums.size(); i++)  // the library index: every album's songs and tags, now
    if (!isPodcastAlbum(i)) listTracks(i);
  songCountCache = -1;
  onLibraryChanged();
}

// ---------- recently played ----------
// Albums played lately, newest first (their folders, up to 40), kept on the card in
// /.recent so each shelf can list them first.
static constexpr const char* RECENT_FILE = "/.recent";
static constexpr size_t RECENT_MAX = 40;
static std::vector<String> recentAlbums;

static void loadRecent() {
  recentAlbums.clear();
  File f = SD.open(RECENT_FILE);
  while (f && f.available() && recentAlbums.size() < RECENT_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) recentAlbums.push_back(line);
  }
  if (f) f.close();
}

static void noteRecent(const String& path) {
  if (!recentAlbums.empty() && recentAlbums[0] == path) return;  // already first: nothing to write
  recentAlbums.erase(std::remove(recentAlbums.begin(), recentAlbums.end(), path), recentAlbums.end());
  recentAlbums.insert(recentAlbums.begin(), path);
  if (recentAlbums.size() > RECENT_MAX) recentAlbums.resize(RECENT_MAX);
  File f = SD.open(RECENT_FILE, FILE_WRITE);
  if (!f) return;
  for (auto& p : recentAlbums) f.println(p);
  f.close();
}

static int recentRank(const String& path) {
  for (size_t i = 0; i < recentAlbums.size(); i++)
    if (recentAlbums[i] == path) return i;
  return INT_MAX;
}

static int findAlbum(const String& path) {
  for (size_t i = 0; i < albums.size(); i++)
    if (albums[i].path == path) return i;
  return -1;
}

// Rescans the card but keeps the player's place: the same album and song stay loaded,
// and a song that's playing keeps playing (unless its file is gone).
static void rescanKeepingPlace() {
  String albumPath = playAlbum >= 0 && playAlbum < (int)albums.size() ? albums[playAlbum].path : "";
  String trackPath = current >= 0 && current < (int)tracks.size() ? tracks[current] : "";
  int track = std::max(current, 0);
  rescanLibrary(true);
  int a = findAlbum(albumPath);
  if (a < 0 && !albums.empty()) { a = 0; track = 0; }
  if (a < 0) { stopPlayback(); playAlbum = -1; tracks.clear(); current = -1; return; }
  playAlbum = a;
  tracks = listTracks(a);
  auto it = std::find(tracks.begin(), tracks.end(), trackPath);
  if (it != tracks.end()) {
    current = it - tracks.begin();
  } else {  // the song we had is gone: load another, paused
    stopPlayback();
    current = tracks.empty() ? -1 : std::min<int>(track, tracks.size() - 1);
    paused = true;
  }
  if (current >= 0 && !file) { title = trackTitle(tracks[current], a); artist = guessArtist(a, tracks[current]); }
  dirtyNow = true;
}

// ---------- titles ----------
// The display font is ASCII, so fold common UTF-8 (smart quotes, accents) to ASCII.
static String asciiFold(const String& in) {
  static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";  // U+00C0..U+00FF
  String o;
  for (int i = 0; i < (int)in.length();) {
    uint8_t c = in[i];
    if (c < 0x80) { o += (char)c; i++; continue; }
    uint32_t cp = 0;
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    cp = n == 1 ? c : c & (0x7F >> n);
    for (int k = 1; k < n && i + k < (int)in.length(); k++) cp = (cp << 6) | (in[i + k] & 0x3F);
    i += n;
    if (cp >= 0xC0 && cp <= 0xFF) o += latin1[cp - 0xC0];
    else if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032) o += '\'';
    else if (cp == 0x201C || cp == 0x201D) o += '"';
    else if (cp == 0x2013 || cp == 0x2014) o += '-';
    else if (cp == 0x2026) o += "...";
    // anything else is dropped
  }
  return o;
}

// Tags kids don't need: "(Taylor's Version)", "- 2004 Remaster", "(feat. X)", "(From The Vault)"...
static bool isNoise(String s) {
  s = lowerStr(s);
  for (const char* w : {"remaster", "version", "feat.", "feat ", "ft.", "from ", "bonus", "vault", "explicit",
                        "deluxe", "edit)", "edit]", "mono", "stereo", "soundtrack"})
    if (s.indexOf(w) >= 0) return true;
  return false;
}

// "03 Fleetwood Mac - Everywhere - 2017 Remaster" -> "Everywhere"
static String cleanTitle(String t, bool fromFilename, bool dropArtist) {
  t = asciiFold(t);
  if (fromFilename) {  // leading track numbers: "03 ", "1-03 - ", "03_"
    int i = 0;
    while (i < (int)t.length() && (isdigit(t[i]) || strchr(" -_.", t[i]))) i++;
    if (i > 0 && isdigit(t[0]) && i < (int)t.length()) t = t.substring(i);
    t.replace('_', ' ');
  }
  for (int k = 0; k < 6; k++) {  // trailing (noise) / [noise] groups first, then " - noise" segments
    int o = std::max(t.lastIndexOf('('), t.lastIndexOf('['));
    if (o > 0 && isNoise(t.substring(o))) { t = t.substring(0, o); t.trim(); continue; }
    int p = t.lastIndexOf(" - ");
    if (p > 0 && isNoise(t.substring(p + 3))) { t = t.substring(0, p); continue; }
    break;
  }
  if (dropArtist) {  // "Artist, Other Artist - Title"
    int p = t.indexOf(" - ");
    if (p > 0 && p + 3 < (int)t.length()) t = t.substring(p + 3);
  }
  while (t.indexOf("  ") >= 0) t.replace("  ", " ");
  t.trim();
  return t.length() ? t : String("Song");
}

static bool isPodcastAlbum(int a) {
  return a >= 0 && a < (int)albums.size() && albums[a].shelf >= 0 && albums[a].shelf < (int)shelves.size() &&
         shelves[albums[a].shelf].kind == ShelfKind::Podcasts;
}

// "Wow in the World: The Moon" in the "Wow in the World" show -> "The Moon"
static String dropShowName(String t, const String& show) {
  if (show.length() < 3 || t.length() <= show.length() + 2 || !startsNoCase(t.c_str(), show.c_str(), show.length())) return t;
  int i = show.length();
  while (i < (int)t.length() && strchr(" :-|", t[i])) i++;
  return i < (int)t.length() ? t.substring(i) : t;
}

static String trackTitle(const String& path, int a) {
  String n = path.substring(path.lastIndexOf('/') + 1);
  if (a >= 0 && a < (int)albums.size() && !isPodcastAlbum(a))
    for (auto& t : albums[a].tags)
      if (!strcmp(t.file, n.c_str()) && t.title[0]) return t.title;  // the song's tag says

  if (n.length() > 4) n.remove(n.length() - 4);
  String t = cleanTitle(n, true, a >= 0 && a < (int)albums.size() && albums[a].artistPrefixed == 1);
  return isPodcastAlbum(a) ? dropShowName(t, albums[a].name) : t;
}

static String albumName() { return playAlbum >= 0 ? albums[playAlbum].name : String(); }

// ---------- owner ----------
// The grown-ups can name whose Tiny Tunes this is: "Hi, Ava!" on the splash, "Ava's Tiny
// Tunes" on the web page, and ava-tunes.local on the network (WiFi mode), which tells two
// of them apart. Set from the web page, upload.py --name, or the NAME serial command.
static void setOwner(String name) {
  name = asciiFold(name);  // the screen font is ASCII
  name.trim();
  if (name.length() > 16) name = name.substring(0, 16);
  owner = name;
  if (owner.length()) prefs.putString("owner", owner);
  else prefs.remove("owner");
}

// "Ava Rose" -> "ava-rose-tunes"; "tiny-tunes" with no owner.
static String hostName() {
  String h;
  for (char c : owner) {
    c = lowerc(c);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) h += c;
    else if (h.length() && !h.endsWith("-")) h += '-';
  }
  while (h.endsWith("-")) h.remove(h.length() - 1);
  return h.length() ? h + "-tunes" : String("tiny-tunes");
}

// ---------- resume (Books and Podcasts) ----------
// Each book or show folder keeps a small ".resume" file: one "file<TAB>byte position<TAB>done"
// line per track that has been started. It's saved every 15 s while playing, and on pause,
// stop and track change, so a power-off loses at most ~15 s. Music doesn't resume.
static constexpr const char* RESUME_FILE = ".resume";
static std::vector<ResumeEntry> playResume;  // the playing album's entries
static String playResumePath;
static bool trackJustFinished = false;       // don't let stopPlayback() overwrite "done"

static bool isResumable(int a) {
  if (a < 0 || a >= (int)albums.size() || albums[a].shelf < 0 || albums[a].shelf >= (int)shelves.size()) return false;
  ShelfKind k = shelves[albums[a].shelf].kind;
  return k == ShelfKind::Books || k == ShelfKind::Podcasts;
}

static String fileName(const String& path) { return path.substring(path.lastIndexOf('/') + 1); }

static std::vector<ResumeEntry> loadResume(const String& albumPath) {
  std::vector<ResumeEntry> v;
  File f = SD.open(joinPath(albumPath, RESUME_FILE));
  while (f && f.available()) {
    String line = f.readStringUntil('\n');
    int a = line.indexOf('\t'), b = a < 0 ? -1 : line.indexOf('\t', a + 1);
    if (b > a && a > 0) v.push_back({line.substring(0, a), (uint32_t)line.substring(a + 1, b).toInt(), line.substring(b + 1).toInt() == 1});
  }
  if (f) f.close();
  return v;
}

static void saveResume(const String& albumPath, const std::vector<ResumeEntry>& v) {
  File f = SD.open(joinPath(albumPath, RESUME_FILE), FILE_WRITE);
  if (!f) return;
  for (auto& e : v) f.printf("%s\t%u\t%d\n", e.file.c_str(), (unsigned)e.pos, e.done ? 1 : 0);
  f.close();
}

static ResumeEntry* findResume(std::vector<ResumeEntry>& v, const String& f) {
  for (auto& e : v) if (e.file == f) return &e;
  return nullptr;
}

static std::vector<ResumeEntry>& resumeFor(int a) {
  if (playResumePath != albums[a].path) { playResume = loadResume(albums[a].path); playResumePath = albums[a].path; }
  return playResume;
}

static int countNewEpisodes(int a) {
  auto v = loadResume(albums[a].path);
  int n = 0;
  for (auto& t : listTracks(a)) n += !findResume(v, fileName(t));
  return n;
}

// Records where the playing track is, or that it played to the end.
static void saveResumePos(bool finished) {
  if (!file || current < 0 || current >= (int)tracks.size() || !isResumable(playAlbum)) return;
  auto& v = resumeFor(playAlbum);
  String f = fileName(tracks[current]);
  ResumeEntry* e = findResume(v, f);
  if (!e) { v.push_back({f, 0, false}); e = &v.back(); }
  e->pos = finished ? 0 : file->getPos();
  e->done = finished;
  saveResume(albums[playAlbum].path, v);
}

// ---------- playback ----------
static uint32_t be32(const uint8_t* b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static uint32_t synchsafe(const uint8_t* b) { return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }

// ---------- tags ----------
// The library index (every album's songs and their tags) is built when the card is
// scanned, at boot and after changes made through the device. Its text lives in PSRAM:
// hundreds of little Strings would eat the internal RAM that Bluetooth needs.
static std::vector<char*> arenaBlocks;
static size_t arenaUsed = 0;
static constexpr size_t ARENA_BLOCK = 16384;

static const char* arenaDup(const String& s) {
  size_t n = std::min<size_t>(s.length(), 255) + 1;
  if (arenaBlocks.empty() || arenaUsed + n > ARENA_BLOCK) {
    char* b = (char*)heap_caps_malloc(ARENA_BLOCK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) return "";
    arenaBlocks.push_back(b);
    arenaUsed = 0;
  }
  char* p = arenaBlocks.back() + arenaUsed;
  memcpy(p, s.c_str(), n - 1);
  p[n - 1] = 0;
  arenaUsed += n;
  return p;
}

static void arenaReset() {
  for (char* b : arenaBlocks) heap_caps_free(b);
  arenaBlocks.clear();
  arenaUsed = 0;
}

// A title from a tag, tidied: "01 - The Cyclone" / "3. Dreams" -> "The Cyclone" / "Dreams"
// (a number only counts as a track number when " - " or ". " follows it, so
// "99 Luftballons" keeps its 99).
static String tagTitleClean(const String& tag) {
  String t = cleanTitle(asciiFold(tag), false, false);
  int i = 0;
  while (i < (int)t.length() && i < 3 && isdigit(t[i])) i++;
  if (i > 0 && i + 2 < (int)t.length() && ((t[i] == ' ' && t[i + 1] == '-') || t[i] == '.')) {
    int j = i + 1;
    while (j < (int)t.length() && strchr(" -.", t[j])) j++;
    if (j > i + 1 && j < (int)t.length()) t = t.substring(j);
  }
  return t == "Song" ? String() : t;
}
// Songs are named by their ID3v2 tags where they have them (title, artist, album, album
// artist, track and disc numbers, and a picture for albums without a cover image), and
// by their file and folder names otherwise. An album's tags are read once per rescan.

// One ID3 text frame's value as UTF-8: Latin-1, UTF-16 (with or without a byte-order
// mark) or UTF-8. Characters outside the BMP are dropped (the screen font is ASCII anyway).
static String id3Text(const uint8_t* v, uint32_t size) {
  String out;
  uint8_t enc = v[0];
  uint32_t i = 1;
  if (enc == 1 || enc == 2) {
    bool be = enc == 2;
    if (enc == 1 && i + 1 < size) {
      if (v[i] == 0xFE && v[i + 1] == 0xFF) { be = true; i += 2; }
      else if (v[i] == 0xFF && v[i + 1] == 0xFE) i += 2;
    }
    for (; i + 1 < size; i += 2) {
      uint16_t c = be ? (v[i] << 8 | v[i + 1]) : (v[i + 1] << 8 | v[i]);
      if (!c) break;
      if (c >= 0xD800 && c < 0xE000) continue;
      if (c < 0x80) out += (char)c;
      else if (c < 0x800) { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 0x3F)); }
      else { out += (char)(0xE0 | c >> 12); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
  } else if (enc == 0) {  // Latin-1 -> UTF-8
    for (; i < size && v[i]; i++) {
      if (v[i] < 0x80) out += (char)v[i];
      else { out += (char)(0xC0 | v[i] >> 6); out += (char)(0x80 | (v[i] & 0x3F)); }
    }
  } else if (enc == 3) {  // UTF-8
    for (; i < size && v[i]; i++) out += (char)v[i];
  }
  out.trim();
  return out;
}

// Reads an ID3v2.3 / v2.4 tag. Seeks past every frame it doesn't need (ESP8266Audio's
// AudioFileSourceID3 reads the whole tag a byte at a time, which took seconds on songs
// with cover art inside). wantPic: also note where the embedded picture is. Returns
// false when there's no tag (out.audioStart is then 0).
static bool readId3(File& f, Id3Info& out, bool wantPic) {
  out = Id3Info();
  uint8_t h[10];
  f.seek(0);
  if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) || h[3] < 3 || h[3] > 4) return false;
  uint32_t tagEnd = 10 + synchsafe(h + 6) + ((h[5] & 0x10) ? 10 : 0);  // + footer
  out.audioStart = tagEnd;
  if (h[5] & 0x80) return true;  // unsynchronised tag (rare): just skip it
  uint32_t pos = 10;
  if (h[5] & 0x40) {  // extended header
    uint8_t e[4];
    if (f.read(e, 4) != 4) return true;
    pos += h[3] == 4 ? synchsafe(e) : be32(e) + 4;
  }
  while (pos + 10 <= tagEnd) {
    uint8_t fh[10];
    f.seek(pos);
    if (f.read(fh, 10) != 10 || !fh[0]) break;  // the end, or padding
    uint32_t size = h[3] == 4 ? synchsafe(fh + 4) : be32(fh + 4);
    if (!size || pos + 10 + size > tagEnd) break;
    const char* id = (const char*)fh;
    if (id[0] == 'T' && size < 512) {
      String* text = !memcmp(id, "TIT2", 4) ? &out.title : !memcmp(id, "TPE1", 4) ? &out.artist
                   : !memcmp(id, "TALB", 4) ? &out.album : !memcmp(id, "TPE2", 4) ? &out.albumArtist : nullptr;
      int* num = !memcmp(id, "TRCK", 4) ? &out.track : !memcmp(id, "TPOS", 4) ? &out.disc : nullptr;
      if (text || num) {
        uint8_t v[512];
        if (f.read(v, size) != size) break;
        String t = id3Text(v, size);
        if (text) *text = t;
        else *num = t.toInt();  // "3/12" -> 3
      }
    } else if (wantPic && !out.picLen && !memcmp(id, "APIC", 4) && size > 32) {
      uint8_t v[160];
      uint32_t n = std::min<uint32_t>(size, sizeof v), i = 1;
      if (f.read(v, n) != n) break;
      uint8_t enc = v[0];
      String mime;
      while (i < n && v[i]) mime += (char)v[i++];
      i += 2;  // the mime type's nul, the picture type
      if (enc == 1 || enc == 2) { while (i + 1 < n && (v[i] || v[i + 1])) i += 2; i += 2; }  // description
      else { while (i < n && v[i]) i++; i++; }
      if (i < n) {
        out.picOff = pos + 10 + i;
        out.picLen = size - i;
        out.picPng = lowerStr(mime).indexOf("png") >= 0 || (v[i] == 0x89 && v[i + 1] == 'P');
      }
    }
    pos += 10 + size;
  }
  return true;
}

// Fills an album's tags (titles and track numbers) the first time it's listed.
static void loadTags(int a, const std::vector<String>& files) {
  Album& al = albums[a];
  if (al.tagsRead) return;
  al.tagsRead = true;
  al.tags.clear();
  for (auto& path : files) {
    audioPump();
    File f = SD.open(path);
    if (!f) continue;
    Id3Info tag;
    readId3(f, tag, false);
    f.close();
    String title = tag.title.length() ? tagTitleClean(tag.title) : String();
    al.tags.push_back({arenaDup(path.substring(path.lastIndexOf('/') + 1)), arenaDup(title), (uint16_t)tag.track, (uint16_t)tag.disc});
  }
}

// At scan time, one song per album: the album's name and artist from its tag, and
// whether it carries a picture (for albums without a cover image).
static void scanAlbumTag(Album& al) {
  File dir = SD.open(al.path);
  if (!dir || !dir.isDirectory()) return;
  String first;
  while (File f = dir.openNextFile()) {
    if (!f.isDirectory() && isMp3(f.name()) && (!first.length() || lessCI(f.name(), first))) first = f.name();
    f.close();
  }
  dir.close();
  if (!first.length()) return;
  String path = joinPath(al.path, first.c_str());
  File f = SD.open(path);
  if (!f) return;
  Id3Info tag;
  bool any = readId3(f, tag, !al.coverFile.length());
  f.close();
  if (!any) return;
  if (tag.album.length() && al.name != "Songs") {
    String n = cleanTitle(asciiFold(tag.album), false, false);
    if (n != "Song") al.name = n;
  }
  String who = tag.albumArtist.length() ? tag.albumArtist : tag.artist;
  if (who.length()) al.albumArtist = asciiFold(who);
  if (tag.picLen) al.artFile = path;
}

// The artist when the song's tag doesn't say: the show for podcasts, "Artist - Album"
// folder names, or "Artist - Title" file names.
static String guessArtist(int a, const String& path) {
  if (a < 0 || a >= (int)albums.size()) return "";
  if (isPodcastAlbum(a)) return albums[a].name;
  if (albums[a].albumArtist.length()) return albums[a].albumArtist;
  int p = albums[a].name.indexOf(" - ");
  if (p > 0) return albums[a].name.substring(0, p);
  if (albums[a].artistPrefixed == 1) {
    String n = fileName(path);
    if (n.length() > 4) n.remove(n.length() - 4);
    String t = cleanTitle(n, true, false);
    int q = t.indexOf(" - ");
    if (q > 0) return t.substring(0, q);
  }
  return "";
}

static void stopPlayback() {
  if (!trackJustFinished) saveResumePos(false);  // books/podcasts: remember the spot
  if (mp3 && mp3->isRunning()) mp3->stop();
  delete file; file = nullptr;
}

static uint32_t forceStartPos = 0;  // recovering from a damaged spot, or seeking: start here instead
static uint32_t trackAudioStart = 0;  // byte where the audio begins (after the ID3 tag)
static int trackKbps = 128;           // its bitrate, for "min left" and skipping by seconds
static int recoverTries = 0;

// The bitrate in the first MPEG audio frame header, leaving `f` where it was.
// (VBR files report their first frame's rate, which is close enough here.)
static int readMp3Kbps(AudioFileSourceSD* f) {
  static const uint16_t V1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};  // MPEG-1 layer III
  static const uint16_t V2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};     // MPEG-2/2.5 layer III
  uint32_t at = f->getPos();
  uint8_t h[4];
  int kbps = 0;
  if (f->read(h, 4) == 4 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) kbps = ((h[1] & 0x18) == 0x18 ? V1 : V2)[h[2] >> 4];
  f->seek(at, SEEK_SET);
  return kbps ? kbps : 128;
}

static void playTrack(int i) {
  static int missing = 0;  // files in a row that wouldn't open
  if (tracks.empty()) return;
  i = (i + tracks.size()) % tracks.size();  // stays inside the album, wrapping around
  bool recovering = forceStartPos != 0;
  if (!recovering) recoverTries = 0;
  stopPlayback();
  current = i;
  paused = false;
  file = new AudioFileSourceSD(tracks[i].c_str());
  if (!file->isOpen()) {  // deleted or unreadable: skip it (but don't spin forever)
    delete file;
    file = nullptr;
    forceStartPos = 0;
    if (++missing < (int)tracks.size()) { playTrack(i + 1); return; }
    missing = 0;
    paused = true;
    title = trackTitle(tracks[i], playAlbum); artist = guessArtist(playAlbum, tracks[i]);
    dirtyNow = true;
    return;
  }
  missing = 0;
  Id3Info tag;
  if (File tf = SD.open(tracks[i])) { readId3(tf, tag, false); tf.close(); }
  file->seek(tag.audioStart, SEEK_SET);
  String tagTitle = tag.title, tagArtist = tag.artist.length() ? tag.artist : tag.albumArtist;
  artist = tagArtist.length() ? asciiFold(tagArtist) : guessArtist(playAlbum, tracks[i]);
  artist.trim();
  trackAudioStart = tag.audioStart;
  trackKbps = readMp3Kbps(file);
  title = tagTitle.length() ? tagTitleClean(tagTitle) : String();
  if (!title.length()) title = trackTitle(tracks[i], playAlbum);
  if (isPodcastAlbum(playAlbum)) title = dropShowName(title, albums[playAlbum].name);
  if (isResumable(playAlbum)) {  // pick up where this track was left, a moment early
    auto& v = resumeFor(playAlbum);
    String f = fileName(tracks[i]);
    ResumeEntry* e = findResume(v, f);
    uint32_t start = file->getPos(), size = file->getSize();
    if (!recovering && e && !e->done && e->pos > start + 48000 && e->pos + 16000 < size) file->seek(e->pos - 24000, SEEK_SET);
    if (!e) {  // first listen: no longer "new"
      v.push_back({f, start, false});
      saveResume(albums[playAlbum].path, v);
      Album& al = albums[playAlbum];
      if (al.newCount > 0) { al.newCount--; shelves[al.shelf].newCount--; }
    }
  }
  if (recovering) { file->seek(std::max(forceStartPos, trackAudioStart), SEEK_SET); forceStartPos = 0; }  // never inside the tag
  mp3->begin(file, &out);  // the decoder finds the next frame on its own after a seek
  dirtyNow = true;
  if (recovering) return;  // same song, carrying on
  songsPlayed++;
  prefs.putUInt("played", songsPlayed);
  prefs.putString("album", albums[playAlbum].path);  // power-on picks up here
  noteRecent(albums[playAlbum].path);
  prefs.putInt("track", current);
}

static void playAlbumTrack(int a, int t) {
  if (a < 0 || a >= (int)albums.size()) return;
  if (a != playAlbum) {
    stopPlayback();
    playAlbum = a;
    tracks = listTracks(a);
    current = -1;
  }
  playTrack(t);
}

static void togglePause() {
  if (current < 0) return;
  if (paused && !(mp3 && mp3->isRunning())) { playTrack(current); return; }  // loaded but not started
  paused = !paused;
  if (paused) saveResumePos(false);
  if (paused) { M5.Speaker.stop(SPK_CH); ringFlushReq = true; }
  dirtyNow = true;
}

static void applyVolume() {
  volume = volLevel * maxVolPct * 255 / 1000;
  M5.Speaker.setVolume(volume);
#if !CORE2_WIFI
  a2dp.set_volume(volume / 2);  // A2DP volume is 0..127
#endif
}

static void setVolLevel(int lvl) {
  volLevel = constrain(lvl, 0, 10);
  applyVolume();
}

// ---------- mode switching ----------
// Reboots into the firmware in the other app slot, carrying volume and track over.
static void switchMode(bool quiet) {
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next || esp_ota_set_boot_partition(next) != ESP_OK) {
    Serial.println("ERR other mode not installed");
    return;
  }
  prefs.putUChar("lvl", volLevel);
  prefs.putString("album", playAlbum >= 0 ? albums[playAlbum].path : "");
  prefs.putInt("track", current);
  Serial.println("OK");
  Serial.flush();
  stopPlayback();
  if (!quiet) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(CORE2_WIFI ? "Switching to Bluetooth..." : "Switching to WiFi...", 160, 120);
  }
  delay(300);
  esp_restart();
}

// ---------- album covers ----------
// Each album's cover (JPG or PNG, any size) is decoded once into a 160x160 image
// and saved next to it as ".cover.rgb565", so later boots skip the slow decode
// (a big PNG can take seconds). Sizes shown on screen are scaled from that.
// Progressive JPEGs aren't supported by the decoder; those albums show a color tile.
static constexpr size_t COVER_BYTES = COVER_MASTER * COVER_MASTER * 2;

static bool jpegSize(const String& path, int& w, int& h) {
  File f = SD.open(path);
  if (!f) return false;
  uint8_t b[9];
  bool ok = false;
  if (f.read(b, 2) == 2 && b[0] == 0xFF && b[1] == 0xD8) {
    while (f.read(b, 4) == 4 && b[0] == 0xFF) {
      int len = (b[2] << 8) | b[3];
      uint8_t marker = b[1];
      if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {  // start of frame
        if (f.read(b, 5) == 5) { h = (b[1] << 8) | b[2]; w = (b[3] << 8) | b[4]; }
        ok = marker != 0xC2 && w > 0 && h > 0;  // C2 = progressive, which the decoder can't do
        break;
      }
      f.seek(f.position() + len - 2);
    }
  }
  f.close();
  return ok;
}

static bool pngSize(const String& path, int& w, int& h) {
  File f = SD.open(path);
  uint8_t b[24];
  bool ok = f && f.read(b, 24) == 24 && !memcmp(b, "\x89PNG", 4);
  if (ok) { w = be32(b + 16); h = be32(b + 20); ok = w > 0 && h > 0; }
  if (f) f.close();
  return ok;
}

static M5Canvas* newCanvas(int size) {
  auto* c = new M5Canvas(&M5.Display);
  c->setPsram(true);
  c->setColorDepth(16);
  if (!c->createSprite(size, size)) { delete c; return nullptr; }
  c->setPivot(size / 2.0f - 0.5f, size / 2.0f - 0.5f);
  return c;
}

static uint32_t fileSize(const String& path) {
  File f = SD.open(path);
  uint32_t n = f ? f.size() : 0;
  if (f) f.close();
  return n;
}

// Loads the cached 160x160 cover if it's still current for the source image.
static bool readCoverCache(const Album& al, uint32_t srcSize, M5Canvas* into) {
  File f = SD.open(joinPath(al.path, COVER_CACHE));
  if (!f) return false;
  CoverCacheHeader h;
  bool ok = f.read((uint8_t*)&h, sizeof h) == sizeof h && !memcmp(h.magic, "C2C1", 4) && h.srcSize == srcSize &&
            (!into || f.read((uint8_t*)into->getBuffer(), COVER_BYTES) == COVER_BYTES);
  f.close();
  return ok;
}

// An album's picture: its cover image file, or else the picture inside one of its songs'
// tags. key tells the cache which one it was made from (embedded pictures set the top bit).
static bool coverSource(const Album& al, String& path, uint32_t& off, uint32_t& len, bool& png, uint32_t& key) {
  if (al.coverFile.length()) {
    path = joinPath(al.path, al.coverFile.c_str());
    off = 0;
    len = key = fileSize(path);
    png = lowerStr(al.coverFile).endsWith(".png");
    return len > 0;
  }
  if (!al.artFile.length()) return false;
  File f = SD.open(al.artFile);
  if (!f) return false;
  Id3Info tag;
  readId3(f, tag, true);
  f.close();
  if (!tag.picLen || tag.picLen > 4000000) return false;
  path = al.artFile;
  off = tag.picOff;
  len = tag.picLen;
  png = tag.picPng;
  key = tag.picLen | 0x80000000u;
  return true;
}

static bool hasCover(const Album& al) { return al.coverFile.length() || al.artFile.length(); }

static bool coverCacheCurrent(const Album& al) {
  String path;
  uint32_t off, len, key;
  bool png;
  return coverSource(al, path, off, len, png, key) && readCoverCache(al, key, nullptr);
}

// A JPEG's size from its bytes (false for progressive JPEGs, which the decoder can't do).
static bool jpegSizeMem(const uint8_t* b, uint32_t n, int& w, int& h) {
  if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;
  for (uint32_t i = 2; i + 9 < n && b[i] == 0xFF;) {
    uint8_t marker = b[i + 1];
    uint32_t len = (b[i + 2] << 8) | b[i + 3];
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      h = (b[i + 5] << 8) | b[i + 6];
      w = (b[i + 7] << 8) | b[i + 8];
      return marker != 0xC2 && w > 0 && h > 0;
    }
    i += 2 + len;
  }
  return false;
}

// A card color from the cover: its colors averaged, weighted toward the more colorful
// pixels (a plain average comes out grey), then darkened enough for white text.
static uint16_t coverTint(M5Canvas* c) {
  const uint16_t* px = (const uint16_t*)c->getBuffer();
  uint64_t r = 0, g = 0, b = 0, wsum = 0;
  for (int i = 0; i < COVER_MASTER * COVER_MASTER; i += 5) {
    uint16_t v = __builtin_bswap16(px[i]);  // sprite pixels are big-endian RGB565
    int pr = (v >> 11) << 3, pg = ((v >> 5) & 63) << 2, pb = (v & 31) << 3;
    int w = std::max({pr, pg, pb}) - std::min({pr, pg, pb}) + 8;  // saturation, roughly
    r += pr * w; g += pg * w; b += pb * w; wsum += w;
  }
  int cr = r / wsum, cg = g / wsum, cb = b / wsum, hi = std::max({cr, cg, cb, 1});
  if (hi > 160) { cr = cr * 160 / hi; cg = cg * 160 / hi; cb = cb * 160 / hi; }
  if (hi < 50) { cr += 30; cg += 30; cb += 45; }  // a black cover: a little lift so the card shows
  return M5.Display.color565(cr, cg, cb) | 1;   // never 0, which means "not known yet"
}

static M5Canvas* loadCoverMaster(Album& al) {
  String src;
  uint32_t off, len, key;
  bool png;
  if (!coverSource(al, src, off, len, png, key)) return nullptr;
  M5Canvas* c = newCanvas(COVER_MASTER);
  if (!c) return nullptr;
  if (readCoverCache(al, key, c)) { al.tint = coverTint(c); return c; }

  int w = 0, h = 0;
  bool ok = false;
  c->fillSprite(TFT_BLACK);
  auto scale = [&](float& sc, int& offX, int& offY) {  // fill the square, center-crop the rest
    sc = (float)COVER_MASTER / std::min(w, h);
    offX = (w * sc - COVER_MASTER) / 2;
    offY = (h * sc - COVER_MASTER) / 2;
  };
  float sc;
  int offX, offY;
  if (!off) {  // an image file
    ok = png ? pngSize(src, w, h) : jpegSize(src, w, h);
    if (ok) {
      scale(sc, offX, offY);
      ok = png ? c->drawPngFile(SD, src.c_str(), 0, 0, COVER_MASTER, COVER_MASTER, offX, offY, sc, sc)
               : c->drawJpgFile(SD, src.c_str(), 0, 0, COVER_MASTER, COVER_MASTER, offX, offY, sc, sc);
    }
  } else {  // the picture inside a song: read it into PSRAM and decode from there
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    File f = buf ? SD.open(src) : File();
    ok = f && f.seek(off) && f.read(buf, len) == len;
    if (f) f.close();
    if (ok) {
      png = len > 24 && !memcmp(buf, "\x89PNG", 4);
      if (png) { w = be32(buf + 16); h = be32(buf + 20); ok = w > 0 && h > 0; }
      else ok = jpegSizeMem(buf, len, w, h);
    }
    if (ok) {
      scale(sc, offX, offY);
      ok = png ? c->drawPng(buf, len, 0, 0, COVER_MASTER, COVER_MASTER, offX, offY, sc, sc)
               : c->drawJpg(buf, len, 0, 0, COVER_MASTER, COVER_MASTER, offX, offY, sc, sc);
    }
    if (buf) heap_caps_free(buf);
  }
  if (!ok) { delete c; return nullptr; }
  al.tint = coverTint(c);
  File out = SD.open(joinPath(al.path, COVER_CACHE), FILE_WRITE);
  if (out) {
    CoverCacheHeader hdr{{'C', '2', 'C', '1'}, key};
    out.write((const uint8_t*)&hdr, sizeof hdr);
    out.write((const uint8_t*)c->getBuffer(), COVER_BYTES);
    out.close();
  }
  return c;
}

// Scaled covers get rounded corners painted in a key color that pushSprite treats
// as transparent, so one cached copy looks right on any background.
static constexpr uint16_t COVER_KEY = 0x0120;  // a very dark green no UI element uses

static void roundCorners(M5Canvas* c, int r) {
  int n = c->width();
  for (int y = 0; y < r; y++)
    for (int x = 0; x < r; x++) {
      int dx = r - x - 1, dy = r - y - 1;
      if (dx * dx + dy * dy <= r * r) continue;
      c->drawPixel(x, y, COVER_KEY);
      c->drawPixel(n - 1 - x, y, COVER_KEY);
      c->drawPixel(x, n - 1 - y, COVER_KEY);
      c->drawPixel(n - 1 - x, n - 1 - y, COVER_KEY);
    }
}

// Scaled covers (the sizes the screens use) are kept in PSRAM, not the 160 px originals
// they're made from. The carousel's are made for every album at boot, while there's
// room; past that the least recently used make way.
static std::vector<CoverEntry> coverCache;
static constexpr size_t COVER_PSRAM_RESERVE = 700000;  // leave this much PSRAM for everything else

static void clearCoverCache() {
  for (auto& e : coverCache) delete e.canvas;
  coverCache.clear();
}

// Halfway to the background color (the carousel's side covers), keeping the corner key.
static void dimCanvas(M5Canvas* c) {
  uint16_t* px = (uint16_t*)c->getBuffer();
  uint16_t key = __builtin_bswap16(COVER_KEY);
  for (int i = 0, n = c->width() * c->height(); i < n; i++) {
    if (px[i] == key) continue;
    uint16_t v = __builtin_bswap16(px[i]);
    int r = ((v >> 11) + (18 >> 3)) / 2, g = (((v >> 5) & 63) + (22 >> 2)) / 2, b = ((v & 31) + (48 >> 3)) / 2;
    px[i] = __builtin_bswap16((uint16_t)(r << 11 | g << 5 | b));
  }
}

static M5Canvas* scaleCover(M5Canvas* master, int size, bool dim) {
  M5Canvas* c = newCanvas(size);
  if (!c) return nullptr;
  master->pushRotateZoomWithAA(c, size / 2.0f - 0.5f, size / 2.0f - 0.5f, 0, (float)size / COVER_MASTER, (float)size / COVER_MASTER);
  if (dim) dimCanvas(c);
  roundCorners(c, size / 8);
  return c;
}

static CoverEntry* findCover(const String& path, int size, bool dim) {
  for (auto& e : coverCache)
    if (e.size == size && e.dim == dim && e.path == path) return &e;
  return nullptr;
}

// Frees the least recently used covers until there's room again.
static void makeCoverRoom() {
  while (!coverCache.empty() && (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < COVER_PSRAM_RESERVE + 60000 || coverCache.size() >= 400)) {
    auto lru = std::min_element(coverCache.begin(), coverCache.end(), [](const CoverEntry& x, const CoverEntry& y) { return x.used < y.used; });
    delete lru->canvas;
    coverCache.erase(lru);
  }
}

static M5Canvas* getCover(int a, int size, bool dim = false) {
  if (a < 0 || a >= (int)albums.size() || !hasCover(albums[a])) return nullptr;
  const String& path = albums[a].path;
  if (CoverEntry* e = findCover(path, size, dim)) { e->used = millis(); return e->canvas; }
  makeCoverRoom();
  M5Canvas* master = loadCoverMaster(albums[a]);
  M5Canvas* c = master ? scaleCover(master, size, dim) : nullptr;
  delete master;
  coverCache.push_back({path, size, dim, c, millis()});  // a failed decode is cached as null too
  audioPump();  // loading and scaling a cover is the slowest part of drawing a screen
  return c;
}

// Makes an album's covers in all the sizes the screens use (one load of the original).
static void prepareAlbumArt(int a, const int* sizes, const bool* dims, int n) {
  if (a < 0 || a >= (int)albums.size() || !hasCover(albums[a])) return;
  M5Canvas* master = nullptr;
  for (int k = 0; k < n; k++) {
    if (findCover(albums[a].path, sizes[k], dims[k])) continue;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < COVER_PSRAM_RESERVE + 60000) break;  // full: the rest on demand
    if (!master && !(master = loadCoverMaster(albums[a]))) return;
    coverCache.push_back({albums[a].path, sizes[k], dims[k], scaleCover(master, sizes[k], dims[k]), millis()});
  }
  delete master;
}

// Decodes any covers that aren't cached yet, before playback starts, so the
// slow part never stutters the music. Usually a no-op.
static void prepareCovers() {
  std::vector<int> todo;
  for (size_t a = 0; a < albums.size(); a++)
    if (hasCover(albums[a]) && !coverCacheCurrent(albums[a])) todo.push_back(a);
  for (size_t i = 0; i < todo.size(); i++) {
    auto& d = M5.Display;
    d.fillRect(0, 176, 320, 46, rgb(18, 22, 48));  // under the splash title
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextColor(TFT_WHITE);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.drawString("Getting album pictures ready", 160, 188);
    d.fillRoundRect(40, 204, 240, 10, 5, rgb(40, 46, 80));
    d.fillRoundRect(40, 204, std::max<int>(10, 240 * (i + 1) / todo.size()), 10, 5, rgb(255, 210, 0));
    delete loadCoverMaster(albums[todo[i]]);
  }
}

static void prepareCarouselArt();  // see "albums"

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
#define C_BG    rgb(18, 22, 48)
#define C_BAR   rgb(10, 12, 30)
#define C_WHITE rgb(255, 255, 255)
#define C_SHADE rgb(0, 0, 0)
#define C_DIM   rgb(120, 130, 170)
#define C_ROW   rgb(40, 46, 80)
#define C_OFF   rgb(50, 56, 92)
#define C_BLUE  rgb(25, 150, 220)
#define C_GREEN rgb(46, 196, 120)
#define C_ORANGE rgb(255, 146, 76)
#define C_YELLOW rgb(255, 210, 0)
#define C_PANEL rgb(28, 33, 66)

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
// Five equal columns, 64 px (8 mm) wide; a tap anywhere in a column, the full height of the
// row, is that button. Quieter, previous, play, next, louder.
static constexpr RoundButton B_QUIET{32, 24}, B_PREV{96, 26}, B_PLAY{160, 34}, B_NEXT{224, 26}, B_LOUD{288, 24};
static constexpr int P_ROW_TOP = P_CTRL_Y - 42, P_ROW_BOTTOM = P_CTRL_Y + 42;
static constexpr int VOL_DOTS_Y = 204, P_LABEL_Y = 205;               // the level under play, labels either side
static constexpr int PB_X = 22, PB_Y = 220, PB_W = 276, PB_H = 14;  // progress: drag or tap to jump
static constexpr int VB_X = 244, VB_Y = 5, VB_W = 68, VB_H = 26;  // the top bar's now-playing button

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
  int h = s * 3 / 5;  // optical centering: shift right a little
  M5.Display.fillTriangle(x - s / 3 + 2, y - h, x - s / 3 + 2, y + h, x + s * 2 / 3 + 2, y, c);
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
  d.drawRoundRect(x, y, 26, 14, 3, C_WHITE);
  d.fillRect(x + 26, y + 4, 3, 6, C_WHITE);
  d.fillRect(x + 2, y + 2, 22, 10, C_BAR);
  d.fillRect(x + 2, y + 2, std::max(2, 22 * constrain(pct, 0, 100) / 100), 10, fill);
  if (usb) {  // lightning bolt
    d.fillTriangle(x + 15, y + 1, x + 9, y + 8, x + 14, y + 8, C_WHITE);
    d.fillTriangle(x + 12, y + 13, x + 18, y + 6, x + 13, y + 6, C_WHITE);
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
    case ShelfKind::Podcasts: return rgb(138, 80, 236);
    case ShelfKind::Books: return C_ORANGE;
    default: return rgb(0, 170, 170);
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
// A flat, white-edged card (Home's shelves).
static void drawCardFrame(int x, int y, int w, int h, uint16_t fill) {
  auto& d = M5.Display;
  d.fillRoundRect(x, y, w, h, 13, C_WHITE);
  d.fillRoundRect(x + 4, y + 4, w - 8, h - 8, 9, fill);
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
    c = rgb((r + 18) / 2, (g + 22) / 2, (b + 48) / 2);
  }
  d.fillRoundRect(x, y, size, size, size / 8, c);
  drawShelfIcon(albumKind(a), x + size / 2, y + size / 2, dim ? rgb(150, 156, 190) : C_WHITE, c, size * 100 / 120);
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
  for (int i = 0; i < 5; i++) d.fillRoundRect(118 + i * 18, VOL_DOTS_Y - 2, 14, 5, 2, i < volStep() ? C_YELLOW : rgb(74, 81, 128));
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
    drawRound(b.x, P_CTRL_Y, b.r, lit ? rgb(120, 230, 170) : C_GREEN);
    if (paused || tracks.empty()) iconPlay(b.x, P_CTRL_Y, b.r * 8 / 9, C_WHITE);
    else iconPause(b.x, P_CTRL_Y, b.r * 8 / 10, C_WHITE);
    return;
  }
  bool vol = i == 0 || i == 4;
  drawRound(b.x, P_CTRL_Y, b.r, lit ? (vol ? rgb(100, 108, 160) : rgb(110, 195, 245)) : (vol ? C_OFF : C_BLUE));
  if (vol) iconSpeakerWaves(b.x, P_CTRL_Y, i == 4 ? 2 : 0, C_WHITE);
  else iconSkip(b.x, P_CTRL_Y, 24, i == 3, C_WHITE);
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
  bool book = isBook(playAlbum), podcast = isPodcastAlbum(playAlbum);
  String big = book ? albums[playAlbum].name : title;
  String second = artist;
  String third = book ? title : podcast ? String() : albums[playAlbum].name;
  if (third == big || third == second) third = "";
  d.setFont(&fonts::FreeSansBold12pt7b);
  auto lines = wrapText(big, tw, 2);
  int y = P_COVER_Y + 10;
  d.setTextDatum(middle_left);
  d.setTextColor(C_WHITE);
  for (auto& l : lines) { d.drawString(l, tx, y); y += 23; }
  d.setFont(&fonts::FreeSansBold9pt7b);
  if (second.length()) { d.setTextColor(C_DIM); d.drawString(fitLine(second, tw), tx, y - 2); y += 19; }
  if (third.length()) {
    d.setTextColor(book ? rgb(226, 210, 250) : rgb(200, 205, 230));
    d.drawString(fitLine(third, tw), tx, y - 2);
  }
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
      d.fillRect(PB_X + (int)(PB_W * at / bookTotal), PB_Y + 3, 2, PB_H - 6, rgb(150, 156, 190));
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
    case Snack::Headphones: bg = rgb(16, 40, 70); edge = C_BLUE; text = "Headphones on"; break;
    case Snack::Speaker: bg = rgb(16, 40, 70); edge = C_BLUE; text = "Playing out loud"; break;
    default: return;
  }
  d.fillRoundRect(SN_X, SN_Y, SN_W, SN_H, 14, edge);
  d.fillRoundRect(SN_X + 3, SN_Y + 3, SN_W - 6, SN_H - 6, 11, bg);
  int cy = SN_Y + SN_H / 2;
  if (snackShown == Snack::Headphones) iconHeadphones(SN_X + 26, cy - 8, C_WHITE);
  else if (snackShown == Snack::Speaker) iconSpeaker(SN_X + 26, cy, C_WHITE);
  else iconBattery(SN_X + 12, cy - 7, snackShown == Snack::Full ? 100 : snackPct, snackShown != Snack::LowBattery);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(middle_left);
  d.setTextColor(C_WHITE);
  d.drawString(text, SN_X + 52, cy + 1);
  if (detail.length()) {
    int tx = SN_X + 52 + d.textWidth(text) + 10;
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextColor(rgb(200, 205, 230));
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

// Battery and charger news for the snackbar, checked every few seconds.
static void watchPower() {
  static uint32_t last = 0;
  static bool first = true, wasUsb = true, fullShown = false;
  static int warned = 101;  // lowest warning given since the last charge
  if (millis() - last < 5000) return;
  last = millis();
  int pct = M5.Power.getBatteryLevel();
  bool usb = usbPower();
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
// A carousel: one big square cover in the middle with the album's name across the screen
// below it, and the albums either side peeking in, dimmed. Swipe for the next / previous
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
  // most recently played first; the rest stay A to Z
  std::stable_sort(shelfAlbums.begin(), shelfAlbums.end(),
                   [](int x, int y) { return recentRank(albums[x].path) < recentRank(albums[y].path); });
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
    d.fillCircle(CV_X + CV_BIG - 16, CV_Y + CV_BIG - 16, 12, C_GREEN);
    iconPlaying(CV_X + CV_BIG - 16, CV_Y + CV_BIG - 16, C_WHITE);
  }
  if (albums[a].newCount > 0) d.fillCircle(CV_X + 14, CV_Y + 14, 7, C_YELLOW);  // new podcast episodes
  // the name across the screen (one size, two lines at most), the artist under it
  d.setFont(&fonts::FreeSansBold12pt7b);
  auto lines = wrapText(albums[a].name, 296, 2);
  d.setTextDatum(middle_center);
  d.setTextColor(C_WHITE);
  int y = lines.size() > 1 ? CV_NAME_Y - 5 : CV_NAME_Y;
  for (auto& l : lines) { d.drawString(l, 160, y); y += 23; }
  const String& who = albums[a].albumArtist;
  if (who.length() && who != albums[a].name) {
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextColor(C_DIM);
    d.drawString(fitLine(who, 296), 160, y - 2);
  }
}

static void drawAlbums() {
  auto& d = M5.Display;
  d.fillScreen(C_BG);
  drawNavBar(1);
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

// Top right on every screen: three sound bars, bouncing on green while something plays,
// still and grey when it's paused (or nothing is loaded). Tap it for the player.
static void drawNowPlayingButton(bool bounce = false) {  // bounce: new random bar heights
  static uint8_t heights[3] = {9, 15, 12};
  static uint32_t seed = 88675123;  // xorshift; libc rand() costs IRAM
  auto& d = M5.Display;
  bool on = musicIsPlaying();
  d.fillRoundRect(VB_X, VB_Y, VB_W, VB_H, VB_H / 2, on ? C_GREEN : C_OFF);
  int cx = VB_X + VB_W / 2, cy = VB_Y + VB_H / 2;
  if (bounce && on)
    for (auto& h : heights) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; h = 5 + seed % 13; }
  uint16_t ink = on ? C_WHITE : rgb(150, 156, 190);
  for (int i = 0; i < 3; i++) d.fillRoundRect(cx - 11 + i * 8, cy + 8 - heights[i], 5, heights[i], 2, ink);
}

// Back on the left (Home's house instead), the shelf's name, and top right the
// now-playing button.
static void drawNavBar(int level) {  // 0 Home, 1 a shelf's albums, 2 an album's songs, 3 the player
  auto& d = M5.Display;
  uint16_t bar = rgb(14, 17, 40);
  d.fillRect(0, 0, 320, NAV_H, bar);
  if (level == 0) {
    drawHouse(NB_X, NAV_CY, C_WHITE, bar);
  } else {
    drawRound(NB_X, NAV_CY - 1, NB_R, C_OFF);
    iconArrow(NB_X - 1, NAV_CY - 1, 0, C_WHITE);
    int shelf = level == 1 ? viewShelf : level == 2 ? (viewAlbum >= 0 ? albums[viewAlbum].shelf : -1)
                                                    : (playAlbum >= 0 ? albums[playAlbum].shelf : -1);
    if (shelf >= 0 && shelf < (int)shelves.size()) {
      d.setFont(&fonts::FreeSansBold12pt7b);
      d.setTextDatum(middle_left);
      d.setTextColor(C_WHITE);
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
// A list of wide cards, one per shelf (Podcasts, Music, Books, then any other), above
// each with the shelf's picture and name, and on the right how many podcast
// episodes are new.
static constexpr int SH_TOP = NAV_H + 6, SH_BOTTOM = 234, SH_GAP = 6;

static void shelfTileRect(int i, int& x, int& y, int& w, int& h) {
  int n = std::max(1, std::min((int)shelves.size(), 6));
  h = std::min(62, (SH_BOTTOM - SH_TOP - (n - 1) * SH_GAP) / n);
  w = 304;
  x = 8;
  y = SH_TOP + i * (h + SH_GAP);
}

static void drawShelves() {
  auto& d = M5.Display;
  d.fillScreen(C_BG);
  drawNavBar(0);
  for (int i = 0; i < (int)shelves.size() && i < 6; i++) {
    audioPump();
    const Shelf& sh = shelves[i];
    int x, y, w, h;
    shelfTileRect(i, x, y, w, h);
    uint16_t c = shelfColor(sh);
    drawCardFrame(x, y, w, h, c);
    int cy = y + h / 2 + 1, tx = x + 20;
    if (h >= 46) {  // room for the picture, with space around it (lots of shelves: just names)
      drawShelfIcon(sh.kind, x + 46, cy, C_WHITE, c, 66);
      tx = x + 86;
    }
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(middle_left);
    d.setTextColor(C_WHITE);
    d.drawString(fitLine(sh.name, 150), tx, cy);
    if (sh.newCount > 0) {  // new podcast episodes
      String n = String(sh.newCount) + " new";
      d.setFont(&fonts::FreeSansBold9pt7b);
      int bw = d.textWidth(n) + 16;
      d.fillRoundRect(x + w - bw - 12, cy - 11, bw, 22, 11, C_YELLOW);
      d.setTextDatum(middle_center);
      d.setTextColor(C_SHADE);
      d.drawString(n, x + w - 12 - bw / 2, cy + 1);
    }
  }
}

static void handleShelves(const m5::touch_detail_t& t) {
  if (!t.wasClicked()) return;
  for (int i = 0; i < (int)shelves.size() && i < 6; i++) {
    int x, y, w, h;
    shelfTileRect(i, x, y, w, h);
    if (t.x >= x - 3 && t.x < x + w + 3 && t.y >= y - 3 && t.y < y + h + 3) {
      buzz();
      openShelf(i, playAlbum >= 0 && albums[playAlbum].shelf == i ? playAlbum : -1);
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
    uint16_t c = playing ? C_WHITE : C_GREEN;
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
      d.fillRoundRect(tlX() + 4, y + 4, 40, h - 8, 8, playing ? C_WHITE : C_OFF);
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
      d.setTextColor(C_WHITE);
      for (size_t k = 0; k < lines.size(); k++) d.drawString(lines[k], tx, ty + k * lh);
      drawProgressMark(i, tx, tlX() + tlW() - 8, y, h, playing);
      continue;
    }
    int nx = tlX() + 17;
    d.fillCircle(nx, my, 12, playing ? C_WHITE : C_OFF);
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
    d.setTextColor(C_WHITE);
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

// ================= Grown-ups (hold two of the touch buttons for 3 s) =================
// Header with Done, then three tabs: Settings, Bluetooth (WiFi in WiFi mode), Stats.
static int guTab = 0, statsPage = 0;
static Screen beforeGrownups = Screen::Player;
static int brightIdx = 1, autoOffIdx = 0;        // saved in prefs
static const uint8_t BRIGHTNESS[3] = {60, 140, 230};
static const uint16_t AUTO_OFF_MIN[3] = {15, 30, 0};  // 0 = never

static constexpr int GU_TABS_Y = 40, GU_TAB_H = 28, GU_BODY_Y = 76;
static constexpr int SEG_X = 118, SEG_W = 194, SEG_H = 26;  // settings: 3-way choices on the right
static constexpr int GU_ROWS = 3, STATS_PAGES = 3;
static const int GU_ROW_Y[GU_ROWS] = {80, 112, 144};
static constexpr int GU_MODE_Y = 206;  // full-width mode switch button

static void drawSegments(int x, int y, int w, int h, const char* const* labels, int n, int sel, uint16_t on) {
  auto& d = M5.Display;
  d.fillRoundRect(x, y, w, h, h / 2, C_ROW);
  int sw = w / n;
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  for (int i = 0; i < n; i++) {
    if (i == sel) d.fillRoundRect(x + i * sw + 2, y + 2, sw - 4, h - 4, (h - 4) / 2, on);
    d.setTextColor(i == sel ? C_WHITE : C_DIM);
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
    drawStatTile(0, 2, "FIRMWARE SIZE", fixed(ESP.getSketchSize() / 100000, 10, 1) + " MB", C_DIM);
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
    d.setTextColor(C_DIM);
    d.drawString(fitLine(owner.length() ? "Owner: " + owner + " (set on the web page)" : String("Name the owner on the web page"), 300), 10, 186);
    d.fillRoundRect(6, GU_MODE_Y, 308, 26, 13, rgb(60, 66, 110));
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
    d.fillRoundRect(6, GU_MODE_Y, 308, 28, 14, rgb(60, 66, 110));
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
  d.fillRoundRect(236, 6, 76, 27, 13, C_GREEN);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  d.drawString("Done", 274, 20);
  static const char* const tabs[] = {"Settings", CORE2_WIFI ? "WiFi" : "Bluetooth", "Stats"};
  drawSegments(6, GU_TABS_Y, 308, GU_TAB_H, tabs, 3, guTab, rgb(138, 80, 236));
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
    } else if (screen == Screen::Shelves || screen == Screen::Albums) {
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
  int i = constrain(t.x / 64, 0, 4);  // the column it landed in
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
//   NAME [<name>]        -> sets (or, empty, clears) the owner's name, "OK"
//   LOAD <album>         -> loads the first album whose name contains <album> (paused), "OK"
//   SHOW <screen>        -> home | albums [<shelf>] | songs | player | grownups, "OK"
//   SHOT                 -> "SHOT 320 240", the screen as raw RGB565 rows, then "END"
//                           (LOAD, SHOW and SHOT are for screenshot.py)
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
        if (sh.length() && lowerStr(shelves[i].name) == sh) k = i;
      openShelf(k, playAlbum);
    } else if (what == "songs" && playAlbum >= 0) {
      shrinkFromCarousel = screen == Screen::Albums;  // from the carousel: with its animation, like a tap
      openTracks(playAlbum);
    }
    else if (what == "player") showScreen(Screen::Player);
    else if (what == "grownups") { guTab = 0; beforeGrownups = screen; showScreen(Screen::Grownups); }
    else { Serial.println("ERR bad screen"); return; }
    Serial.println("OK");
  } else if (line == "SHOT") {  // what's on the screen, read back from the display
    static uint16_t row[320];
    Serial.println("SHOT 320 240");
    for (int y = 0; y < 240; y++) {
      M5.Display.readRect(0, y, 320, 1, row);
      Serial.write((const uint8_t*)row, sizeof row);
    }
    Serial.println("END");
  } else if (line == "NAME" || line.startsWith("NAME ")) {
    setOwner(line.substring(4));
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
  bool nightBoot = prefs.getUChar("nightboot", 0);  // a quiet reboot for the nightly podcast sync
  M5.Display.setBrightness(nightBoot ? 0 : 140);   // the saved brightness is applied below
  M5.Display.fillScreen(C_BG);
  volLevel = prefs.getUChar("lvl", 6);
  maxVolPct = prefs.getUChar("maxvol", 70);
  owner = prefs.getString("owner", "");
  brightIdx = constrain((int)prefs.getUChar("bright", 1), 0, 2);
  autoOffIdx = constrain((int)prefs.getUChar("autooff", 0), 0, 2);
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
    if (btConnected) M5.Speaker.stop(SPK_CH);
    else btHasTarget = false;
    applyVolume();
    showSnack(btConnected ? Snack::Headphones : Snack::Speaker);
    connectDirty = true;
  }
#endif

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
