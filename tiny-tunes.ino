// Tiny Tunes — a kids' audio player, bedside companion and learning games for the M5Stack Core2.
//
// Put .mp3 files on a FAT32 microSD card (root or one folder deep).
//
// This sketch builds two firmwares that live side by side in the two app slots:
//   Bluetooth mode (default build): plays to paired BT headphones/speakers, or
//     the built-in speaker when nothing is connected.
//   WiFi mode (-DCORE2_WIFI=1): built-in speaker, NTP clock sync, and a web page
//     for uploading music and remote control.
// The precompiled ESP32 core can't fit BT Classic and WiFi in IRAM together, so
// switching modes reboots into the other slot (~2 s). SD card and settings are shared.
// Build/flash both with ./flash.sh.
//
// Kid-friendly controls:
//   Big buttons: volume down, previous, play/pause, next, volume up
//   The three touch zones above the screen are places: Listen / Play / Sleep
//     Listen again on the player = play/pause; Play = the game picker;
//     Sleep again = start or extend the sleep timer
//   Tap the song card: album covers (swipe up/down to page)
//   Swipe right on the album covers or a song list: back
//   Swipe the song card left/right: next / previous song
//   Hold the top of the screen for 3 s: grown-ups screen (settings, Bluetooth/WiFi, stats)
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
#else
#include <BluetoothA2DPSource.h>
#endif

// Declared up here so Arduino's auto-generated prototypes can see it.
enum class Screen { Player, Albums, Tracks, Grownups, Spell, Math, Clock, Words, Pattern, Simon, Memory, Maze, Moles, PlayHome, Sleep };
struct RoundButton { int x, r; };
// A quiz game's progress. Levels are played in rounds of 5 questions: 4+ right
// on the first try levels up, 2 or fewer drops a level, 3 plays the level again.
struct Quiz {
  const char* key;  // prefs key prefix
  int maxLevel;
  bool stacked = false;  // answers as three full-width rows (words) instead of bubbles
  int level = 1, stars = 0;
  uint8_t round[5] = {0, 0, 0, 0, 0};  // this round so far: 0 not yet, 1 first try, 2 needed retries
  int roundDone = 0;
  String choices[3];
  int answer = 0;  // index into choices
  bool wrong[3] = {false, false, false};
  int tries = 0;
  bool solved = false, started = false;
  uint32_t nextAt = 0;
};
struct Album { String name, path, coverFile; int8_t artistPrefixed = -1; };  // coverFile "" = none; -1 = not yet known

// Screen upside down (USB port on top). The three touch buttons are detected in raw
// panel coordinates, so after rotating they sit above the screen in C, B, A order:
// BTN_LEFT/BTN_RIGHT keep "left" meaning back/previous from the viewer's side.
static constexpr int ROTATION = 3;  // 1 = normal landscape
#define BTN_LEFT  (ROTATION == 3 ? M5.BtnC : M5.BtnA)
#define BTN_RIGHT (ROTATION == 3 ? M5.BtnA : M5.BtnC)

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
static BluetoothA2DPSource a2dp;

// ---------- ring buffer: decoder (loop task) -> A2DP callback (BT task) ----------
// Single producer / single consumer, so atomics on head/tail are enough.
static constexpr uint32_t RING_LEN = 16384;  // frames, ~370 ms at 44.1 kHz
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
  // The MP3 decoder announces its rate only when it changes, so a calm sound
  // (Sleep) overrides the rate instead of replacing it; clearing the override
  // puts the decoder's rate back however the music restarts.
  bool SetRate(int hz) override { _decoderRate = hz; applyRate(); return true; }
  void overrideRate(int hz) { _override = hz; applyRate(); }
  bool ConsumeSample(int16_t s[2]) override {
#if !CORE2_WIFI
    if (btConnected) return consumeBT(s);
#endif
    return consumeSpeaker(s);
  }
  void flush() override { flushSpeaker(); }
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
    hertz = _override ? _override : _decoderRate;
#if !CORE2_WIFI
    _step = (float)hertz / BT_RATE;
#endif
  }
  int _decoderRate = 44100, _override = 0;

  static constexpr size_t BUF_LEN = 4096;  // ~140 ms queued, so slow redraws don't stutter
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
static int playAlbum = -1;          // album whose tracks are in `tracks`
static std::vector<String> tracks;  // tracks of the playing album
static int current = -1;
static bool paused = false;
static uint8_t volume = 0;      // actual output volume 0..255
static int volLevel = 6;        // what the kid sees: 0..10
static int maxVolPct = 70;      // grown-up volume limit, percent
static String title;
static bool dirtyAll = true, dirtyNow = true, dirtyHeader = true;
static volatile bool connectDirty = false;  // grown-ups screen needs redraw
static Screen screen = Screen::Player;  // which screen is showing
static int songCountCache = -1;  // stats page; reset when the library changes
static uint32_t listenSec = 0, songsPlayed = 0;  // stats page; saved in prefs
static int sleepSound = 0;      // Sleep place: 0 quiet, 1 rain, 2 ocean
static bool nightSync = false;  // this WiFi boot is the nightly podcast sync (see "nightly podcast sync")
static void nightSyncDone();

static void stopPlayback();
static void playTrack(int i);
static void playAlbumTrack(int album, int track);
static void rescanLibrary();
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
static String joinPath(const String& dir, const char* name);
static bool isMp3(const char* name);
static int findAlbum(const String& path);
static bool jpegSize(const String& path, int& w, int& h);
static bool removePath(const String& path);
static uint32_t fileSize(const String& path);
static constexpr const char* COVER_CACHE = ".cover.rgb565";  // decoded cover, see "album covers"

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
  bool connect = btHasTarget && !memcmp(addr, btTarget, ESP_BD_ADDR_LEN);
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

static void wifiStart() {
  if (!prefs.isKey("ssid")) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(prefs.getString("ssid").c_str(), prefs.getString("pass").c_str());
  sntp_set_time_sync_notification_cb([](timeval*) { ntpSynced = true; });
  configTzTime(prefs.getString("tz", DEFAULT_TZ).c_str(), "pool.ntp.org", "time.google.com");
}

static uint32_t wifiUpAt = 0;  // when WiFi connected (podcast sync waits ~30 s after)
static void podcastLoop();
static void wifiLoop() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !webUp) {
    web.begin();
    webUp = true;
    Serial.printf("IP %s\n", WiFi.localIP().toString().c_str());
    dirtyHeader = true;
    connectDirty = true;
  } else if (!up && webUp) {
    web.stop();
    webUp = false;
    dirtyHeader = true;
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
    dirtyHeader = true;
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
</style></head><body><h1>Tiny Tunes</h1>
<div id="now" class="dim">...</div>
<div class="ctl"><button onclick="cmd('prev')">&#9198;</button><button onclick="cmd('play')">&#9199;</button>
<button onclick="cmd('next')">&#9197;</button></div>
<div class="ctl"><button onclick="cmd('voldown')">Vol &minus;</button><button onclick="cmd('volup')">Vol +</button></div>

<h2>Add music</h2>
<input type="text" id="album" list="albumlist" placeholder="Album name, e.g. Trolls (blank = loose songs)"><datalist id="albumlist"></datalist>
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
<div class="ctl"><button onclick="syncNow()">&#8635; Sync now</button></div>
<div id="syncline" class="dim"></div><progress id="syncprog" value="0" max="100" hidden></progress>
<div id="pods"></div>

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

async function refreshNow(){const s=await (await fetch('/state')).json();
 if(s.version)$('ver').textContent='Tiny Tunes '+s.version;
 $('now').textContent=s.tracks.length?(s.paused?'Paused: ':'Playing: ')+(s.tracks[s.current]||'')+' — '+s.album+'  (vol '+s.volume+'%)':'Nothing playing';
 return s}
async function cmd(c){await post('/cmd?c='+c);refreshNow();if(/album|track/.test(c))setTimeout(loadLib,300)}

// ---------- library / organize ----------
async function loadLib(){
 lib=await (await fetch('/library')).json();const s=await refreshNow();
 const open=new Set([...document.querySelectorAll('#lib details[open]')].map(d=>d.dataset.path));
 $('albumlist').innerHTML=lib.albums.filter(a=>a.folder!=lib.root).map(a=>`<option value="${esc(a.folder.split('/').pop())}">`).join('');
 $('lib').innerHTML=lib.albums.length?'':'No albums yet.';
 lib.albums.forEach((a,ai)=>{
  const d=document.createElement('details');d.dataset.path=a.folder;if(open.has(a.folder))d.open=true;
  if(ai==lib.playing)d.className='cur';
  const cover=a.cover?`<img src="/file?path=${q(join(a.folder,a.cover))}&v=${a.coverSize}">`:'<span class="nocover"></span>';
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
 try{await post('/mkdir?path='+q(join(lib.root,safe(n))));$('album').value=safe(n);say(`Created ${safe(n)}. Drop songs above to fill it.`);loadLib()}catch(e){alert(e.message)}}
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
async function upload(files){const name=safe($('album').value);files=[...files];const dir=name?join(lib.root,name):lib.root;
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
  if(a.folder.startsWith('/podcasts/'))continue;  // managed by the podcast sync
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
  if(!a.cover&&a.tracks.length)plan.push({on:true,label:`Make a cover for <b>${esc(a.name)}</b> from the art inside its songs`,
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
 $('syncprog').hidden=!(busy&&p.pct>=0);$('syncprog').value=Math.max(p.pct,0);
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

// Rescans, then keeps the same album and song loaded (paused) so organizing
// doesn't jump the kid's player back to the first album.
static void rescanKeepingPlace() {
  String path = playAlbum >= 0 ? albums[playAlbum].path : "";
  int track = std::max(current, 0);
  rescanLibrary();
  int a = findAlbum(path);
  if (a < 0 && !albums.empty()) { a = 0; track = 0; }
  if (a >= 0) {
    playAlbum = a;
    tracks = listTracks(a);
    current = tracks.empty() ? -1 : std::min<int>(track, tracks.size() - 1);
    if (current >= 0) title = trackTitle(tracks[current], a);
    paused = true;
  }
  if (screen == Screen::Albums || screen == Screen::Tracks) screen = Screen::Player;  // their indexes are stale now
  dirtyAll = true;
}

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
  if (tag.startsWith("![CDATA[")) { rss.text += tag.substring(8, tag.length() - 2); return; }
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
    bool cdata = rss.tag.startsWith("![CDATA[");
    if (c == '>' && (!cdata || rss.tag.endsWith("]]"))) { rssTag(rss.tag); rss.tag = ""; rss.inTag = false; }
    else if (rss.tag.length() < 1500) rss.tag += c;
    return;
  }
  if (c == '<') { rss.inTag = true; rss.tag = ""; return; }
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
  if (syncChanged) rescanPending = true;  // rescanning stops playback, so not mid-song
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
  for (auto& p : drop) { SD.remove(p); syncChanged = true; }
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

// Called from wifiLoop() while connected.
static void podcastLoop() {
  uint32_t now = millis();
  if (!wifiUpAt) wifiUpAt = now;
  if (syncState == SyncState::Idle) {
    if (rescanPending && !musicIsPlaying()) { rescanPending = false; rescanKeepingPlace(); }
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
    if (web.arg("files") == "1") { stopPlayback(); removePath(showFolder(feeds[i], i)); }
    feeds.erase(feeds.begin() + i);
    saveFeeds();
    rescanKeepingPlace();
    web.send(200, "text/plain", "OK");
  });
  web.on("/podcast/sync", HTTP_POST, [] { requestSync(); web.send(200, "text/plain", "OK"); });
}

static void setupWeb() {
  setupPodcastWeb();
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html", INDEX_HTML); });
  web.on("/state", HTTP_GET, [] {
    String albumName = playAlbum >= 0 ? albums[playAlbum].name : "";
    String j = "{\"current\":" + String(current) + ",\"paused\":" + (paused ? "true" : "false") +
               ",\"volume\":" + String(volLevel * 10) + ",\"version\":\"" TT_VERSION "\",\"album\":\"" + jsonEscape(albumName) +
               "\",\"tracks\":[";
    for (size_t i = 0; i < tracks.size(); i++) {
      if (i) j += ',';
      j += "\"" + jsonEscape(trackTitle(tracks[i], playAlbum)) + "\"";
    }
    web.send(200, "application/json", j + "]}");
  });
  // Everything the organize/cleanup UI needs: albums, songs with display titles, cover status, stray files.
  web.on("/library", HTTP_GET, [] {
    String j = "{\"root\":\"" + jsonEscape(libraryRoot()) + "\",\"playing\":" + String(playAlbum) + ",\"albums\":[";
    for (size_t a = 0; a < albums.size(); a++) {
      std::vector<String> list = (int)a == playAlbum ? tracks : listTracks(a);
      const String& cf = albums[a].coverFile;
      uint32_t coverSize = cf.length() ? fileSize(joinPath(albums[a].path, cf.c_str())) : 0;
      j += (a ? ",{" : "{");
      j += "\"name\":\"" + jsonEscape(albums[a].name) + "\",\"folder\":\"" + jsonEscape(albums[a].path) +
           "\",\"cover\":\"" + jsonEscape(cf) + "\",\"coverSize\":" + String(coverSize) + ",\"tracks\":[";
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
        if (!f.isDirectory() && !isMp3(n) && albums[a].coverFile != n && strcmp(n, COVER_CACHE) &&
            !(albums[a].path == "/" && n[0] == '.')) {
          j += (first ? "\"" : ",\"") + jsonEscape(n) + "\"";
          first = false;
        }
        f.close();
      }
      dir.close();
      j += "]}";
    }
    web.send(200, "application/json", j + "]}");
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
    }
    f.close();
  });
  web.on("/mv", HTTP_POST, [] {
    String from = web.arg("from"), to = web.arg("to");
    if (!okPath(from) || !okPath(to) || !SD.exists(from)) { web.send(400, "text/plain", "bad path"); return; }
    if (SD.exists(to)) { web.send(409, "text/plain", "something with that name already exists"); return; }
    stopPlayback();
    int slash = to.lastIndexOf('/');
    if (slash > 0) SD.mkdir(to.substring(0, slash));
    if (SD.rename(from, to)) web.send(200, "text/plain", "OK");
    else web.send(500, "text/plain", "rename failed");
  });
  web.on("/rm", HTTP_POST, [] {
    String path = web.arg("path");
    if (!okPath(path) || path == "/music") { web.send(400, "text/plain", "bad path"); return; }
    stopPlayback();
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
  web.on("/rescan", HTTP_POST, [] {
    rescanKeepingPlace();
    web.send(200, "text/plain", "OK");
  });
  // ?dir=/full/folder (web page) or ?album=Name (upload.py; relative to the library root)
  web.on("/upload", HTTP_POST,
         [] { web.send(webPath.length() ? 200 : 500, "text/plain", webPath.length() ? "OK" : "write failed"); },
         [] {
           HTTPUpload& up = web.upload();
           if (up.status == UPLOAD_FILE_START) {
             stopPlayback();
             paused = true;
             String dir = web.hasArg("dir") ? web.arg("dir") : web.arg("album").length() ? joinPath(libraryRoot(), safeName(web.arg("album")).c_str()) : libraryRoot();
             if (dir != "/" && !okPath(dir)) { webPath = ""; return; }
             if (dir != "/") SD.mkdir(dir);
             webPath = joinPath(dir, safeName(up.filename.substring(up.filename.lastIndexOf('/') + 1)).c_str());
             webFile = SD.open(webPath, FILE_WRITE);
             if (!webFile) webPath = "";
           } else if (up.status == UPLOAD_FILE_WRITE && webFile) {
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
    if (!f.isDirectory() && isMp3(f.name())) {
      found.push_back(joinPath(albums[a].path, f.name()));
      if (strstr(f.name(), " - ")) dashed++;
    }
    f.close();
  }
  dir.close();
  std::sort(found.begin(), found.end(), lessCI);
  if (albums[a].path.startsWith("/podcasts/")) std::reverse(found.begin(), found.end());  // date-named: newest first
  albums[a].artistPrefixed = !found.empty() && dashed * 10 >= (int)found.size() * 6;
  return found;
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

// Adds `dirPath` as an album if it holds MP3s, then looks `depth` levels deeper.
// Handles both "/Artist - Album/" and "/music/Artist - Album/" layouts.
static void scanAlbums(const String& dirPath, const char* name, int depth) {
  bool hasMp3 = false;
  String cover;
  int coverRank = 99;
  std::vector<String> subdirs;
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) return;
  while (File f = dir.openNextFile()) {
    const char* n = f.name();
    if (f.isDirectory()) { if (depth > 0 && !isHidden(n)) subdirs.push_back(n); }
    else if (isMp3(n)) hasMp3 = true;
    else if (int r = coverRank_(n); r < coverRank) { coverRank = r; cover = n; }
    f.close();
  }
  dir.close();
  if (hasMp3) {
    // loose songs in the card root or a "music" folder show up as "Songs"
    bool loose = dirPath == "/" || !strcasecmp(name, "music");
    albums.push_back({loose ? String("Songs") : prettyAlbumName(name), dirPath, cover});
  }
  for (auto& sub : subdirs) scanAlbums(joinPath(dirPath, sub.c_str()), sub.c_str(), depth - 1);
}

static void clearCoverCache();
static void rescanLibrary() {
  stopPlayback();
  albums.clear();
  scanAlbums("/", "", 2);
  std::sort(albums.begin(), albums.end(), [](const Album& a, const Album& b) {
    bool la = a.name == "Songs", lb = b.name == "Songs";  // loose songs go last
    return la != lb ? lb : lessCI(a.name, b.name);
  });
  playAlbum = -1;
  tracks.clear();
  current = -1;
  clearCoverCache();
  songCountCache = -1;
}

static int findAlbum(const String& path) {
  for (size_t i = 0; i < albums.size(); i++)
    if (albums[i].path == path) return i;
  return -1;
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

static bool isPodcastAlbum(int a) { return a >= 0 && a < (int)albums.size() && albums[a].path.startsWith("/podcasts/"); }

// "Wow in the World: The Moon" in the "Wow in the World" show -> "The Moon"
static String dropShowName(String t, const String& show) {
  if (show.length() < 3 || t.length() <= show.length() + 2 || !startsNoCase(t.c_str(), show.c_str(), show.length())) return t;
  int i = show.length();
  while (i < (int)t.length() && strchr(" :-|", t[i])) i++;
  return i < (int)t.length() ? t.substring(i) : t;
}

static String trackTitle(const String& path, int a) {
  String n = path.substring(path.lastIndexOf('/') + 1);
  if (n.length() > 4) n.remove(n.length() - 4);
  String t = cleanTitle(n, true, a >= 0 && a < (int)albums.size() && albums[a].artistPrefixed == 1);
  return isPodcastAlbum(a) ? dropShowName(t, albums[a].name) : t;
}

static String albumName() { return playAlbum >= 0 ? albums[playAlbum].name : String(); }

// ---------- playback ----------
static uint32_t be32(const uint8_t* b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static uint32_t synchsafe(const uint8_t* b) { return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }

// Reads the ID3v2 title and leaves `f` at the start of the audio. ESP8266Audio's
// AudioFileSourceID3 reads the entire tag a byte at a time, which took seconds on
// songs with embedded cover art; this seeks past every frame except the title.
static String readId3Title(AudioFileSourceSD* f) {
  uint8_t h[10];
  if (f->read(h, 10) != 10 || memcmp(h, "ID3", 3) || h[3] < 3 || h[3] > 4) {
    f->seek(0, SEEK_SET);
    return "";
  }
  uint32_t tagEnd = 10 + synchsafe(h + 6) + ((h[5] & 0x10) ? 10 : 0);  // + footer
  String title;
  if (!(h[5] & 0xC0)) {  // skip frame parsing for unsynchronised tags / extended headers (rare)
    for (uint32_t pos = 10; pos + 10 <= tagEnd && !title.length();) {
      uint8_t fh[10];
      if (f->read(fh, 10) != 10 || !fh[0]) break;  // end, or padding
      uint32_t size = h[3] == 4 ? synchsafe(fh + 4) : be32(fh + 4);
      if (!memcmp(fh, "TIT2", 4) && size > 1 && size < 256) {
        uint8_t v[256];
        if (f->read(v, size) != size) break;
        if (v[0] == 0) {  // Latin-1 -> UTF-8
          for (uint32_t i = 1; i < size && v[i]; i++) {
            if (v[i] < 0x80) title += (char)v[i];
            else { title += (char)(0xC0 | v[i] >> 6); title += (char)(0x80 | (v[i] & 0x3F)); }
          }
        } else if (v[0] == 3) {  // UTF-8
          v[size < 256 ? size : 255] = 0;
          title = (const char*)v + 1;
        }  // UTF-16 titles fall back to the filename
      }
      pos += 10 + size;
      f->seek(pos, SEEK_SET);
    }
  }
  f->seek(tagEnd, SEEK_SET);
  return title;
}

static void stopPlayback() {
  if (mp3 && mp3->isRunning()) mp3->stop();
  delete file; file = nullptr;
}

static void playTrack(int i) {
  if (tracks.empty()) return;
  i = (i + tracks.size()) % tracks.size();  // stays inside the album, wrapping around
  stopPlayback();
  sleepSound = 0;  // music replaces any calm sound
  out.overrideRate(0);
  current = i;
  paused = false;
  file = new AudioFileSourceSD(tracks[i].c_str());
  String tagTitle = readId3Title(file);
  title = tagTitle.length() ? cleanTitle(tagTitle, false, false) : trackTitle(tracks[i], playAlbum);
  if (isPodcastAlbum(playAlbum)) title = dropShowName(title, albums[playAlbum].name);
  mp3->begin(file, &out);
  dirtyNow = dirtyHeader = true;
  songsPlayed++;
  prefs.putUInt("played", songsPlayed);
  prefs.putString("album", albums[playAlbum].path);  // power-on picks up here
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
  if (!paused && sleepSound) {  // music takes the output back from a calm sound
    sleepSound = 0;
    out.overrideRate(0);
  }
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

static void showVolumeOverlay();
static void setVolLevel(int lvl) {
  volLevel = constrain(lvl, 0, 10);
  applyVolume();
  showVolumeOverlay();
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
static constexpr int COVER_MASTER = 160;
static constexpr size_t COVER_BYTES = COVER_MASTER * COVER_MASTER * 2;
struct CoverCacheHeader { char magic[4]; uint32_t srcSize; };

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

static bool coverCacheCurrent(const Album& al) {
  return al.coverFile.length() && readCoverCache(al, fileSize(joinPath(al.path, al.coverFile.c_str())), nullptr);
}

static M5Canvas* loadCoverMaster(const Album& al) {
  if (!al.coverFile.length()) return nullptr;
  String src = joinPath(al.path, al.coverFile.c_str());
  uint32_t srcSize = fileSize(src);
  M5Canvas* c = newCanvas(COVER_MASTER);
  if (!c) return nullptr;
  if (readCoverCache(al, srcSize, c)) return c;

  String lower = lowerStr(al.coverFile);
  bool png = lower.endsWith(".png");
  int w = 0, h = 0;
  bool ok = png ? pngSize(src, w, h) : jpegSize(src, w, h);
  if (ok) {
    c->fillSprite(TFT_BLACK);
    float sc = (float)COVER_MASTER / std::min(w, h);  // fill the square, center-crop the rest
    int offX = (w * sc - COVER_MASTER) / 2, offY = (h * sc - COVER_MASTER) / 2;
    ok = png ? c->drawPngFile(SD, src.c_str(), 0, 0, COVER_MASTER, COVER_MASTER, offX, offY, sc, sc)
             : c->drawJpgFile(SD, src.c_str(), 0, 0, COVER_MASTER, COVER_MASTER, offX, offY, sc, sc);
  }
  if (!ok) { delete c; return nullptr; }
  File out = SD.open(joinPath(al.path, COVER_CACHE), FILE_WRITE);
  if (out) {
    CoverCacheHeader hdr{{'C', '2', 'C', '1'}, srcSize};
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

struct CoverEntry { String path; int size; M5Canvas* canvas; uint32_t used; };
static std::vector<CoverEntry> coverCache;

static void clearCoverCache() {
  for (auto& e : coverCache) delete e.canvas;
  coverCache.clear();
}

static M5Canvas* getCover(int a, int size) {
  if (a < 0 || a >= (int)albums.size() || !albums[a].coverFile.length()) return nullptr;
  const String& path = albums[a].path;
  for (auto& e : coverCache)
    if (e.size == size && e.path == path) { e.used = millis(); return e.canvas; }
  M5Canvas* master = size == COVER_MASTER ? nullptr : getCover(a, COVER_MASTER);
  if (coverCache.size() >= 24) {  // evict least recently used
    auto lru = std::min_element(coverCache.begin(), coverCache.end(),
                                [](const CoverEntry& x, const CoverEntry& y) { return x.used < y.used; });
    if (lru->canvas == master) master = nullptr;  // don't free what we're about to scale from
    else { delete lru->canvas; coverCache.erase(lru); }
  }
  M5Canvas* c = nullptr;
  if (size == COVER_MASTER) c = loadCoverMaster(albums[a]);
  else if (master && (c = newCanvas(size))) {
    master->pushRotateZoomWithAA(c, size / 2.0f - 0.5f, size / 2.0f - 0.5f, 0, (float)size / COVER_MASTER, (float)size / COVER_MASTER);
    roundCorners(c, size / 8);
  }
  coverCache.push_back({path, size, c, millis()});  // a failed decode is cached as null too
  return c;
}

// Decodes any covers that aren't cached yet, before playback starts, so the
// slow part never stutters the music. Usually a no-op.
static void prepareCovers() {
  std::vector<int> todo;
  for (size_t a = 0; a < albums.size(); a++)
    if (albums[a].coverFile.length() && !coverCacheCurrent(albums[a])) todo.push_back(a);
  for (size_t i = 0; i < todo.size(); i++) {
    auto& d = M5.Display;
    d.fillRect(0, 90, 320, 70, rgb(18, 22, 48));
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextColor(TFT_WHITE);
    d.setTextDatum(middle_center);
    d.drawString("Getting album pictures ready", 160, 108);
    d.fillRoundRect(40, 132, 240, 12, 6, rgb(40, 46, 80));
    d.fillRoundRect(40, 132, std::max<int>(12, 240 * (i + 1) / todo.size()), 12, 6, rgb(255, 210, 0));
    delete loadCoverMaster(albums[todo[i]]);
  }
}

// ================= kid-friendly UI =================
// Player:   big colorful card (cover, song, album) with chunky picture buttons.
//           Tap the card for albums; swipe it left/right for next/previous song.
// Albums:   2x2 tiles with covers; swipe or arrows to page; tap to see its songs.
// Tracks:   the album's songs as big rows; tap one to play.
// Grown-ups (hold the top bar for 3 s): Bluetooth pairing, volume limit, mode switch.


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

static uint16_t albumColor(int i) {
  static const uint8_t pal[][3] = {
      {255, 89, 94}, {255, 146, 76}, {46, 196, 120}, {25, 150, 220},
      {138, 80, 236}, {255, 70, 160}, {0, 170, 170}, {230, 160, 0}};
  auto& c = pal[(i < 0 ? 0 : i) % 8];
  return rgb(c[0], c[1], c[2]);
}

static constexpr int TOP_H = 22;
static constexpr int CARD_X = 6, CARD_Y = 26, CARD_W = 308, CARD_H = 124, COVER = 108;
static constexpr int PROG_Y = 158;
static constexpr int BTN_Y = 204;
static constexpr RoundButton B_VOLDN{32, 25}, B_PREV{96, 27}, B_PLAY{160, 32}, B_NEXT{224, 27}, B_VOLUP{288, 25};

static uint32_t lastTouchMs = 0, overlayUntil = 0;
static bool dimmed = false;
static int albumsPage = 0;
static int viewAlbum = -1, tracksTop = 0;
static std::vector<String> viewTracks;
static int eqCenter = 160;

// A short pulse; loop() switches it off. The motor needs ~100 ms to be felt.
// (Song loads used to block loop() for seconds and stretch this; they're fast now.)
// pulses: 1 for a tap, 2 (on-off-on) for a wrong answer. Each edge flips the motor.
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

static bool hit(const RoundButton& b, int x, int y) {
  int dx = x - b.x, dy = y - BTN_Y, r = b.r + 8;
  return dx * dx + dy * dy <= r * r;
}

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

// Draws centered, wrapped text with a 1 px shadow, picking the biggest font that fits.
static void drawTitle(const String& text, int cx, int cy, int w, int h, uint16_t fg, bool small = false) {
  auto& d = M5.Display;
  const lgfx::IFont* big[] = {&fonts::FreeSansBold18pt7b, &fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b};
  const lgfx::IFont* little[] = {&fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b};
  auto* list = small ? little : big;
  int count = small ? 2 : 3;
  std::vector<String> lines;
  int lh = 0;
  for (int f = 0; f < count; f++) {
    d.setFont(list[f]);
    lh = d.fontHeight();
    lines = wrapText(text, w, std::max(1, h / lh));
    bool fits = true;
    for (auto& l : lines) fits &= !l.endsWith("...");
    if (fits) break;
  }
  d.setTextDatum(middle_center);
  int y0 = cy - (int)(lines.size() - 1) * lh / 2;
  for (size_t i = 0; i < lines.size(); i++) {
    d.setTextColor(C_SHADE);
    d.drawString(lines[i], cx + 1, y0 + i * lh + 1);
    d.setTextColor(fg);
    d.drawString(lines[i], cx, y0 + i * lh);
  }
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
static void iconVolume(int x, int y, bool up, uint16_t c) {  // speaker + sign, centered
  iconSpeaker(x - 7, y, c);
  M5.Display.fillRoundRect(x + 4, y - 2, 12, 5, 2, c);
  if (up) M5.Display.fillRoundRect(x + 8, y - 6, 5, 13, 2, c);
}
static void iconBattery(int x, int y, int pct, bool usb) {
  auto& d = M5.Display;
  uint16_t fill = pct > 50 ? rgb(80, 220, 100) : pct > 20 ? C_YELLOW : rgb(255, 70, 70);
  d.drawRoundRect(x, y, 26, 14, 3, C_WHITE);
  d.fillRect(x + 26, y + 4, 3, 6, C_WHITE);
  d.fillRect(x + 2, y + 2, 22, 10, C_BAR);
  d.fillRect(x + 2, y + 2, 22 * constrain(pct, 0, 100) / 100, 10, fill);
  if (usb) {  // lightning bolt
    d.fillTriangle(x + 15, y + 1, x + 9, y + 8, x + 14, y + 8, C_WHITE);
    d.fillTriangle(x + 12, y + 13, x + 18, y + 6, x + 13, y + 6, C_WHITE);
  }
}
static void iconHeadphones(int x, int y, uint16_t c) {
  M5.Display.fillArc(x, y + 9, 9, 7, 180, 360, c);
  M5.Display.fillRoundRect(x - 10, y + 7, 5, 8, 2, c);
  M5.Display.fillRoundRect(x + 6, y + 7, 5, 8, 2, c);
}
static void iconWifi(int x, int y, uint16_t c) {
  for (int r = 4; r <= 12; r += 4) M5.Display.fillArc(x, y + 13, r, r - 2, 225, 315, c);
  M5.Display.fillCircle(x, y + 12, 2, c);
}
static void iconGrid(int x, int y, uint16_t c) {
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++) M5.Display.fillRoundRect(x + i * 9, y + j * 9, 7, 7, 2, c);
}
static void iconArrow(int x, int y, int dir, uint16_t c) {  // dir: 0 left, 1 right, 2 up, 3 down
  int s = 8;
  if (dir == 0) M5.Display.fillTriangle(x - s, y, x + s / 2, y - s, x + s / 2, y + s, c);
  else if (dir == 1) M5.Display.fillTriangle(x + s, y, x - s / 2, y - s, x - s / 2, y + s, c);
  else if (dir == 2) M5.Display.fillTriangle(x, y - s, x - s, y + s / 2, x + s, y + s / 2, c);
  else M5.Display.fillTriangle(x, y + s, x - s, y - s / 2, x + s, y - s / 2, c);
}

// ----- top bar: clock, connection, battery (hold 3 s for grown-ups) -----
// USB plugged in? The AXP192's VBUS-present flag, not the VBUS voltage reading,
// which can stay above 4 V for a while after unplugging.
static bool usbPower() { return M5.Power.Axp192.isVBUS(); }

static void drawTopBar() {
  auto& d = M5.Display;
  d.fillRect(0, 0, 320, TOP_H, C_BAR);
  m5::rtc_datetime_t dt;
  if (M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&dt) && dt.date.year >= 2024) {
    int h12 = dt.time.hours % 12 ? dt.time.hours % 12 : 12;
    char hm[8];
    snprintf(hm, sizeof hm, "%d:%02d", h12, dt.time.minutes);
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextColor(C_WHITE);
    d.setTextDatum(middle_left);
    d.drawString(hm, 8, TOP_H / 2);
  }
#if CORE2_WIFI
  iconWifi(254, 2, webUp ? C_WHITE : C_DIM);
#else
  if (btConnected) iconHeadphones(254, 2, C_WHITE);
#endif
  iconBattery(280, 4, M5.Power.getBatteryLevel(), usbPower());
}

static constexpr uint32_t HOLD_MS = 3000;
static constexpr int HOLD_ZONE = 44;  // the 22 px bar is too thin to hit reliably; include the card's top edge

// A yellow strip grows along the bottom of the top bar; full = grown-ups screen.
static void drawHoldProgress(uint32_t heldMs) {
  M5.Display.fillRect(0, TOP_H - 4, 320 * std::min(heldMs, HOLD_MS) / HOLD_MS, 4, C_YELLOW);
}

// ----- player -----
static void drawEqualizer(bool animate) {
  static const uint8_t restHeights[] = {8, 12, 16, 12, 8, 12, 16, 12, 8};
  static uint32_t seed = 2463534242u;  // xorshift; libc rand() costs IRAM
  auto& d = M5.Display;
  int n = 9, bw = 10, gap = 6, x0 = eqCenter - (n * (bw + gap) - gap) / 2, base = CARD_Y + CARD_H - 8;
  d.fillRect(x0, base - 22, n * (bw + gap), 22, albumColor(playAlbum));
  for (int i = 0; i < n; i++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    int h = animate ? 8 + seed % 15 : restHeights[i];
    d.fillRoundRect(x0 + i * (bw + gap), base - h, bw, h, 3, C_WHITE);
  }
}

static void drawCard() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H, 320, PROG_Y - 4 - TOP_H, C_BG);  // down to where drawProgress takes over
  if (tracks.empty()) {
    d.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 18, C_OFF);
    drawTitle("No songs yet! Ask a grown-up to add some.", 160, CARD_Y + CARD_H / 2, CARD_W - 30, CARD_H - 20, C_WHITE);
    eqCenter = 160;
    return;
  }
  d.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 18, albumColor(playAlbum));
  int tx = CARD_X, tw = CARD_W;  // text area
  if (M5Canvas* cv = getCover(playAlbum, COVER)) {
    cv->pushSprite(&d, CARD_X + 8, CARD_Y + 8, COVER_KEY);
    tx = CARD_X + 8 + COVER;
    tw = CARD_W - 8 - COVER;
  } else {
    iconGrid(CARD_X + 12, CARD_Y + 12, C_WHITE);
  }
  int cx = tx + tw / 2;
  eqCenter = cx;
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(C_WHITE);
  d.setTextDatum(top_right);
  d.drawString(String(current + 1) + "/" + String(tracks.size()), CARD_X + CARD_W - 12, CARD_Y + 8);
  drawTitle(title, cx, CARD_Y + 48, tw - 28, 50, C_WHITE);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  d.setTextColor(C_SHADE);
  String an = fitLine(albumName(), tw - 24);
  d.drawString(an, cx + 1, CARD_Y + 84);
  d.setTextColor(rgb(235, 240, 255));
  d.drawString(an, cx, CARD_Y + 83);
  drawEqualizer(false);
}

static void drawProgress() {
  auto& d = M5.Display;
  int x = 18, w = 284, h = 10;
  int p = (file && file->getSize() > 0) ? (int64_t)w * file->getPos() / file->getSize() : 0;
  d.fillRect(x - 9, PROG_Y - 4, w + 18, h + 8, C_BG);
  d.fillRoundRect(x, PROG_Y, w, h, h / 2, C_OFF);
  if (p > 0) d.fillRoundRect(x, PROG_Y, std::max(p, h), h, h / 2, albumColor(playAlbum));
  d.fillCircle(x + p, PROG_Y + h / 2, 8, C_WHITE);
}

static void drawButton(const RoundButton& b, uint16_t color) {
  M5.Display.fillCircle(b.x, BTN_Y + 3, b.r, C_SHADE);  // drop shadow
  M5.Display.fillCircle(b.x, BTN_Y, b.r, color);
}

static void drawControls() {
  auto& d = M5.Display;
  d.fillRect(0, BTN_Y - B_PLAY.r - 2, 320, 240 - (BTN_Y - B_PLAY.r - 2), C_BG);
  drawButton(B_VOLDN, C_ORANGE);
  iconVolume(B_VOLDN.x, BTN_Y, false, C_WHITE);
  drawButton(B_PREV, C_BLUE);
  iconSkip(B_PREV.x, BTN_Y, 24, false, C_WHITE);
  drawButton(B_PLAY, C_GREEN);
  if (paused || tracks.empty()) iconPlay(B_PLAY.x, BTN_Y, 28, C_WHITE);
  else iconPause(B_PLAY.x, BTN_Y, 26, C_WHITE);
  drawButton(B_NEXT, C_BLUE);
  iconSkip(B_NEXT.x, BTN_Y, 24, true, C_WHITE);
  drawButton(B_VOLUP, C_ORANGE);
  iconVolume(B_VOLUP.x, BTN_Y, true, C_WHITE);
}

static void showVolumeOverlay() {
  if (screen != Screen::Player) return;
  auto& d = M5.Display;
  int w = 220, h = 70, x = 160 - w / 2, y = CARD_Y + CARD_H / 2 - h / 2;
  d.fillRoundRect(x, y, w, h, 14, rgb(20, 20, 40));
  iconSpeaker(x + 26, y + h / 2, C_WHITE);
  for (int i = 0; i < 10; i++) {
    int bh = 10 + i * 4, bx = x + 50 + i * 16;
    d.fillRoundRect(bx, y + h / 2 + 22 - bh, 11, bh, 3, i < volLevel ? C_YELLOW : rgb(70, 74, 100));
  }
  overlayUntil = millis() + 1500;
}

// ----- albums -----
// Square tiles, 3 across and 2 down, using the whole screen below the top bar.
// The cover fills the tile; without one, the album name sits on the album's color.
// The playing album gets a ring and a badge. Swipe up/down to page (dots at the
// bottom); swipe right (or the Music button) goes back to the player, the same
// "back" as on the song list.
static constexpr int TILE = 100, TILE_GAP_X = 7, TILE_GAP_Y = 6, TILES = 6;
static constexpr int GRID_X = 3, GRID_Y = 26;

static int albumPages() { return std::max<int>(1, (albums.size() + TILES - 1) / TILES); }

static void iconPlaying(int x, int y, uint16_t c) {  // three little equalizer bars
  M5.Display.fillRect(x - 6, y - 1, 3, 6, c);
  M5.Display.fillRect(x - 1, y - 5, 3, 10, c);
  M5.Display.fillRect(x + 4, y - 3, 3, 8, c);
}

static void drawAlbums() {
  auto& d = M5.Display;
  albumsPage = constrain(albumsPage, 0, albumPages() - 1);  // albums may have been deleted
  d.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
  for (int k = 0; k < TILES; k++) {
    int a = albumsPage * TILES + k;
    if (a >= (int)albums.size()) break;
    int x = GRID_X + (k % 3) * (TILE + TILE_GAP_X), y = GRID_Y + (k / 3) * (TILE + TILE_GAP_Y);
    if (a == playAlbum)
      for (int t = 2; t <= 3; t++) d.drawRoundRect(x - t, y - t, TILE + 2 * t, TILE + 2 * t, 11 + t, C_WHITE);
    if (M5Canvas* cv = getCover(a, TILE)) {
      cv->pushSprite(&d, x, y, COVER_KEY);
    } else {
      d.fillRoundRect(x, y, TILE, TILE, TILE / 8, albumColor(a));
      drawTitle(albums[a].name, x + TILE / 2, y + TILE / 2, TILE - 12, TILE - 12, C_WHITE, true);
    }
    if (a == playAlbum) {
      d.fillCircle(x + TILE - 14, y + TILE - 14, 12, C_GREEN);
      iconPlaying(x + TILE - 14, y + TILE - 14, C_WHITE);
    }
  }
  int pages = albumPages();
  for (int p = 0; p < pages && pages > 1 && p < 20; p++)
    d.fillCircle(160 - (std::min(pages, 20) - 1) * 6 + p * 12, 237, 2, p == albumsPage ? C_WHITE : C_DIM);
}

// ----- tracks -----
// Album header (tap it, or swipe right, to go back), then five big song rows.
// Swipe up/down to scroll; the thin bar on the right shows where you are.
static constexpr int TR_HEAD_H = 34, TR_Y = TOP_H + TR_HEAD_H + 6;
// Songs: five one-line rows. Podcast episodes: three taller rows (long titles wrap).
static int trRowH() { return isPodcastAlbum(viewAlbum) ? 58 : 35; }
static int trRows() { return isPodcastAlbum(viewAlbum) ? 3 : 5; }

static void openTracks(int a) {
  viewAlbum = a;
  viewTracks = (a == playAlbum) ? tracks : listTracks(a);
  tracksTop = (a == playAlbum && current >= 0) ? std::max(0, std::min(current - 1, (int)viewTracks.size() - trRows())) : 0;
  screen = Screen::Tracks;
  dirtyAll = true;
}

static void drawTracks() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
  int hy = TOP_H + 2, cy = hy + TR_HEAD_H / 2;
  d.fillRoundRect(6, hy, 308, TR_HEAD_H, 10, albumColor(viewAlbum));
  iconArrow(20, cy, 0, C_WHITE);  // back
  int tx = 36;
  if (M5Canvas* cv = getCover(viewAlbum, TILE)) {  // reuse the tile-size cover, shown small
    cv->pushRotateZoom(&d, tx + 13, cy, 0, 26.0f / TILE, 26.0f / TILE, COVER_KEY);
    tx += 34;
  }
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(middle_left);
  d.setTextColor(C_WHITE);
  d.drawString(fitLine(albums[viewAlbum].name, 314 - tx - 10), tx, cy);
  int rowH = trRowH(), rows = trRows();
  bool podcast = isPodcastAlbum(viewAlbum);
  for (int r = 0; r < rows; r++) {
    int i = tracksTop + r;
    if (i >= (int)viewTracks.size()) break;
    int y = TR_Y + r * rowH, h = rowH - 4, my = y + h / 2;
    bool playing = viewAlbum == playAlbum && i == current;
    uint16_t ac = albumColor(viewAlbum);
    d.fillRoundRect(6, y, 300, h, 10, playing ? ac : C_ROW);
    if (podcast) {  // date badge + title wrapped over up to three lines
      String f = viewTracks[i].substring(viewTracks[i].lastIndexOf('/') + 1);
      static const char* MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
      int mon = f.length() > 10 && f[4] == '-' ? f.substring(5, 7).toInt() : 0;
      d.fillRoundRect(11, y + 5, 44, h - 10, 8, playing ? C_WHITE : ac);
      d.setTextDatum(middle_center);
      d.setTextColor(playing ? ac : C_WHITE);
      if (mon >= 1 && mon <= 12) {
        d.setFont(&fonts::FreeSansBold9pt7b);
        d.drawString(MON[mon - 1], 33, y + 18);
        d.setFont(&fonts::FreeSansBold12pt7b);
        d.drawString(String(f.substring(8, 10).toInt()), 33, y + 36);
      } else {
        d.setFont(&fonts::FreeSansBold12pt7b);
        d.drawString(String(i + 1), 33, my);
      }
      d.setFont(&fonts::FreeSansBold9pt7b);
      auto lines = wrapText(trackTitle(viewTracks[i], viewAlbum), 236, 3);
      int lh = 17, ty = my - (int)(lines.size() - 1) * lh / 2;
      d.setTextDatum(middle_left);
      d.setTextColor(C_WHITE);
      for (size_t k = 0; k < lines.size(); k++) d.drawString(lines[k], 64, ty + k * lh);
      continue;
    }
    d.fillCircle(24, my, 12, playing ? C_WHITE : C_OFF);
    if (playing) iconPlaying(24, my, ac);
    else {
      d.setFont(&fonts::FreeSansBold9pt7b);
      d.setTextDatum(middle_center);
      d.setTextColor(C_WHITE);
      d.drawString(String(i + 1), 24, my);
    }
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(middle_left);
    d.setTextColor(C_WHITE);
    d.drawString(fitLine(trackTitle(viewTracks[i], viewAlbum), 252), 44, my);
  }
  int n = viewTracks.size();
  if (n > rows) {  // scroll bar
    int trackH = rows * rowH - 4, barH = std::max(16, trackH * rows / n);
    int barY = TR_Y + (trackH - barH) * tracksTop / std::max(1, n - rows);
    d.fillRoundRect(311, TR_Y, 5, trackH, 2, C_ROW);
    d.fillRoundRect(311, std::min(barY, TR_Y + trackH - barH), 5, barH, 2, C_DIM);
  }
}

// ================= Games: Spelling (middle button) and Math (right button) =================
// Four quiz games on one engine: Missing Letter, Right Spelling, Telling Time, Math.
// Three answers; a wrong pick greys out so they can try again. Levels are rounds of
// 5 questions (dots in the header): 4+ first-try answers level up, 2 or fewer drop
// a level. Pitched for a 5-year-old at level 1 up to a 2nd grader at the top.
// Sounds only play when the music is stopped or paused (and never over Bluetooth).

static Quiz spellQuiz{"spell", 9}, mathQuiz{"math", 11}, clockQuiz{"clock", 5}, wordsQuiz{"words", 8, true}, patternQuiz{"pattern", 6};
static uint32_t quizFxUntil = 0, quizLastFx = 0;
static String quizBanner;
static constexpr int MB_Y = 202, MB_W = 92, MB_H = 60;
static constexpr int ST_Y0 = 82, ST_H = 42, ST_GAP = 7;  // stacked answer rows
static const int quizBubbleX[3] = {56, 160, 264};

static int rnd(int lo, int hi) {  // inclusive; xorshift (libc rand() costs IRAM)
  static uint32_t seed = 0;
  if (!seed) seed = esp_random() | 1;
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
  return lo + seed % (hi - lo + 1);
}

static bool musicIsPlaying() { return !paused && mp3 && mp3->isRunning(); }

static void chime(bool happy) {
  if (btConnected || musicIsPlaying()) return;
  if (happy) {
    M5.Speaker.tone(1047, 90, 1, true);
    M5.Speaker.tone(1319, 90, 1, false);
    M5.Speaker.tone(1568, 160, 1, false);
  } else {
    M5.Speaker.tone(196, 180, 1, true);
  }
}

static void drawStar(int x, int y, int r, uint16_t c) {
  // 5-point star in hundredths of r (precomputed: no sin/cos at runtime)
  static const int8_t P[10][2] = {{0, -100}, {26, -36}, {95, -31}, {43, 14}, {59, 81}, {0, 45}, {-59, 81}, {-43, 14}, {-95, -31}, {-26, -36}};
  auto px = [&](int i) { return x + P[i % 10][0] * r / 100; };
  auto py = [&](int i) { return y + P[i % 10][1] * r / 100; };
  for (int i = 0; i < 10; i += 2) {
    M5.Display.fillTriangle(x, y, px(i), py(i), px(i + 1), py(i + 1), c);
    M5.Display.fillTriangle(x, y, px(i), py(i), px(i + 9), py(i + 9), c);
  }
}

// Kid-friendly shapes for Memory Match and What Comes Next.
// Shapes: 0 circle, 1 square, 2 triangle, 3 star, 4 diamond, 5 heart, 6 plus, 7 ring.
static const uint8_t SHAPE_COL[6][3] = {{235, 60, 70}, {40, 140, 240}, {40, 200, 90}, {250, 205, 40}, {150, 80, 220}, {255, 146, 76}};
static constexpr int SHAPES = 8, SHAPE_COLORS = 6;

static void drawShape(int kind, int color, int cx, int cy, int r, uint16_t bg) {
  auto& d = M5.Display;
  const uint8_t* k = SHAPE_COL[color % SHAPE_COLORS];
  uint16_t c = rgb(k[0], k[1], k[2]);
  switch (kind % SHAPES) {
    case 0: d.fillCircle(cx, cy, r, c); break;
    case 1: d.fillRoundRect(cx - r * 85 / 100, cy - r * 85 / 100, r * 17 / 10, r * 17 / 10, r / 5, c); break;
    case 2: d.fillTriangle(cx, cy - r, cx - r, cy + r * 8 / 10, cx + r, cy + r * 8 / 10, c); break;
    case 3: drawStar(cx, cy + r / 10, r + 1, c); break;
    case 4: d.fillTriangle(cx, cy - r, cx - r * 3 / 4, cy, cx + r * 3 / 4, cy, c);
            d.fillTriangle(cx, cy + r, cx - r * 3 / 4, cy, cx + r * 3 / 4, cy, c); break;
    case 5: d.fillCircle(cx - r / 2, cy - r / 4, r / 2 + 1, c);
            d.fillCircle(cx + r / 2, cy - r / 4, r / 2 + 1, c);
            d.fillTriangle(cx - r, cy - r / 8, cx + r, cy - r / 8, cx, cy + r, c); break;
    case 6: d.fillRoundRect(cx - r / 3, cy - r, r * 2 / 3, 2 * r, r / 6, c);
            d.fillRoundRect(cx - r, cy - r / 3, 2 * r, r * 2 / 3, r / 6, c); break;
    case 7: d.fillCircle(cx, cy, r, c); d.fillCircle(cx, cy, r * 45 / 100, bg); break;
  }
}

static void drawQuizHeader(const Quiz& q) {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H, 320, 28, C_BG);
  d.fillRoundRect(8, TOP_H + 4, 84, 22, 11, C_OFF);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  d.setTextColor(C_WHITE);
  d.drawString("Level " + String(q.level), 50, TOP_H + 15);
  for (int i = 0; i < 5; i++) {  // this round: green = first try, orange = needed help
    uint16_t c = q.round[i] == 1 ? C_GREEN : q.round[i] == 2 ? C_ORANGE : C_OFF;
    d.fillCircle(118 + i * 20, TOP_H + 15, 7, c);
  }
  drawStar(262, TOP_H + 15, 10, C_YELLOW);
  d.setTextDatum(middle_left);
  d.drawString(String(q.stars), 276, TOP_H + 15);
}

static void drawQuizBubble(const Quiz& q, int i) {
  auto& d = M5.Display;
  static const uint8_t col[3][3] = {{255, 89, 140}, {25, 150, 220}, {138, 80, 236}};
  bool right = q.solved && i == q.answer;
  uint16_t c = right ? C_GREEN : q.wrong[i] ? C_OFF : rgb(col[i][0], col[i][1], col[i][2]);
  int x, y, w, h, r;
  if (q.stacked) { x = 10; w = 300; h = ST_H; y = ST_Y0 + i * (ST_H + ST_GAP); r = 14; }
  else { x = quizBubbleX[i] - MB_W / 2; w = MB_W; h = MB_H; y = MB_Y - MB_H / 2; r = 22; }
  d.fillRect(x - 4, y - 4, w + 8, h + 10, C_BG);
  d.fillRoundRect(x, y + 3, w, h, r, C_SHADE);
  d.fillRoundRect(x, y, w, h, r, c);
  if (q.choices[i].startsWith("#")) {  // a shape answer (What Comes Next)
    drawShape(q.choices[i][1] - '0', q.choices[i][2] - '0', x + w / 2, y + h / 2, 20, c);
    if (q.wrong[i]) d.fillRoundRect(x + 6, y + h / 2 - 2, w - 12, 4, 2, C_DIM);  // crossed out
    return;
  }
  d.setTextDatum(middle_center);
  d.setTextColor(q.wrong[i] ? C_DIM : C_WHITE);
  for (auto* f : {&fonts::FreeSansBold24pt7b, &fonts::FreeSansBold18pt7b, &fonts::FreeSansBold12pt7b}) {
    d.setFont(f);  // biggest font that fits ("12:45" doesn't at 24 pt)
    if (d.textWidth(q.choices[i]) <= w - 12) break;
  }
  d.drawString(q.choices[i], x + w / 2, y + h / 2 + 2);
}

// Puts the answer at a random slot among two distractors.
static void setChoices(Quiz& q, const String& answer, const String& d1, const String& d2) {
  q.answer = rnd(0, 2);
  int k = 0;
  const String* other[2] = {&d1, &d2};
  for (int i = 0; i < 3; i++) q.choices[i] = i == q.answer ? answer : *other[k++];
  for (bool& w : q.wrong) w = false;
  q.tries = 0;
  q.solved = false;
}

// ----- math -----
static int mathA, mathB, mathResult;
static char mathOp;

// Level 1: sums to 5 with dots to count (age 5). Then + and - with bigger numbers,
// x2 x5 x10 from level 5, x3 x4 from 7, division from 8, all tables from 9.
static void newMathQuestion() {
  int L = mathQuiz.level, max = std::min(100, 5 + (L - 1) * 9);
  int kind = rnd(0, 99);
  bool div = L >= 8 && kind < 25, mul = !div && L >= 5 && kind < (L >= 8 ? 50 : 35);
  bool sub = !div && !mul && L >= 2 && rnd(0, 1);
  if (mul || div) {
    static const int f1[] = {2, 5, 10}, f2[] = {2, 3, 4, 5, 10};
    int f = L >= 9 ? rnd(2, 10) : L >= 7 ? f2[rnd(0, 4)] : f1[rnd(0, 2)];
    int k = rnd(1, 10);
    if (mul) { mathA = k; mathB = f; mathOp = 'x'; mathResult = k * f; }
    else { mathA = k * f; mathB = f; mathOp = '/'; mathResult = k; }
  } else if (sub) {
    mathA = rnd(1, max); mathB = rnd(0, mathA); mathOp = '-'; mathResult = mathA - mathB;
  } else {
    mathResult = rnd(2, max); mathA = rnd(0, mathResult); mathB = mathResult - mathA; mathOp = '+';
  }
  int cand[] = {mathResult + 1, mathResult - 1, mathResult + 2, mathResult - 2, mathResult + 10, mathResult - 10,
                mathOp == 'x' ? mathResult + mathB : mathOp == '/' ? mathB : mathOp == '+' ? abs(mathA - mathB) : mathA + mathB,
                mathOp == 'x' ? mathResult - mathB : mathResult + 3};
  int picked[2], n = 0;
  for (int t = 0; n < 2 && t < 60; t++) {
    int c = cand[rnd(0, mathResult >= 12 || mathOp == 'x' || mathOp == '/' ? 7 : 3)];
    if (c < 0 || c == mathResult || (n && c == picked[0])) continue;
    picked[n++] = c;
  }
  while (n < 2) { picked[n] = mathResult + 3 + n; n++; }
  setChoices(mathQuiz, String(mathResult), String(picked[0]), String(picked[1]));
}

// x and / are drawn (the font has no ÷ and its x looks like a letter).
static void drawOp(char op, int x, int y, uint16_t c) {  // x = left edge, 28 px wide
  auto& d = M5.Display;
  int cx = x + 14;
  if (op == 'x') {
    d.drawWideLine(cx - 8, y - 8, cx + 8, y + 8, 2.5f, c);
    d.drawWideLine(cx - 8, y + 8, cx + 8, y - 8, 2.5f, c);
  } else if (op == '/') {
    d.fillRoundRect(cx - 11, y - 2, 22, 5, 2, c);
    d.fillCircle(cx, y - 9, 3, c);
    d.fillCircle(cx, y + 10, 3, c);
  } else {
    d.setTextDatum(middle_center);
    d.drawString(op == '-' ? "-" : "+", cx, y);
  }
}

static void drawMathQuestion() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H + 28, 320, MB_Y - MB_H / 2 - 6 - (TOP_H + 28), C_BG);
  String a = String(mathA), b = String(mathB) + " = ", ans = mathQuiz.solved ? String(mathResult) : String("?");
  bool dots = mathOp == '+' && mathResult <= 10;
  d.setFont(&fonts::FreeSansBold24pt7b);
  int sp = 10, w = d.textWidth(a) + sp + 28 + sp + d.textWidth(b) + d.textWidth(ans), y = dots ? 92 : 110;
  int x = 160 - w / 2;
  d.setTextColor(C_WHITE);
  d.setTextDatum(middle_left);
  d.drawString(a, x, y);
  x += d.textWidth(a) + sp;
  drawOp(mathOp, x, y, C_WHITE);
  x += 28 + sp;
  d.setTextDatum(middle_left);
  d.drawString(b, x, y);
  x += d.textWidth(b);
  d.setTextColor(mathQuiz.solved ? C_GREEN : C_YELLOW);
  d.drawString(ans, x, y);
  if (dots) {  // things to count: a in blue, b in orange
    int total = mathA + mathB, x0 = 160 - (total * 14 + (mathA && mathB ? 10 : 0)) / 2 + 6;
    for (int i = 0; i < total; i++)
      d.fillCircle(x0 + i * 14 + (i >= mathA ? 10 : 0), 138, 5, i < mathA ? C_BLUE : C_ORANGE);
  }
}

// ----- spelling: fill in the missing letter, with a clue to read -----
struct Word { const char* word; const char* clue; };
static const Word WORDS_3[] = {
    {"cat", "A pet that says meow"}, {"dog", "A pet that barks"}, {"sun", "It shines in the sky"},
    {"hat", "You wear it on your head"}, {"bed", "You sleep in it"}, {"pig", "A pink farm animal"},
    {"cup", "You drink from it"}, {"bus", "A big yellow ride to school"}, {"fox", "An orange animal with a bushy tail"},
    {"hen", "A bird that lays eggs"}, {"bug", "A tiny thing that crawls"}, {"map", "It shows you the way"},
    {"jam", "Sweet and sticky, on toast"}, {"net", "You catch fish with it"}, {"pen", "You write with it"},
    {"red", "The color of a fire truck"}, {"six", "The number after five"}, {"ten", "How many toes you have"},
    {"box", "You put things in it"}, {"fan", "It blows air to cool you"}, {"van", "A car that carries lots"},
    {"web", "A spider makes it"}, {"mop", "You clean the floor with it"}, {"bat", "You hit a ball with it"},
    {"cow", "A farm animal that says moo"}, {"zoo", "Where you see lions and bears"}, {"pot", "You cook soup in it"},
    {"leg", "You walk on it"}, {"lip", "Part of your mouth"}, {"mud", "Wet, brown dirt"},
    {"jet", "A very fast plane"}, {"owl", "A bird awake at night"}, {"ant", "A tiny bug in a colony"},
    {"egg", "A chick hatches from it"}, {"ice", "Frozen water"}, {"pie", "A dessert with a crust"},
    {"toy", "Something you play with"}, {"car", "It drives on the road"}, {"bee", "It buzzes and makes honey"},
    {"ear", "You hear with it"}};
static const Word WORDS_4[] = {
    {"fish", "It swims in water"}, {"frog", "A green animal that hops"}, {"duck", "A bird that says quack"},
    {"cake", "You eat it on your birthday"}, {"bike", "It has two wheels and pedals"}, {"kite", "It flies on a string"},
    {"rain", "Water falling from clouds"}, {"tree", "It has a trunk and leaves"}, {"star", "It twinkles at night"},
    {"moon", "It glows in the night sky"}, {"ship", "A big boat"}, {"milk", "A white drink from cows"},
    {"book", "You read it"}, {"ball", "You kick or throw it"}, {"bell", "It rings"},
    {"nest", "A bird's home"}, {"hand", "It has five fingers"}, {"ring", "You wear it on a finger"},
    {"sock", "It goes on your foot"}, {"rock", "A hard stone"}, {"sing", "What you do with a song"},
    {"jump", "Go up in the air with your legs"}, {"king", "He wears a crown"}, {"lamp", "It gives light"},
    {"gift", "A present"}, {"snow", "Cold and white, it falls in winter"}, {"boat", "It floats on water"},
    {"coat", "You wear it when it's cold"}, {"goat", "A farm animal that eats everything"}, {"leaf", "It grows on a tree"},
    {"bear", "A big furry animal"}, {"door", "You open it to go in"}, {"shoe", "It goes on your foot"},
    {"seal", "It swims and claps its flippers"}, {"corn", "Yellow food on a cob"}, {"wolf", "A wild dog that howls"},
    {"nose", "You smell with it"}, {"bath", "You get clean in it"}, {"farm", "Where cows and pigs live"},
    {"swim", "What fish do"}};
static const Word WORDS_5[] = {
    {"apple", "A red or green fruit"}, {"train", "It rides on tracks"}, {"plant", "It grows in soil"},
    {"clock", "It tells the time"}, {"smile", "A happy face"}, {"snake", "A long animal with no legs"},
    {"house", "A home"}, {"horse", "You can ride it"}, {"whale", "The biggest animal in the sea"},
    {"sheep", "A farm animal with wool"}, {"chair", "You sit on it"}, {"bread", "You make toast with it"},
    {"cloud", "White and fluffy in the sky"}, {"grass", "Green and grows on a lawn"}, {"beach", "Sand by the sea"},
    {"lunch", "A meal in the middle of the day"}, {"mouse", "A small animal that squeaks"}, {"tooth", "It's in your mouth"},
    {"queen", "She wears a crown"}, {"zebra", "A horse with stripes"}, {"tiger", "A big cat with stripes"},
    {"pizza", "Cheesy food cut in slices"}, {"robot", "A machine that can move"}, {"rocket", "It flies to space"},
    {"turtle", "It carries its shell"}, {"monkey", "It swings in trees"}, {"rabbit", "It hops and has long ears"},
    {"spider", "It has eight legs"}, {"banana", "A long yellow fruit"}, {"kitten", "A baby cat"},
    {"pencil", "You write and draw with it"}, {"window", "You look outside through it"}, {"flower", "It blooms and smells nice"},
    {"garden", "Where flowers grow"}, {"dragon", "A make-believe beast that breathes fire"}, {"castle", "Where a king lives"},
    {"planet", "Earth is one"}, {"school", "Where you learn"}, {"friend", "Someone you like to play with"},
    {"family", "The people you live with"}};

static const Word* spellWord;
static int spellBlank;

static bool isVowel(char c) { return strchr("aeiou", c) != nullptr; }

// A letter that's easy (very different) or hard (easily confused) to tell apart from `c`.
static char distractor(char c, bool hard, char avoid) {
  static const char* groups[] = {"aeiou", "bdp", "mn", "ckq", "sz", "fv", "gj", "tf", "lr", "wy"};
  for (int t = 0; t < 40; t++) {
    char x = 'a' + rnd(0, 25);
    if (hard) {
      for (const char* g : groups)
        if (strchr(g, c)) { x = g[rnd(0, strlen(g) - 1)]; break; }
      if (x == c && t > 20) x = 'a' + rnd(0, 25);
    } else if (strchr("aeiou", c) ? isVowel(x) : !isVowel(x)) {
      continue;  // easy: a vowel's distractors are consonants and vice versa
    }
    if (x != c && x != avoid) return x;
  }
  return c == 'x' ? 'y' : 'x';
}

static void newSpellQuestion() {
  int s = (spellQuiz.level - 1) * 3;
  const Word* list;
  int n;
  int pick = rnd(0, 9);
  if (s < 6 || (s < 12 && pick < 3)) { list = WORDS_3; n = sizeof WORDS_3 / sizeof *WORDS_3; }
  else if (s < 18 || (pick < 4)) { list = WORDS_4; n = sizeof WORDS_4 / sizeof *WORDS_4; }
  else { list = WORDS_5; n = sizeof WORDS_5 / sizeof *WORDS_5; }
  const Word* prev = spellWord;
  do spellWord = &list[rnd(0, n - 1)]; while (spellWord == prev);
  int len = strlen(spellWord->word);
  if (s < 4) spellBlank = 0;  // first letter is easiest
  else {
    std::vector<int> vowels;
    for (int i = 0; i < len; i++) if (isVowel(spellWord->word[i])) vowels.push_back(i);
    spellBlank = (s >= 10 && !vowels.empty() && rnd(0, 2)) ? vowels[rnd(0, vowels.size() - 1)] : rnd(0, len - 1);
  }
  char c = spellWord->word[spellBlank];
  bool hard = s >= 8;
  char d1 = distractor(c, hard, 0), d2 = distractor(c, hard, d1);
  setChoices(spellQuiz, String(c), String(d1), String(d2));
}

static void drawSpellQuestion() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H + 28, 320, MB_Y - MB_H / 2 - 6 - (TOP_H + 28), C_BG);
  drawTitle(spellWord->clue, 160, 78, 300, 44, rgb(235, 240, 255), true);
  int len = strlen(spellWord->word), tw = 34, gap = 5, x0 = 160 - (len * (tw + gap) - gap) / 2, y = 108;
  d.setFont(&fonts::FreeSansBold18pt7b);
  d.setTextDatum(middle_center);
  for (int i = 0; i < len; i++) {
    int x = x0 + i * (tw + gap);
    bool blank = i == spellBlank;
    uint16_t bg = blank ? (spellQuiz.solved ? C_GREEN : C_BG) : C_ROW;
    d.fillRoundRect(x, y, tw, 44, 8, bg);
    if (blank && !spellQuiz.solved)
      for (int k = 0; k < 3; k++) d.drawRoundRect(x + k, y + k, tw - 2 * k, 44 - 2 * k, 8, C_YELLOW);
    d.setTextColor(blank && !spellQuiz.solved ? C_YELLOW : C_WHITE);
    d.drawString(blank && !spellQuiz.solved ? String("?") : String(spellWord->word[i]), x + tw / 2, y + 22);
  }
}

// ----- telling time: read the analog clock -----
// o'clock -> half past -> quarter past/to -> any 5 minutes. Wrong choices are the
// classic mistakes: hands read swapped, hour off by one, minutes off by a step.
static int clockH = 3, clockM = 0;
static const int16_t SIN60[60] = {0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995, 1000, 995, 978, 951, 914,
                                  866, 809, 743, 669, 588, 500, 407, 309, 208, 105, 0, -105, -208, -309, -407, -500, -588, -669, -743, -809,
                                  -866, -914, -951, -978, -995, -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588, -500, -407, -309, -208, -105};
static int sin60(int i) { return SIN60[((i % 60) + 60) % 60]; }
static int cos60(int i) { return sin60(i + 15); }

static String timeText(int h, int m) {
  char b[8];
  snprintf(b, sizeof b, "%d:%02d", h, m);
  return b;
}

static void newClockQuestion() {
  int L = clockQuiz.level, step = L == 1 ? 60 : L == 2 ? 30 : L == 3 ? 15 : 5;
  clockH = rnd(1, 12);
  clockM = step == 60 ? 0 : rnd(0, 60 / step - 1) * step;
  String answer = timeText(clockH, clockM), picks[2];
  String cand[] = {
      timeText(clockM ? clockM / 5 : 12, clockH % 12 * 5),  // hour and minute hands swapped
      timeText(clockH % 12 + 1, clockM),                     // hour one too high (the hour hand is past it)
      timeText(clockH == 1 ? 12 : clockH - 1, clockM),       // hour one too low
      timeText(clockH, (clockM + (step == 60 ? 30 : step)) % 60),
      timeText(clockH, (clockM + 30) % 60),
  };
  int n = 0;
  for (int t = 0; n < 2 && t < 40; t++) {
    const String& c = cand[rnd(0, 4)];
    if (c != answer && (n == 0 || c != picks[0])) picks[n++] = c;
  }
  while (n < 2) { picks[n] = timeText(clockH % 12 + 1 + n, 0); n++; }
  setChoices(clockQuiz, answer, picks[0], picks[1]);
}

static void drawHand(int cx, int cy, int step60, int len, float width, uint16_t c) {
  M5.Display.drawWideLine(cx, cy, cx + sin60(step60) * len / 1000, cy - cos60(step60) * len / 1000, width, c);
}

static void drawClockQuestion() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H + 28, 320, MB_Y - MB_H / 2 - 6 - (TOP_H + 28), C_BG);
  int cx = 160, cy = 111, r = 56;
  d.fillCircle(cx, cy + 3, r, C_SHADE);
  d.fillCircle(cx, cy, r, clockQuiz.solved ? rgb(220, 250, 230) : rgb(250, 246, 232));
  d.drawCircle(cx, cy, r, rgb(90, 90, 120));
  d.drawCircle(cx, cy, r - 1, rgb(90, 90, 120));
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  d.setTextColor(rgb(40, 40, 70));
  for (int h = 1; h <= 12; h++) d.drawString(String(h), cx + sin60(h * 5) * (r - 12) / 1000, cy - cos60(h * 5) * (r - 12) / 1000 + 1);
  for (int k = 0; k < 60; k++)
    if (k % 5) d.drawPixel(cx + sin60(k) * (r - 3) / 1000, cy - cos60(k) * (r - 3) / 1000, rgb(150, 150, 170));
  drawHand(cx, cy, clockM, r - 14, 3, C_BLUE);                          // minute hand
  drawHand(cx, cy, clockH % 12 * 5 + clockM / 12, r * 52 / 100, 5, rgb(235, 70, 80));  // hour hand
  d.fillCircle(cx, cy, 4, rgb(40, 40, 70));
}

// ----- right spelling: pick the correctly spelled word -----
// Misspellings are the kind kids actually make; none is a real word with another meaning.
struct Spelled { const char* right; const char* wrong1; const char* wrong2; };
static const Spelled SPELLED_1[] = {
    {"said", "sed", "siad"}, {"they", "thay", "tehy"}, {"was", "wuz", "waz"}, {"have", "hav", "haev"},
    {"come", "cum", "kom"}, {"some", "sum", "som"}, {"what", "wat", "whut"}, {"you", "yoo", "yuo"},
    {"are", "arr", "aer"}, {"the", "teh", "thu"}, {"of", "uv", "ov"}, {"from", "frum", "fram"},
    {"were", "wer", "whur"}, {"does", "duz", "dose"}, {"look", "luk", "lok"}, {"want", "wunt", "whant"},
    {"there", "thare", "ther"}, {"many", "meny", "mani"}, {"two", "tuo", "twoo"}, {"one", "wun", "oen"}};
static const Spelled SPELLED_2[] = {
    {"because", "becuz", "becaus"}, {"friend", "freind", "frend"}, {"people", "peeple", "poeple"},
    {"where", "whare", "wher"}, {"could", "cud", "coud"}, {"would", "wud", "woud"}, {"school", "skool", "schol"},
    {"again", "agen", "agian"}, {"every", "evry", "evrey"}, {"pretty", "prity", "pretey"}, {"very", "verry", "vurry"},
    {"laugh", "laf", "lauf"}, {"only", "onley", "onely"}, {"once", "wunce", "onse"}, {"little", "littel", "litle"},
    {"about", "abowt", "abot"}, {"today", "todai", "tooday"}, {"house", "hous", "howse"}, {"girl", "gurl", "girel"},
    {"water", "watter", "wawter"}};
static const Spelled SPELLED_3[] = {
    {"beautiful", "beutiful", "beautyful"}, {"different", "diffrent", "differant"}, {"favorite", "favrite", "faverite"},
    {"together", "togather", "tugether"}, {"answer", "anser", "ansewr"}, {"busy", "bizzy", "buzy"},
    {"enough", "enuf", "enouf"}, {"special", "speshal", "specail"}, {"animal", "aminal", "animle"},
    {"brought", "brot", "braught"}, {"thought", "thot", "thougt"}, {"minute", "minit", "minnut"},
    {"chocolate", "choclate", "chocolat"}, {"dinosaur", "dinosor", "dinasaur"}, {"probably", "probly", "probaly"},
    {"library", "libary", "liberry"}, {"birthday", "brithday", "berthday"}, {"surprise", "suprise", "serprise"},
    {"tomorrow", "tommorow", "tomorow"}, {"believe", "beleive", "beleve"}};
static const Spelled* spelledWord = nullptr;

static void newWordsQuestion() {
  int s = (wordsQuiz.level - 1) * 3, pick = rnd(0, 9);
  const Spelled* list;
  int n;
  if (s < 6 || (s < 14 && pick < 2)) { list = SPELLED_1; n = sizeof SPELLED_1 / sizeof *SPELLED_1; }
  else if (s < 14 || pick < 3) { list = SPELLED_2; n = sizeof SPELLED_2 / sizeof *SPELLED_2; }
  else { list = SPELLED_3; n = sizeof SPELLED_3 / sizeof *SPELLED_3; }
  const Spelled* prev = spelledWord;
  do spelledWord = &list[rnd(0, n - 1)]; while (spelledWord == prev);
  setChoices(wordsQuiz, spelledWord->right, spelledWord->wrong1, spelledWord->wrong2);
}

static void drawWordsQuestion() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H + 28, 320, ST_Y0 - 2 - (TOP_H + 28), C_BG);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(middle_center);
  d.setTextColor(wordsQuiz.solved ? C_GREEN : rgb(235, 240, 255));
  d.drawString(wordsQuiz.solved ? "Yes! That's right!" : "Which spelling is correct?", 160, TOP_H + 42);
}

// ----- what comes next: finish the pattern -----
// Pictures first (no reading): AB, AAB, ABC, AABB; then skip-counting numbers for
// the 7-year-old. Shape answers are encoded as "#<shape><color>" and drawn on the bubble.
static String patItems[8];  // the visible sequence (shape codes or numbers); the answer is next
static int patCount = 0;
static bool patNumbers = false;

static String shapeCode(int kind, int color) { return "#" + String(kind) + String(color); }

static void newPatternQuestion() {
  int L = patternQuiz.level;
  String answer, d1, d2;
  if (L >= 5 && (L == 6 || rnd(0, 1))) {  // numbers: 2 4 6 8 ?
    static const int easy[] = {1, 2, 5, 10}, more[] = {2, 3, 5, 10};
    int step = L == 5 ? easy[rnd(0, 3)] : more[rnd(0, 3)];
    bool down = L == 6 && rnd(0, 2) == 0;
    patNumbers = true;
    patCount = 5;
    int start = down ? step * (patCount + rnd(1, 5)) : step * rnd(0, 4);  // counting down never goes below 0
    for (int i = 0; i < patCount; i++) patItems[i] = String(start + (down ? -1 : 1) * i * step);
    int next = start + (down ? -1 : 1) * patCount * step;
    answer = String(next);
    d1 = String(next + (rnd(0, 1) || next == 0 ? 1 : -1));  // off by one
    d2 = patItems[patCount - 1];                            // repeating the last number
    if (d2 == d1) d2 = String(next + (down ? -step : step));
  } else {
    patNumbers = false;
    // pattern unit by level: AB, AAB, ABC, AABB/ABB
    static const char* units[] = {"AB", "AAB", "ABC", "AABB"};
    String unit = L >= 4 ? (rnd(0, 1) ? "AABB" : "ABB") : units[L - 1];
    int nUnique = unit.indexOf('C') >= 0 ? 3 : 2;
    // what changes: color only (same shape), shape only, or both
    int mode = L == 1 ? rnd(0, 1) : rnd(0, 2);
    int baseShape = rnd(0, SHAPES - 1), baseColor = rnd(0, SHAPE_COLORS - 1);
    String elem[4];
    static const int SHAPE_STEP[4] = {0, 3, 6, 1}, COLOR_STEP[4] = {1, 3, 5, 2};  // four distinct pieces
    for (int u = 0; u < 4; u++) {
      int shape = mode == 0 ? baseShape : (baseShape + SHAPE_STEP[u]) % SHAPES;
      int color = mode == 1 ? baseColor : (baseColor + COLOR_STEP[u]) % SHAPE_COLORS;
      elem[u] = shapeCode(shape, color);
    }
    patCount = std::min(7, (int)unit.length() * 2 + (unit.length() == 2 ? 1 : 0));
    for (int i = 0; i < patCount; i++) patItems[i] = elem[unit[i % unit.length()] - 'A'];
    answer = elem[unit[patCount % unit.length()] - 'A'];
    // wrong choices: the other pattern pieces, plus a new one if needed
    String others[4];
    int n = 0;
    for (int u = 0; u < nUnique; u++) if (elem[u] != answer) others[n++] = elem[u];
    others[n++] = elem[3];
    d1 = others[0];
    d2 = others[1] != d1 ? others[1] : others[2];
  }
  setChoices(patternQuiz, answer, d1, d2);
}

// Draws one sequence item / answer: a shape code, or a number in a rounded box.
static void drawPatternItem(const String& item, int cx, int cy, int r, uint16_t bg) {
  if (item.startsWith("#")) { drawShape(item[1] - '0', item[2] - '0', cx, cy, r, bg); return; }
  auto& d = M5.Display;
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(middle_center);
  d.setTextColor(C_WHITE);
  d.drawString(item, cx, cy + 1);
}

static void drawPatternQuestion() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H + 28, 320, MB_Y - MB_H / 2 - 6 - (TOP_H + 28), C_BG);
  int n = patCount + 1, cell = patNumbers ? 44 : 38, x0 = 160 - n * cell / 2 + cell / 2, y = 110;
  for (int i = 0; i < patCount; i++) {
    if (patNumbers) d.fillRoundRect(x0 + i * cell - cell / 2 + 2, y - 18, cell - 4, 36, 8, C_ROW);
    drawPatternItem(patItems[i], x0 + i * cell, y, 14, patNumbers ? C_ROW : C_BG);
  }
  int qx = x0 + patCount * cell;
  if (patternQuiz.solved) {
    if (patNumbers) d.fillRoundRect(qx - cell / 2 + 2, y - 18, cell - 4, 36, 8, C_GREEN);
    drawPatternItem(patternQuiz.choices[patternQuiz.answer], qx, y, 14, patNumbers ? C_GREEN : C_BG);
  } else {
    for (int k = 0; k < 2; k++) d.drawRoundRect(qx - cell / 2 + 2 + k, y - 18 + k, cell - 4 - 2 * k, 36 - 2 * k, 8, C_YELLOW);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(middle_center);
    d.setTextColor(C_YELLOW);
    d.drawString("?", qx, y + 1);
  }
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(C_DIM);
  d.drawString("What comes next?", 160, 148);
}

// ----- shared flow -----
static bool isQuiz(Screen s) {
  return s == Screen::Spell || s == Screen::Math || s == Screen::Clock || s == Screen::Words || s == Screen::Pattern;
}
static Quiz& quizFor(Screen s) {
  return s == Screen::Spell ? spellQuiz : s == Screen::Clock ? clockQuiz : s == Screen::Words ? wordsQuiz
       : s == Screen::Pattern ? patternQuiz : mathQuiz;
}
static void newQuestion(Screen s) {
  if (s == Screen::Spell) newSpellQuestion();
  else if (s == Screen::Clock) newClockQuestion();
  else if (s == Screen::Words) newWordsQuestion();
  else if (s == Screen::Pattern) newPatternQuestion();
  else newMathQuestion();
}
static void drawQuestion(Screen s) {
  if (s == Screen::Spell) drawSpellQuestion();
  else if (s == Screen::Clock) drawClockQuestion();
  else if (s == Screen::Words) drawWordsQuestion();
  else if (s == Screen::Pattern) drawPatternQuestion();
  else drawMathQuestion();
}

static void drawQuiz(Screen s) {
  Quiz& q = quizFor(s);
  M5.Display.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
  drawQuizHeader(q);
  drawQuestion(s);
  for (int i = 0; i < 3; i++) drawQuizBubble(q, i);
}

static void enterQuiz(Screen s) {
  Quiz& q = quizFor(s);
  if (!q.started || (q.solved && !q.nextAt)) newQuestion(s);
  q.started = true;
}

static void showScreen(Screen s);
static void handleQuiz(Screen s, const m5::touch_detail_t& t) {
  Quiz& q = quizFor(s);
  int dir;
  if (flickedX(t, dir) && dir < 0) { buzz(); showScreen(Screen::PlayHome); return; }  // swipe right = back to the games
  if (!t.wasClicked() || q.solved) return;
  for (int i = 0; i < 3; i++) {
    bool inside = q.stacked ? t.x >= 6 && t.x < 314 && abs(t.y - (ST_Y0 + i * (ST_H + ST_GAP) + ST_H / 2)) <= ST_H / 2 + 3
                            : abs(t.x - quizBubbleX[i]) <= MB_W / 2 + 6 && abs(t.y - MB_Y) <= MB_H / 2 + 8;
    if (!inside || q.wrong[i]) continue;
    if (i == q.answer) {
      buzz();
      chime(true);
      q.solved = true;
      q.stars++;
      q.round[q.roundDone++] = q.tries == 0 ? 1 : 2;
      quizBanner = "";
      if (q.roundDone == 5) {  // round over: 4+ first-try answers up, 2 or fewer down
        int right = 0;
        for (uint8_t r : q.round) right += r == 1;
        if (right >= 4) quizBanner = q.level < q.maxLevel ? (q.level++, "Level up!") : "Super star!";
        else if (right <= 2 && q.level > 1) { q.level--; quizBanner = "Let's practice!"; }
        else quizBanner = "Keep going!";
        q.roundDone = 0;
        memset(q.round, 0, sizeof q.round);
      }
      prefs.putUChar((String(q.key) + "lvl").c_str(), q.level);
      prefs.putUInt((String(q.key) + "stars").c_str(), q.stars);
      drawQuizHeader(q);
      drawQuestion(s);
      drawQuizBubble(q, i);
      quizFxUntil = millis() + 1100;
      q.nextAt = millis() + (quizBanner.length() ? 2200 : 1300);
    } else {
      buzz(2);
      chime(false);
      q.wrong[i] = true;
      q.tries++;
      drawQuizBubble(q, i);
    }
    return;
  }
}

// Confetti while celebrating, then the next question.
static void serviceQuiz(Screen s) {
  Quiz& q = quizFor(s);
  uint32_t now = millis();
  if (quizFxUntil && now - quizLastFx > 40) {
    quizLastFx = now;
    static const uint16_t conf[] = {0xF8A0, 0x07E0, 0x041F, 0xFFE0, 0xF81F, 0x07FF};
    for (int k = 0; k < 6; k++)
      M5.Display.fillRect(rnd(4, 312), rnd(TOP_H + 30, q.stacked ? ST_Y0 - 8 : MB_Y - MB_H / 2 - 8), rnd(3, 7), rnd(3, 7), conf[rnd(0, 5)]);
    if (quizBanner.length()) {
      M5.Display.setFont(&fonts::FreeSansBold18pt7b);
      M5.Display.setTextDatum(middle_center);
      M5.Display.setTextColor(C_YELLOW);
      M5.Display.drawString(quizBanner, 160, q.stacked ? 56 : 118);
    }
    if ((int32_t)(now - quizFxUntil) >= 0) quizFxUntil = 0;
  }
  if (q.nextAt && (int32_t)(now - q.nextAt) >= 0) {
    q.nextAt = 0;
    newQuestion(s);
    drawQuiz(s);
  }
}

static void loadQuizProgress() {
  for (Quiz* q : {&spellQuiz, &mathQuiz, &clockQuiz, &wordsQuiz, &patternQuiz}) {
    String k = q->key;
    int lvl = prefs.isKey((k + "lvl").c_str()) ? prefs.getUChar((k + "lvl").c_str(), 1)
                                               : prefs.getUChar((k + "skill").c_str(), 0) / 3 + 1;  // from the skill-score builds
    q->level = constrain(lvl, 1, q->maxLevel);
    q->stars = prefs.getUInt((k + "stars").c_str(), 0);
  }
}

// ================= Simon: watch the pads light up, then copy the sequence =================
// No reading needed. Each round adds one step and plays a little faster. Waiting
// too long just replays the sequence; a wrong tap ends the game with the score.
// Best score is saved. Tones follow the games rule (silent while music plays).
static constexpr int SIMON_MAX = 64;
static constexpr int SP_Y = 52, SP_W = 151, SP_H = 90, SP_GAP = 6;
static const uint8_t SIMON_COL[4][3] = {{40, 200, 90}, {235, 60, 70}, {250, 205, 40}, {40, 140, 240}};
static const uint16_t SIMON_HZ[4] = {330, 277, 440, 165};  // the classic Simon notes
enum class SimonState { Start, Showing, Input, Over };

static SimonState simonState = SimonState::Start;
static uint8_t simonSeq[SIMON_MAX];
static int simonLen = 0, simonPos = 0, simonBest = 0, simonLit = -1;
static uint32_t simonNextAt = 0, simonInputAt = 0;
static String simonMsg;

static void simonPadRect(int i, int& x, int& y) {
  x = 6 + (i % 2) * (SP_W + SP_GAP);
  y = SP_Y + (i / 2) * (SP_H + SP_GAP);
}

static void drawSimonPad(int i, bool lit) {
  auto& d = M5.Display;
  int x, y;
  simonPadRect(i, x, y);
  const uint8_t* c = SIMON_COL[i];
  d.fillRoundRect(x, y, SP_W, SP_H, 18, lit ? rgb(std::min(255, c[0] + 60), std::min(255, c[1] + 60), std::min(255, c[2] + 60))
                                            : rgb(c[0] * 2 / 5, c[1] * 2 / 5, c[2] * 2 / 5));
  if (lit)
    for (int t = 0; t < 3; t++) d.drawRoundRect(x + t, y + t, SP_W - 2 * t, SP_H - 2 * t, 18 - t, C_WHITE);
}

static void drawSimonHeader() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H, 320, SP_Y - 2 - TOP_H, C_BG);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(middle_left);
  d.setTextColor(C_WHITE);
  d.drawString(simonMsg, 10, TOP_H + 15);
  drawStar(232, TOP_H + 15, 9, C_YELLOW);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.drawString("Best " + String(simonBest), 246, TOP_H + 16);
}

static void drawSimon() {
  M5.Display.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
  drawSimonHeader();
  for (int i = 0; i < 4; i++) drawSimonPad(i, i == simonLit);
  if (simonState == SimonState::Start || simonState == SimonState::Over) {  // big play button in the middle
    M5.Display.fillCircle(160, SP_Y + SP_H + SP_GAP / 2, 30, C_SHADE);
    M5.Display.fillCircle(160, SP_Y + SP_H + SP_GAP / 2 - 2, 30, C_WHITE);
    iconPlay(160, SP_Y + SP_H + SP_GAP / 2 - 2, 26, C_GREEN);
  }
}

static void simonTone(int pad, int ms) {
  if (btConnected || musicIsPlaying()) return;
  M5.Speaker.tone(SIMON_HZ[pad], ms, 1, true);
}

static int simonOnMs() { return simonLen < 6 ? 480 : simonLen < 10 ? 380 : 300; }
static int simonOffMs() { return simonLen < 6 ? 180 : 130; }

static void simonShow() {  // play the sequence from the start
  simonState = SimonState::Showing;
  simonPos = 0;
  simonLit = -1;
  simonNextAt = millis() + 700;
  simonMsg = "Watch...";
  drawSimon();
}

static void simonNewGame() {
  simonLen = 1;
  for (auto& s : simonSeq) s = rnd(0, 3);
  simonShow();
}

static void enterSimon() {
  if (simonState == SimonState::Showing || simonState == SimonState::Input) simonShow();  // pick up where they were
  else { simonState = SimonState::Start; simonLit = -1; simonMsg = "Simon says!"; }
}

static void handleSimon(const m5::touch_detail_t& t) {
  int dir;
  if (flickedX(t, dir) && dir < 0) { buzz(); showScreen(Screen::PlayHome); return; }  // swipe right = back to the games
  if (!t.wasPressed()) return;  // react on touch-down, like real buttons
  if (simonState == SimonState::Start || simonState == SimonState::Over) { buzz(); simonNewGame(); return; }
  if (simonState != SimonState::Input) return;
  int pad = -1;
  for (int i = 0; i < 4; i++) {
    int x, y;
    simonPadRect(i, x, y);
    if (t.x >= x - 3 && t.x < x + SP_W + 3 && t.y >= y - 3 && t.y < y + SP_H + 3) pad = i;
  }
  if (pad < 0) return;
  simonInputAt = millis();
  if (pad != simonSeq[simonPos]) {  // wrong: show the score, offer another go
    buzz(2);
    chime(false);
    int score = simonLen - 1;
    if (score > simonBest) { simonBest = score; prefs.putUShort("simonbest", simonBest); }
    simonMsg = score ? "You got " + String(score) + "!" : String("Try again!");
    simonState = SimonState::Over;
    simonLit = simonSeq[simonPos];  // light the one it should have been
    drawSimon();
    return;
  }
  buzz();
  simonTone(pad, 250);
  if (simonLit >= 0 && simonLit != pad) drawSimonPad(simonLit, false);
  simonLit = pad;
  drawSimonPad(pad, true);
  simonNextAt = millis() + 250;  // unlight shortly
  if (++simonPos == simonLen) {  // round done: one more step next time
    simonMsg = simonLen >= 5 && simonLen % 5 == 0 ? "Wow! " + String(simonLen) + "!" : String("Great!");
    drawSimonHeader();
    if (simonLen < SIMON_MAX) simonLen++;
    simonState = SimonState::Showing;
    simonPos = 0;
    simonNextAt = millis() + 1100;
  }
}

static void serviceSimon() {
  uint32_t now = millis();
  if (simonState == SimonState::Showing && (int32_t)(now - simonNextAt) >= 0) {
    if (simonLit >= 0) {  // gap after a lit pad
      drawSimonPad(simonLit, false);
      simonLit = -1;
      simonNextAt = now + simonOffMs();
      if (simonPos >= simonLen) {  // shown it all: their turn
        simonState = SimonState::Input;
        simonPos = 0;
        simonInputAt = now;
        simonMsg = "Your turn!";
        drawSimonHeader();
      }
    } else {
      if (simonPos == 0) { simonMsg = "Watch..."; drawSimonHeader(); }
      simonLit = simonSeq[simonPos++];
      drawSimonPad(simonLit, true);
      simonTone(simonLit, simonOnMs());
      simonNextAt = now + simonOnMs();
    }
  } else if (simonState == SimonState::Input) {
    if (simonLit >= 0 && (int32_t)(now - simonNextAt) >= 0) { drawSimonPad(simonLit, false); simonLit = -1; }
    if (now - simonInputAt > 7000) simonShow();  // stuck? show it again, no penalty
  }
}

// ================= Memory Match =================
// Flip two cards; matching shapes stay up. Level 1 is 3 pairs, up to 8 pairs (4x4).
// Every board ends in a celebration; a tidy one (few extra flips) moves up a level.
static const uint8_t MEM_COLS[4] = {3, 4, 4, 4}, MEM_ROWS[4] = {2, 2, 3, 4};
static int memLevel = 1, memCards = 0, memMoves = 0, memFirst = -1, memSecond = -1;
static uint8_t memFace[16];       // pair id per card
static bool memUp[16], memDone[16];
static uint32_t memHideAt = 0, memWinAt = 0;
static bool memWon = false;

static void memCardRect(int i, int& x, int& y, int& w, int& h) {
  int cols = MEM_COLS[memLevel - 1], rows = MEM_ROWS[memLevel - 1];
  w = (312 - (cols - 1) * 6) / cols;
  h = (210 - (rows - 1) * 6) / rows;
  x = 4 + (i % cols) * (w + 6);
  y = TOP_H + 5 + (i / cols) * (h + 6);
}

static void drawMemCard(int i) {
  auto& d = M5.Display;
  int x, y, w, h;
  memCardRect(i, x, y, w, h);
  d.fillRect(x, y, w, h + 3, C_BG);
  d.fillRoundRect(x, y + 3, w, h, 10, C_SHADE);
  if (memUp[i] || memDone[i]) {
    uint16_t face = rgb(245, 242, 232);
    d.fillRoundRect(x, y, w, h, 10, face);
    if (memDone[i]) for (int t = 0; t < 2; t++) d.drawRoundRect(x + t, y + t, w - 2 * t, h - 2 * t, 10, C_GREEN);
    int f = memFace[i];
    drawShape(f % SHAPES, (f * 5) % SHAPE_COLORS, x + w / 2, y + h / 2, std::min(w, h) * 32 / 100, face);
  } else {
    d.fillRoundRect(x, y, w, h, 10, rgb(110, 70, 200));
    drawStar(x + w / 2, y + h / 2, std::min(w, h) / 5, rgb(170, 140, 240));
  }
}

static void drawMemory() {
  M5.Display.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
  for (int i = 0; i < memCards; i++) drawMemCard(i);
  if (memWon) {
    auto& d = M5.Display;
    d.fillRoundRect(40, 86, 240, 70, 16, rgb(20, 20, 40));
    d.setFont(&fonts::FreeSansBold18pt7b);
    d.setTextDatum(middle_center);
    d.setTextColor(C_YELLOW);
    d.drawString("You did it!", 160, 110);
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextColor(C_WHITE);
    d.drawString(String(memMoves) + " flips. Tap for more!", 160, 140);
  }
}

static void memNewBoard() {
  int pairs = MEM_COLS[memLevel - 1] * MEM_ROWS[memLevel - 1] / 2;
  memCards = pairs * 2;
  int start = rnd(0, SHAPES - 1);  // which shapes, varied board to board
  for (int i = 0; i < memCards; i++) memFace[i] = (start + i / 2) % SHAPES + (i / 2 >= SHAPES ? SHAPES : 0);
  for (int i = memCards - 1; i > 0; i--) std::swap(memFace[i], memFace[rnd(0, i)]);
  for (int i = 0; i < memCards; i++) memUp[i] = memDone[i] = false;
  memMoves = 0;
  memFirst = memSecond = -1;
  memHideAt = memWinAt = 0;
  memWon = false;
}

static void enterMemory() { if (!memCards || memWon) memNewBoard(); }

static void handleMemory(const m5::touch_detail_t& t) {
  int dir;
  if (flickedX(t, dir) && dir < 0) { buzz(); showScreen(Screen::PlayHome); return; }
  if (!t.wasClicked()) return;
  if (memWon) { if (millis() - memWinAt > 800) { buzz(); memNewBoard(); drawMemory(); } return; }
  if (memHideAt) {  // tapping while a wrong pair shows: flip them back now
    memUp[memFirst] = memUp[memSecond] = false;
    drawMemCard(memFirst); drawMemCard(memSecond);
    memFirst = memSecond = -1;
    memHideAt = 0;
  }
  for (int i = 0; i < memCards; i++) {
    int x, y, w, h;
    memCardRect(i, x, y, w, h);
    if (t.x < x || t.x >= x + w || t.y < y || t.y >= y + h || memUp[i] || memDone[i]) continue;
    buzz();
    memUp[i] = true;
    drawMemCard(i);
    if (memFirst < 0) { memFirst = i; return; }
    memSecond = i;
    memMoves++;
    if (memFace[memFirst] == memFace[memSecond]) {
      memDone[memFirst] = memDone[memSecond] = true;
      drawMemCard(memFirst); drawMemCard(memSecond);
      memFirst = memSecond = -1;
      chime(true);
      bool all = true;
      for (int k = 0; k < memCards; k++) all &= memDone[k];
      if (all) {
        int pairs = memCards / 2;
        if (memMoves <= pairs * 2 && memLevel < 4) { memLevel++; prefs.putUChar("memlvl", memLevel); }
        memWon = true;
        memWinAt = millis();
        quizFxUntil = millis() + 1500;  // confetti
        drawMemory();
      }
    } else {
      memHideAt = millis() + 1000;
    }
    return;
  }
}

static void serviceMemory() {
  if (memHideAt && (int32_t)(millis() - memHideAt) >= 0) {
    memUp[memFirst] = memUp[memSecond] = false;
    drawMemCard(memFirst); drawMemCard(memSecond);
    memFirst = memSecond = -1;
    memHideAt = 0;
  }
  if (quizFxUntil && millis() - quizLastFx > 40) {  // confetti around the "You did it!" box
    quizLastFx = millis();
    static const uint16_t conf[] = {0xF8A0, 0x07E0, 0x041F, 0xFFE0, 0xF81F, 0x07FF};
    for (int k = 0; k < 6; k++) {
      int y = rnd(TOP_H + 4, 236);
      if (y > 80 && y < 160) continue;
      M5.Display.fillRect(rnd(4, 312), y, rnd(3, 7), rnd(3, 7), conf[rnd(0, 5)]);
    }
    if ((int32_t)(millis() - quizFxUntil) >= 0) quizFxUntil = 0;
  }
}

// ================= Tilt Maze =================
// Tilt the Core2 to roll the ball to the star. The tilt is measured relative to how
// it's held when the maze starts. Mazes are random, bigger each level.
// The motion sensor's axes vs the (upside-down) screen: set TILT_* if the ball moves wrong.
static constexpr int TILT_X_AXIS = 0, TILT_Y_AXIS = 1;  // 0 = ax, 1 = ay
static constexpr float TILT_X_SIGN = 1.0f, TILT_Y_SIGN = -1.0f;
static const uint8_t MAZE_COLS[5] = {4, 5, 7, 9, 11}, MAZE_ROWS[5] = {3, 4, 5, 6, 7};
static constexpr int MAZE_MAX_C = 11, MAZE_MAX_R = 7;
static uint8_t mazeWall[MAZE_MAX_R][MAZE_MAX_C];  // bits: 1 N, 2 E, 4 S, 8 W
static int mazeLevel = 1, mazeCols = 4, mazeRows = 3, mazeCell = 50, mazeX0 = 0, mazeY0 = 0, mazeR = 10;
static float ballX, ballY, ballVX, ballVY, tiltBaseX = 0, tiltBaseY = 0;
static int ballDrawnX = -1000, ballDrawnY = -1000;
static uint32_t mazeStartAt = 0, mazeWinAt = 0, mazeLastStep = 0;
static bool mazeWon = false, mazeCalibrated = false;

static void mazeGenerate() {
  mazeCols = MAZE_COLS[mazeLevel - 1];
  mazeRows = MAZE_ROWS[mazeLevel - 1];
  mazeCell = std::min(312 / mazeCols, 210 / mazeRows);
  mazeX0 = 160 - mazeCols * mazeCell / 2;
  mazeY0 = TOP_H + 4 + (212 - mazeRows * mazeCell) / 2;
  mazeR = std::max(5, mazeCell * 26 / 100);
  for (int r = 0; r < mazeRows; r++) for (int c = 0; c < mazeCols; c++) mazeWall[r][c] = 15;
  // depth-first carve from the start cell
  static uint8_t stack[MAZE_MAX_C * MAZE_MAX_R];
  static bool seen[MAZE_MAX_R][MAZE_MAX_C];
  memset(seen, 0, sizeof seen);
  int sp = 0;
  stack[sp++] = 0;
  seen[0][0] = true;
  static const int DR[4] = {-1, 0, 1, 0}, DC[4] = {0, 1, 0, -1}, BIT[4] = {1, 2, 4, 8}, OPP[4] = {4, 8, 1, 2};
  while (sp) {
    int cur = stack[sp - 1], r = cur / mazeCols, c = cur % mazeCols;
    int opts[4], n = 0;
    for (int k = 0; k < 4; k++) {
      int nr = r + DR[k], nc = c + DC[k];
      if (nr >= 0 && nr < mazeRows && nc >= 0 && nc < mazeCols && !seen[nr][nc]) opts[n++] = k;
    }
    if (!n) { sp--; continue; }
    int k = opts[rnd(0, n - 1)], nr = r + DR[k], nc = c + DC[k];
    mazeWall[r][c] &= ~BIT[k];
    mazeWall[nr][nc] &= ~OPP[k];
    seen[nr][nc] = true;
    stack[sp++] = nr * mazeCols + nc;
  }
  ballX = mazeX0 + mazeCell / 2.0f;
  ballY = mazeY0 + mazeCell / 2.0f;
  ballVX = ballVY = 0;
  ballDrawnX = ballDrawnY = -1000;
  mazeWon = false;
  mazeCalibrated = false;
  mazeStartAt = millis();
}

static void drawMazeBall(bool erase) {
  auto& d = M5.Display;
  if (erase && ballDrawnX > -1000) d.fillCircle(ballDrawnX, ballDrawnY, mazeR, C_BG);
  ballDrawnX = (int)ballX;
  ballDrawnY = (int)ballY;
  d.fillCircle(ballDrawnX, ballDrawnY, mazeR, rgb(255, 120, 60));
  d.fillCircle(ballDrawnX - mazeR / 3, ballDrawnY - mazeR / 3, std::max(1, mazeR / 4), rgb(255, 210, 170));
}

static void drawMaze() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
  int gx = mazeX0 + (mazeCols - 1) * mazeCell + mazeCell / 2, gy = mazeY0 + (mazeRows - 1) * mazeCell + mazeCell / 2;
  drawStar(gx, gy, mazeCell * 32 / 100, C_YELLOW);
  uint16_t wc = rgb(120, 200, 255);
  for (int r = 0; r < mazeRows; r++)
    for (int c = 0; c < mazeCols; c++) {
      int x = mazeX0 + c * mazeCell, y = mazeY0 + r * mazeCell;
      uint8_t w = mazeWall[r][c];
      if (w & 1) d.fillRect(x - 1, y - 1, mazeCell + 3, 3, wc);
      if (w & 8) d.fillRect(x - 1, y - 1, 3, mazeCell + 3, wc);
      if (r == mazeRows - 1 && (w & 4)) d.fillRect(x - 1, y + mazeCell - 1, mazeCell + 3, 3, wc);
      if (c == mazeCols - 1 && (w & 2)) d.fillRect(x + mazeCell - 1, y - 1, 3, mazeCell + 3, wc);
      if (r < mazeRows - 1 && (w & 4)) d.fillRect(x - 1, y + mazeCell - 1, mazeCell + 3, 3, wc);
      if (c < mazeCols - 1 && (w & 2)) d.fillRect(x + mazeCell - 1, y - 1, 3, mazeCell + 3, wc);
    }
  ballDrawnX = -1000;
  drawMazeBall(false);
  if (mazeWon) {
    d.fillRoundRect(50, 92, 220, 56, 16, rgb(20, 20, 40));
    d.setFont(&fonts::FreeSansBold18pt7b);
    d.setTextDatum(middle_center);
    d.setTextColor(C_YELLOW);
    d.drawString("You made it!", 160, 120);
  }
}

static void readTilt(float& tx, float& ty) {
  float a[3] = {0, 0, 0};
  M5.Imu.getAccel(&a[0], &a[1], &a[2]);
  tx = a[TILT_X_AXIS] * TILT_X_SIGN;
  ty = a[TILT_Y_AXIS] * TILT_Y_SIGN;
}

static void enterMaze() { if (!mazeStartAt || mazeWon) mazeGenerate(); else mazeCalibrated = false; }

static void handleMaze(const m5::touch_detail_t& t) {
  int dir;
  if (flickedX(t, dir) && dir < 0) { buzz(); showScreen(Screen::PlayHome); return; }
  if (t.wasClicked() && mazeWon && millis() - mazeWinAt > 800) { buzz(); mazeGenerate(); drawMaze(); }
}

static void serviceMaze() {
  uint32_t now = millis();
  if (mazeWon) {
    if (now - mazeWinAt > 2500) { mazeGenerate(); drawMaze(); }  // next maze on its own
    return;
  }
  if (now - mazeLastStep < 16) return;
  float dt = std::min(0.05f, (now - mazeLastStep) / 1000.0f);
  mazeLastStep = now;
  float tx, ty;
  readTilt(tx, ty);
  if (!mazeCalibrated) {  // the way they're holding it now counts as level
    tiltBaseX = tx; tiltBaseY = ty; mazeCalibrated = true;
    return;
  }
  lastTouchMs = now;  // tilting is playing: don't dim
  float ax = (tx - tiltBaseX), ay = (ty - tiltBaseY);
  if (fabsf(ax) < 0.04f) ax = 0;  // small dead zone so it can rest
  if (fabsf(ay) < 0.04f) ay = 0;
  float accel = mazeCell * 14.0f;  // px/s^2 per g
  ballVX = (ballVX + ax * accel * dt) * 0.97f;
  ballVY = (ballVY + ay * accel * dt) * 0.97f;
  float maxV = mazeCell * 6.0f;
  ballVX = constrain(ballVX, -maxV, maxV);
  ballVY = constrain(ballVY, -maxV, maxV);
  // move in small steps, stopping at walls of the cell the ball is in
  int steps = 1 + (int)(std::max(fabsf(ballVX), fabsf(ballVY)) * dt / (mazeR * 0.5f));
  for (int s = 0; s < steps; s++) {
    float nx = ballX + ballVX * dt / steps, ny = ballY + ballVY * dt / steps;
    int c = constrain((int)((ballX - mazeX0) / mazeCell), 0, mazeCols - 1);
    int r = constrain((int)((ballY - mazeY0) / mazeCell), 0, mazeRows - 1);
    uint8_t w = mazeWall[r][c];
    float left = mazeX0 + c * mazeCell + mazeR + 2, right = mazeX0 + (c + 1) * mazeCell - mazeR - 2;
    float top = mazeY0 + r * mazeCell + mazeR + 2, bottom = mazeY0 + (r + 1) * mazeCell - mazeR - 2;
    if ((w & 8) && nx < left) { nx = left; ballVX = -ballVX * 0.3f; }
    if ((w & 2) && nx > right) { nx = right; ballVX = -ballVX * 0.3f; }
    if ((w & 1) && ny < top) { ny = top; ballVY = -ballVY * 0.3f; }
    if ((w & 4) && ny > bottom) { ny = bottom; ballVY = -ballVY * 0.3f; }
    // passing through an open side: stay clear of the wall stubs at its corners
    bool outX = nx < mazeX0 + c * mazeCell + mazeR || nx > mazeX0 + (c + 1) * mazeCell - mazeR;
    bool outY = ny < mazeY0 + r * mazeCell + mazeR || ny > mazeY0 + (r + 1) * mazeCell - mazeR;
    if (outX && outY) { nx = ballX; ny = ballY; ballVX *= 0.5f; ballVY *= 0.5f; }
    ballX = nx;
    ballY = ny;
  }
  if (abs((int)ballX - ballDrawnX) + abs((int)ballY - ballDrawnY) >= 1) drawMazeBall(true);
  int c = (int)((ballX - mazeX0) / mazeCell), r = (int)((ballY - mazeY0) / mazeCell);
  if (c == mazeCols - 1 && r == mazeRows - 1) {  // reached the star
    mazeWon = true;
    mazeWinAt = now;
    buzz();
    chime(true);
    if (mazeLevel < 5) { mazeLevel++; prefs.putUChar("mazelvl", mazeLevel); }
    drawMaze();
  }
}

// ================= Whack-a-Mole =================
// 30-second rounds: tap moles as they pop up. They come faster as the score climbs.
static constexpr int MOLES = 9, MOLE_ROUND_MS = 30000;
static constexpr int MOLE_Y0 = 54, MOLE_CW = 106, MOLE_CH = 62;
enum class MoleState { Start, Playing, Over };
static MoleState moleState = MoleState::Start;
static uint32_t moleUpUntil[MOLES], moleBonkUntil[MOLES], moleRoundEnd = 0, moleNextSpawn = 0;
static int moleScore = 0, moleBest = 0;
static bool moleShown[MOLES];

static void moleCenter(int i, int& cx, int& cy) {
  cx = 53 + (i % 3) * MOLE_CW;
  cy = MOLE_Y0 + (i / 3) * MOLE_CH + 40;  // hole line
}

static void drawMoleCell(int i) {
  auto& d = M5.Display;
  int cx, cy;
  moleCenter(i, cx, cy);
  uint16_t grass = rgb(60, 140, 70);
  d.fillRect(cx - MOLE_CW / 2, cy - 40, MOLE_CW, MOLE_CH, grass);
  bool up = moleUpUntil[i] != 0, bonk = moleBonkUntil[i] != 0;
  d.fillEllipse(cx, cy + 6, 36, 11, rgb(60, 35, 20));  // hole
  if (up || bonk) {
    uint16_t fur = rgb(150, 100, 60);
    d.fillRect(cx - 22, cy - 18, 45, 24, fur);
    d.fillCircle(cx, cy - 18, 22, fur);
    d.fillEllipse(cx, cy - 8, 11, 7, rgb(235, 200, 160));  // snout
    d.fillCircle(cx, cy - 12, 3, rgb(60, 30, 30));          // nose
    if (bonk) {  // X eyes and a star
      for (int s = -1; s <= 1; s += 2) {
        d.drawLine(cx + s * 9 - 3, cy - 27, cx + s * 9 + 3, cy - 21, C_SHADE);
        d.drawLine(cx + s * 9 - 3, cy - 21, cx + s * 9 + 3, cy - 27, C_SHADE);
      }
      drawStar(cx + 20, cy - 36, 7, C_YELLOW);
    } else {
      d.fillCircle(cx - 9, cy - 24, 3, C_SHADE);
      d.fillCircle(cx + 9, cy - 24, 3, C_SHADE);
    }
    d.fillEllipse(cx, cy + 6, 36, 5, rgb(60, 35, 20));  // hole lip in front of the mole
  }
  moleShown[i] = up || bonk;
}

static void drawMoleHeader() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H, 320, MOLE_Y0 - TOP_H, C_BG);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(middle_left);
  d.setTextColor(C_WHITE);
  d.drawString(String(moleScore), 10, TOP_H + 16);
  if (moleState == MoleState::Playing) {
    int left = std::max<int32_t>(0, (int32_t)(moleRoundEnd - millis()));
    d.fillRoundRect(60, TOP_H + 10, 160, 12, 6, C_ROW);
    d.fillRoundRect(60, TOP_H + 10, std::max(12, 160 * left / MOLE_ROUND_MS), 12, 6, left < 5000 ? rgb(255, 90, 90) : C_GREEN);
  }
  drawStar(240, TOP_H + 16, 9, C_YELLOW);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.drawString("Best " + String(moleBest), 254, TOP_H + 17);
}

static void drawMoles() {
  M5.Display.fillRect(0, TOP_H, 320, 240 - TOP_H, rgb(60, 140, 70));
  drawMoleHeader();
  for (int i = 0; i < MOLES; i++) drawMoleCell(i);
  if (moleState != MoleState::Playing) {
    auto& d = M5.Display;
    d.fillRoundRect(60, 104, 200, 70, 16, rgb(20, 20, 40));
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(middle_center);
    d.setTextColor(C_YELLOW);
    d.drawString(moleState == MoleState::Start ? "Whack the moles!" : "You got " + String(moleScore) + "!", 160, 126);
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextColor(C_WHITE);
    d.drawString("Tap to start", 160, 154);
  }
}

static void moleStart() {
  for (int i = 0; i < MOLES; i++) moleUpUntil[i] = moleBonkUntil[i] = 0;
  moleScore = 0;
  moleState = MoleState::Playing;
  moleRoundEnd = millis() + MOLE_ROUND_MS;
  moleNextSpawn = millis() + 600;
  drawMoles();
}

static void enterMoles() { if (moleState == MoleState::Playing) moleState = MoleState::Over; }

static void handleMoles(const m5::touch_detail_t& t) {
  int dir;
  if (flickedX(t, dir) && dir < 0 && moleState != MoleState::Playing) { buzz(); showScreen(Screen::PlayHome); return; }
  if (!t.wasPressed()) return;
  if (moleState != MoleState::Playing) { buzz(); moleStart(); return; }
  for (int i = 0; i < MOLES; i++) {
    if (!moleUpUntil[i]) continue;
    int cx, cy;
    moleCenter(i, cx, cy);
    if (abs(t.x - cx) > 40 || t.y < cy - 52 || t.y > cy + 18) continue;
    moleUpUntil[i] = 0;
    moleBonkUntil[i] = millis() + 300;
    moleScore++;
    buzz();
    if (!btConnected && !musicIsPlaying()) M5.Speaker.tone(880, 60, 1, true);
    drawMoleCell(i);
    drawMoleHeader();
    return;
  }
}

static void serviceMoles() {
  if (moleState != MoleState::Playing) return;
  uint32_t now = millis();
  lastTouchMs = now;
  for (int i = 0; i < MOLES; i++) {
    if (moleUpUntil[i] && (int32_t)(now - moleUpUntil[i]) >= 0) { moleUpUntil[i] = 0; drawMoleCell(i); }
    if (moleBonkUntil[i] && (int32_t)(now - moleBonkUntil[i]) >= 0) { moleBonkUntil[i] = 0; drawMoleCell(i); }
  }
  if ((int32_t)(now - moleNextSpawn) >= 0) {
    int speed = std::min(moleScore, 30);  // faster as they score
    int up = 1500 - speed * 25, gap = 900 - speed * 15;
    int maxUp = moleScore < 10 ? 1 : moleScore < 20 ? 2 : 3, nUp = 0;
    for (int i = 0; i < MOLES; i++) nUp += moleUpUntil[i] != 0;
    if (nUp < maxUp) {
      for (int tries = 0; tries < 12; tries++) {
        int i = rnd(0, MOLES - 1);
        if (moleUpUntil[i] || moleBonkUntil[i]) continue;
        moleUpUntil[i] = now + up;
        drawMoleCell(i);
        break;
      }
    }
    moleNextSpawn = now + gap + rnd(0, 300);
  }
  static uint32_t lastBar = 0;
  if (now - lastBar > 250) { lastBar = now; drawMoleHeader(); }
  if ((int32_t)(now - moleRoundEnd) >= 0) {  // time's up
    moleState = MoleState::Over;
    if (moleScore > moleBest) { moleBest = moleScore; prefs.putUShort("molebest", moleBest); }
    chime(true);
    buzz(2);
    for (int i = 0; i < MOLES; i++) moleUpUntil[i] = moleBonkUntil[i] = 0;
    drawMoles();
  }
}

// ================= Play: the game picker (middle button) =================
// A 3x2 grid of big game tiles (icon and name), swipe up/down for more. Inside a game, swipe right (or the middle button) comes back here.
struct GameTile { const char* name; const char* icon; uint8_t r, g, b; Screen screen; };  // icon nullptr = drawn
static const GameTile GAMES[] = {
    {"Missing Letter", "c_t", 255, 89, 140, Screen::Spell},
    {"Right Spelling", "Aa", 0, 170, 170, Screen::Words},
    {"Telling Time", nullptr, 255, 146, 76, Screen::Clock},
    {"Simon", nullptr, 60, 66, 110, Screen::Simon},
    {"Memory Match", nullptr, 110, 70, 200, Screen::Memory},
    {"What Comes Next?", nullptr, 25, 150, 220, Screen::Pattern},
    {"Tilt Maze", nullptr, 0, 150, 120, Screen::Maze},
    {"Whack-a-Mole", nullptr, 60, 140, 70, Screen::Moles},
    {"Math", "1+2", 138, 80, 236, Screen::Math},
};
static constexpr int GAME_COUNT = sizeof GAMES / sizeof *GAMES;
static constexpr int GT_Y = 26, GT_W = 100, GT_H = 100, GT_GAP = 7, GT_PER_PAGE = 6;
static int gamesPage = 0;
static int gamePages() { return (GAME_COUNT + GT_PER_PAGE - 1) / GT_PER_PAGE; }

static void gameTileRect(int i, int& x, int& y) {  // i: position on the page
  x = 3 + (i % 3) * (GT_W + GT_GAP);
  y = GT_Y + (i / 3) * (GT_H + GT_GAP);
}

static void drawPlayHome() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
  for (int k = 0; k < GT_PER_PAGE; k++) {
    int i = gamesPage * GT_PER_PAGE + k;
    if (i >= GAME_COUNT) break;
    const GameTile& g = GAMES[i];
    int x, y;
    gameTileRect(k, x, y);
    int cx = x + GT_W / 2;
    d.fillRoundRect(x, y + 3, GT_W, GT_H, 16, C_SHADE);
    d.fillRoundRect(x, y, GT_W, GT_H, 16, rgb(g.r, g.g, g.b));
    int iy = y + 34;  // icon, with the name (wrapped) below it
    if (g.icon) {
      d.setFont(&fonts::FreeSansBold24pt7b);
      d.setTextDatum(middle_center);
      d.setTextColor(C_WHITE);
      d.drawString(g.icon, cx, iy);
    } else if (g.screen == Screen::Clock) {  // a little clock face
      d.fillCircle(cx, iy, 21, C_WHITE);
      d.drawWideLine(cx, iy, cx, iy - 15, 2.5f, rgb(g.r, g.g, g.b));
      d.drawWideLine(cx, iy, cx + 10, iy + 4, 3.5f, rgb(g.r, g.g, g.b));
      d.fillCircle(cx, iy, 3, rgb(g.r, g.g, g.b));
    } else if (g.screen == Screen::Simon) {  // four little colored pads
      for (int p = 0; p < 4; p++) {
        const uint8_t* c = SIMON_COL[p];
        d.fillRoundRect(cx - 21 + (p % 2) * 22, iy - 21 + (p / 2) * 22, 20, 20, 5, rgb(c[0], c[1], c[2]));
      }
    } else if (g.screen == Screen::Memory) {  // two cards, one flipped
      d.fillRoundRect(cx - 26, iy - 18, 24, 34, 5, rgb(170, 140, 240));
      d.fillRoundRect(cx + 2, iy - 18, 24, 34, 5, rgb(245, 242, 232));
      drawShape(5, 0, cx + 14, iy - 1, 8, rgb(245, 242, 232));
    } else if (g.screen == Screen::Pattern) {  // circle, square, circle, ?
      drawShape(0, 0, cx - 33, iy, 9, rgb(g.r, g.g, g.b));
      drawShape(1, 3, cx - 11, iy, 9, rgb(g.r, g.g, g.b));
      drawShape(0, 0, cx + 11, iy, 9, rgb(g.r, g.g, g.b));
      d.setFont(&fonts::FreeSansBold12pt7b);
      d.setTextDatum(middle_center);
      d.setTextColor(C_WHITE);
      d.drawString("?", cx + 33, iy + 1);
    } else if (g.screen == Screen::Maze) {  // a tiny maze with a ball
      uint16_t wc = C_WHITE;
      d.drawRect(cx - 22, iy - 20, 44, 40, wc);
      d.drawFastVLine(cx - 8, iy - 20, 26, wc);
      d.drawFastVLine(cx + 7, iy - 6, 26, wc);
      d.drawFastHLine(cx + 7, iy - 6, 15, wc);
      d.fillCircle(cx - 15, iy - 13, 4, rgb(255, 120, 60));
      drawStar(cx + 15, iy + 13, 5, C_YELLOW);
    } else {  // a mole peeking out
      d.fillEllipse(cx, iy + 14, 24, 7, rgb(60, 35, 20));
      d.fillCircle(cx, iy - 2, 15, rgb(150, 100, 60));
      d.fillRect(cx - 15, iy - 2, 31, 14, rgb(150, 100, 60));
      d.fillEllipse(cx, iy + 4, 7, 5, rgb(235, 200, 160));
      d.fillCircle(cx - 6, iy - 6, 2, C_SHADE);
      d.fillCircle(cx + 6, iy - 6, 2, C_SHADE);
      d.fillEllipse(cx, iy + 14, 24, 3, rgb(60, 35, 20));
    }
    drawTitle(g.name, cx, y + 78, GT_W - 10, 38, C_WHITE, true);
  }
  int pages = gamePages();
  for (int pg = 0; pg < pages && pages > 1; pg++)
    d.fillCircle(160 - (pages - 1) * 6 + pg * 12, 237, 2, pg == gamesPage ? C_WHITE : C_DIM);
}

static void handlePlayHome(const m5::touch_detail_t& t) {
  int dir;
  if (flickedY(t, dir)) {  // swipe up = next page
    int pg = constrain(gamesPage + dir, 0, gamePages() - 1);
    if (pg != gamesPage) { buzz(); gamesPage = pg; drawPlayHome(); }
    return;
  }
  if (!t.wasClicked()) return;
  for (int k = 0; k < GT_PER_PAGE; k++) {
    int i = gamesPage * GT_PER_PAGE + k, x, y;
    if (i >= GAME_COUNT) break;
    gameTileRect(k, x, y);
    if (t.x >= x - 3 && t.x < x + GT_W + 3 && t.y >= y - 3 && t.y < y + GT_H + 3) { buzz(); showScreen(GAMES[i].screen); return; }
  }
}

// ================= Sleep (right button) =================
// A dim bedside screen: big clock with a moon (stay in bed) or sun (OK to get up),
// and three big buttons: sleep timer, calm sound (rain/ocean noise, generated, so it
// works over Bluetooth too), and night-light (when the screen dims, it glows softly
// instead of going dark). Pressing the Sleep button again starts/extends the timer.
// When the timer runs out, sound fades over 20 s and stops.
static uint32_t sleepTimerEnd = 0, fadeStart = 0;  // millis; 0 = off
static bool nightLight = false;
static int wakeMin = 7 * 60;  // "OK to wake" time, minutes after midnight (grown-ups setting)
static const int WAKE_OPTIONS[3] = {6 * 60 + 30, 7 * 60, 7 * 60 + 30};
static constexpr uint32_t FADE_MS = 20000;
static constexpr int SL_BTN_Y = 192, SL_BTN_R = 30;
static const int SL_BTN_X[3] = {60, 160, 260};
static const uint16_t SL_TIMER_STEPS[] = {0, 15, 30, 45, 60};

#define C_NIGHT rgb(255, 196, 140)  // warm, low-stimulation text color

static bool rtcNow(int& h, int& m) {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.isEnabled() || !M5.Rtc.getDateTime(&dt) || dt.date.year < 2024) return false;
  h = dt.time.hours;
  m = dt.time.minutes;
  return true;
}

static bool stayInBed() {  // moon from 6 pm until the wake-up time
  int h, m;
  if (!rtcNow(h, m)) return false;
  int now = h * 60 + m;
  return now < wakeMin || h >= 18;
}

static String clockText(int minutes) {
  int h = minutes / 60 % 24, m = minutes % 60, h12 = h % 12 ? h % 12 : 12;
  char buf[8];
  snprintf(buf, sizeof buf, "%d:%02d", h12, m);
  return buf;
}

static void drawMoon(int x, int y, int r, uint16_t bg) {
  M5.Display.fillCircle(x, y, r, rgb(255, 230, 150));
  M5.Display.fillCircle(x + r / 2, y - r / 3, r * 4 / 5, bg);
  M5.Display.fillCircle(x - r / 4, y + r / 6, 3, rgb(230, 200, 120));
}

static void drawSun(int x, int y, int r) {
  static const int8_t RAY[8][2] = {{0, -100}, {71, -71}, {100, 0}, {71, 71}, {0, 100}, {-71, 71}, {-100, 0}, {-71, -71}};
  uint16_t c = rgb(255, 200, 40);
  for (auto& v : RAY)
    M5.Display.fillCircle(x + v[0] * (r + 8) / 100, y + v[1] * (r + 8) / 100, 4, c);
  M5.Display.fillCircle(x, y, r * 3 / 4, c);
}

static int sleepMinutesLeft() {
  return sleepTimerEnd ? std::max<int32_t>(1, ((int32_t)(sleepTimerEnd - millis()) + 59999) / 60000) : 0;
}

static void drawSleepClock() {
  auto& d = M5.Display;
  d.fillRect(0, TOP_H, 320, SL_BTN_Y - SL_BTN_R - 6 - TOP_H, C_BG);
  bool moon = stayInBed();
  if (moon) drawMoon(58, 82, 34, C_BG);
  else drawSun(58, 82, 30);
  int h, m;
  d.setTextDatum(middle_center);
  d.setTextColor(C_NIGHT);
  if (rtcNow(h, m)) {
    d.setFont(&fonts::FreeSansBold24pt7b);
    d.setTextSize(1.6f);
    d.drawString(clockText(h * 60 + m), 200, 80);
    d.setTextSize(1);
  }
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(rgb(170, 150, 130));
  String line = moon ? "Stay in bed until " + clockText(wakeMin) : String("Good morning!");
  if (sleepTimerEnd) line = "Sound stops in " + String(sleepMinutesLeft()) + " min";
  if (fadeStart) line = "Shhh... goodnight";
  d.drawString(line, 160, 136);
}

static void iconTimer(int x, int y, uint16_t c) {
  M5.Display.drawCircle(x, y + 1, 11, c);
  M5.Display.drawCircle(x, y + 1, 10, c);
  M5.Display.fillRect(x - 1, y - 6, 3, 8, c);
  M5.Display.fillRect(x - 1, y, 7, 3, c);
  M5.Display.fillRect(x - 3, y - 14, 7, 3, c);
}
static void iconWaves(int x, int y, uint16_t c) {
  for (int k = -1; k <= 1; k++)
    for (int i = 0; i < 4; i++) M5.Display.fillArc(x - 9 + i * 6, y + k * 7, 4, 2, i % 2 ? 0 : 180, i % 2 ? 180 : 360, c);
}
static void iconBulb(int x, int y, uint16_t c, bool on) {
  if (on) M5.Display.fillCircle(x, y - 3, 13, rgb(120, 90, 30));
  M5.Display.fillCircle(x, y - 4, 9, c);
  M5.Display.fillRect(x - 4, y + 4, 9, 7, c);
}

static void drawSleepButtons() {
  auto& d = M5.Display;
  d.fillRect(0, SL_BTN_Y - SL_BTN_R - 6, 320, 240 - (SL_BTN_Y - SL_BTN_R - 6), C_BG);
  static const char* SOUND_NAMES[] = {"Quiet", "Rain", "Ocean"};
  bool on[3] = {sleepTimerEnd != 0, sleepSound != 0, nightLight};
  String label[3] = {sleepTimerEnd ? String(sleepMinutesLeft()) + " min" : String("Timer"), SOUND_NAMES[sleepSound],
                     nightLight ? "Light on" : "Light off"};
  for (int i = 0; i < 3; i++) {
    int x = SL_BTN_X[i];
    d.fillCircle(x, SL_BTN_Y, SL_BTN_R, on[i] ? rgb(70, 60, 130) : rgb(36, 40, 70));
    uint16_t ic = on[i] ? C_NIGHT : rgb(150, 140, 130);
    if (i == 0) iconTimer(x, SL_BTN_Y, ic);
    else if (i == 1) iconWaves(x, SL_BTN_Y, ic);
    else iconBulb(x, SL_BTN_Y, ic, on[i]);
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.setTextDatum(top_center);
    d.setTextColor(on[i] ? C_NIGHT : rgb(130, 120, 110));
    d.drawString(label[i], x, SL_BTN_Y + SL_BTN_R + 3);
  }
}

static void drawSleep() {
  M5.Display.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
  drawSleepClock();
  drawSleepButtons();
}

static void setSleepTimer(int minutes) {
  sleepTimerEnd = minutes ? millis() + minutes * 60000UL : 0;
  if (!sleepTimerEnd) fadeStart = 0;
}

// Same Sleep button again: start a 30 min timer, or add 15 (up to 90).
static void extendSleepTimer() { setSleepTimer(sleepTimerEnd ? std::min(90, sleepMinutesLeft() + 15) : 30); }

static void startSleepSound(int kind) {
  if (kind && musicIsPlaying()) togglePause();  // one sound at a time
  sleepSound = kind;
  if (!kind) out.overrideRate(0);
  if (kind) {
    M5.Speaker.stop(SPK_CH);
    ringFlushReq = true;
    out.begin();
    out.overrideRate(22050);
  }
}

static void handleSleep(const m5::touch_detail_t& t) {
  if (!t.wasClicked()) return;
  for (int i = 0; i < 3; i++) {
    int dx = t.x - SL_BTN_X[i], dy = t.y - SL_BTN_Y;
    if (dx * dx + dy * dy > (SL_BTN_R + 12) * (SL_BTN_R + 12)) continue;
    buzz();
    if (i == 0) {  // Off -> 15 -> 30 -> 45 -> 60 -> Off
      int cur = sleepTimerEnd ? sleepMinutesLeft() : 0, next = 0;
      for (uint16_t s : SL_TIMER_STEPS) if (s > cur) { next = s; break; }
      setSleepTimer(next);
    } else if (i == 1) {
      startSleepSound((sleepSound + 1) % 3);
    } else {
      nightLight = !nightLight;
      if (nightLight) {  // glow right away; a tap anywhere comes back to this screen
        dimmed = true;
        M5.Display.setBrightness(60);
        return;
      }
    }
    drawSleepClock();
    drawSleepButtons();
    return;
  }
}

// Generated calm sound: low-passed noise. Rain is lighter; ocean is deeper and swells.
static void serviceSleepSound() {
  static int32_t lp = 0;
  static uint32_t seed = 22222, swell = 0;
  if (!sleepSound || musicIsPlaying()) return;
  for (int k = 0; k < 256; k++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    int32_t white = (int16_t)(seed >> 16);
    int32_t v;
    if (sleepSound == 1) {
      lp += (white - lp) / 3;
      v = lp * 3 / 4;
    } else {
      lp += (white - lp) / 24;
      swell = (swell + 1) % (22050 * 9);  // a wave every 9 s
      int32_t tri = swell < 22050 * 9 / 2 ? swell : 22050 * 9 - swell;  // 0..99225
      v = lp * (900 + tri * 2400 / 99225) / 1000;  // 0.9x .. 3.3x
    }
    int16_t fr[2];
    fr[0] = fr[1] = (int16_t)constrain(v, -32000, 32000);
    if (!out.ConsumeSample(fr)) break;
  }
}

// Timer, fade-out, calm sound, night-light glow and the clock. Runs on every screen.
static void serviceSleep() {
  uint32_t now = millis();
  if (sleepTimerEnd && (int32_t)(now - sleepTimerEnd) >= 0) { sleepTimerEnd = 0; fadeStart = now; }
  static uint32_t lastFade = 0;
  if (fadeStart && now - lastFade > 250) {
    lastFade = now;
    uint32_t t = now - fadeStart;
    if (t >= FADE_MS) {  // done: stop everything, restore the normal volume for next time
      fadeStart = 0;
      if (musicIsPlaying()) togglePause();
      sleepSound = 0;
      out.overrideRate(0);
      M5.Speaker.stop(SPK_CH);
      ringFlushReq = true;
      applyVolume();
      if (screen == Screen::Sleep) { drawSleepClock(); drawSleepButtons(); }
    } else {
      uint8_t v = volume * (FADE_MS - t) / FADE_MS;
      M5.Speaker.setVolume(v);
#if !CORE2_WIFI
      a2dp.set_volume(v / 2);
#endif
    }
  }
  serviceSleepSound();
  if (screen != Screen::Sleep) return;
  static uint32_t lastGlow = 0;
  if (dimmed && nightLight) {  // slow warm color drift, ~2 minutes around the palette
    if (now - lastGlow > 200) {
      lastGlow = now;
      static const uint8_t pal[4][3] = {{255, 140, 60}, {255, 90, 120}, {150, 80, 220}, {255, 170, 90}};
      uint32_t p = now / 30 % 4000, i = p / 1000, f = p % 1000;
      auto mix = [&](int c) { return (uint8_t)((pal[i][c] * (1000 - f) + pal[(i + 1) % 4][c] * f) / 1000); };
      M5.Display.fillScreen(rgb(mix(0), mix(1), mix(2)));
    }
    return;
  }
  static int shownMinute = -1;
  int h, m;
  if (rtcNow(h, m) && h * 60 + m != shownMinute) {
    shownMinute = h * 60 + m;
    drawSleepClock();
    if (sleepTimerEnd) drawSleepButtons();
  }
}

// ================= Nightly podcast sync =================
// Once a night between midnight and 5 am, when it's plugged in and nothing is playing,
// the Core2 syncs podcasts. In Bluetooth mode it quietly reboots into WiFi mode, syncs,
// and reboots back. The screen stays dark (or keeps the night-light glowing) throughout.
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
  prefs.putUChar("nightscreen", screen == Screen::Sleep);
  prefs.putUChar("nightlight", nightLight);
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
    if (!musicIsPlaying() && !sleepSound && now - lastTouchMs > 60000) { nightReturnPending = false; nightReboot(false); }
    return;
  }
#endif
  if (now - last < 30000) return;
  last = now;
  int h, m;
  if (!rtcNow(h, m) || h >= 5) return;
  uint32_t today = todayStamp();
  if (!today || prefs.getUInt("nightdone", 0) == today) return;
  if (!usbPower() || musicIsPlaying() || sleepSound || sleepTimerEnd || fadeStart) return;
  if (!prefs.isKey("ssid") || !haveFeeds()) { prefs.putUInt("nightdone", today); return; }  // nothing to do tonight
#if CORE2_WIFI
  if (syncState == SyncState::Idle) { prefs.putUInt("nightdone", today); requestSync(); }
#else
  nightReboot(true);
#endif
}

// ================= Grown-ups (hold the top of the screen for 3 s) =================
// Header with Done, then three tabs: Settings, Bluetooth (WiFi in WiFi mode), Stats.
static int guTab = 0, statsPage = 0;
static Screen beforeGrownups = Screen::Player;
static int brightIdx = 1, autoOffIdx = 0;        // saved in prefs
static const uint8_t BRIGHTNESS[3] = {60, 140, 230};
static const uint16_t AUTO_OFF_MIN[3] = {15, 30, 0};  // 0 = never

static constexpr int GU_TABS_Y = 56, GU_TAB_H = 28, GU_BODY_Y = 90;
static constexpr int SEG_X = 118, SEG_W = 194, SEG_H = 26;  // settings: 3-way choices on the right
static const int GU_ROW_Y[4] = {92, 122, 152, 182};
static constexpr int GU_MODE_Y = 212;  // full-width mode switch button

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
      for (size_t a = 0; a < albums.size(); a++) songCountCache += (int)a == playAlbum ? tracks.size() : listTracks(a).size();
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
  } else if (statsPage == 1) {  // listening and the play-for-fun games
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
    drawStatTile(1, 1, "VOLUME", String(volLevel * 10) + "% of " + String(maxVolPct) + "% max", C_ORANGE);
    drawStatTile(0, 2, "SIMON", "Best " + String(simonBest) + " in a row", rgb(40, 200, 90));
    drawStatTile(1, 2, "WHACK-A-MOLE", "Best " + String(moleBest), rgb(150, 100, 60));
    drawStatTile(0, 3, "MEMORY MATCH", "Level " + String(memLevel) + " of 4", rgb(110, 70, 200));
    drawStatTile(1, 3, "TILT MAZE", "Level " + String(mazeLevel) + " of 5", rgb(0, 150, 120));
  } else if (statsPage == 2) {  // the learning games
    auto game = [](const Quiz& q) { return "Level " + String(q.level) + ", " + String(q.stars) + " stars"; };
    drawStatTile(0, 0, "MISSING LETTER", game(spellQuiz), rgb(255, 89, 140));
    drawStatTile(1, 0, "RIGHT SPELLING", game(wordsQuiz), rgb(0, 170, 170));
    drawStatTile(0, 1, "TELLING TIME", game(clockQuiz), C_ORANGE);
    drawStatTile(1, 1, "MATH", game(mathQuiz), rgb(138, 80, 236));
    drawStatTile(0, 2, "WHAT COMES NEXT?", game(patternQuiz), C_BLUE);
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
  for (int p = 0; p < 4; p++) d.fillCircle(136 + p * 16, 236, 3, p == statsPage ? C_WHITE : C_DIM);
}

static void drawGrownupsBody() {
  auto& d = M5.Display;
  d.fillRect(0, GU_BODY_Y, 320, 240 - GU_BODY_Y, C_BG);
  if (guTab == 0) {
    static const char* const vol[] = {"50%", "70%", "100%"};
    static const char* const bright[] = {"Dim", "Normal", "Bright"};
    static const char* const off[] = {"15 min", "30 min", "Never"};
    static const char* const wake[] = {"6:30", "7:00", "7:30"};
    const char* const* opts[4] = {vol, bright, off, wake};
    static const char* names[4] = {"Max volume", "Screen", "Auto-off", "Wake up"};
    int wakeSel = wakeMin == WAKE_OPTIONS[0] ? 0 : wakeMin == WAKE_OPTIONS[2] ? 2 : 1;
    int sel[4] = {maxVolPct == 50 ? 0 : maxVolPct == 70 ? 1 : 2, brightIdx, autoOffIdx, wakeSel};
    for (int r = 0; r < 4; r++) {
      d.setFont(&fonts::FreeSansBold9pt7b);
      d.setTextDatum(middle_left);
      d.setTextColor(C_WHITE);
      d.drawString(names[r], 10, GU_ROW_Y[r] + SEG_H / 2);
      drawSegments(SEG_X, GU_ROW_Y[r], SEG_W, SEG_H, opts[r], 3, sel[r], C_BLUE);
    }
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
  d.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(C_WHITE);
  d.setTextDatum(middle_left);
  d.drawString("Grown-ups", 10, TOP_H + 17);
  d.fillRoundRect(236, TOP_H + 4, 76, 27, 13, C_GREEN);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextDatum(middle_center);
  d.drawString("Done", 274, TOP_H + 18);
  static const char* const tabs[] = {"Settings", CORE2_WIFI ? "WiFi" : "Bluetooth", "Stats"};
  drawSegments(6, GU_TABS_Y, 308, GU_TAB_H, tabs, 3, guTab, rgb(138, 80, 236));
  drawGrownupsBody();
}

static void applyBrightness() { M5.Display.setBrightness(BRIGHTNESS[brightIdx]); }

static void handleGrownups(const m5::touch_detail_t& t) {
  if (statsPage >= 0 && guTab == 2 && t.wasFlicked() && abs(t.distanceX()) > 40) {
    buzz(); statsPage = (statsPage + 1) % 4; drawStats(); return;
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
    for (int r = 0; r < 4; r++) {
      int i = segmentHit(SEG_X, GU_ROW_Y[r], SEG_W, SEG_H, 3, t.x, t.y);
      if (i < 0) continue;
      buzz();
      if (r == 0) { maxVolPct = i == 0 ? 50 : i == 1 ? 70 : 100; prefs.putUChar("maxvol", maxVolPct); applyVolume(); }
      if (r == 1) { brightIdx = i; prefs.putUChar("bright", i); applyBrightness(); }
      if (r == 2) { autoOffIdx = i; prefs.putUChar("autooff", i); }
      if (r == 3) { wakeMin = WAKE_OPTIONS[i]; prefs.putUShort("wake", wakeMin); }
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
    statsPage = (statsPage + 1) % 4;
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

// ----- screen switching / redraw -----
static void showScreen(Screen s) {
  if (isQuiz(s)) enterQuiz(s);
  if (s == Screen::Simon) enterSimon();
  if (s == Screen::Memory) enterMemory();
  if (s == Screen::Maze) enterMaze();
  if (s == Screen::Moles) enterMoles();
  screen = s;
  dirtyAll = true;
}

static void redraw() {
  if (dirtyAll) {
    dirtyAll = false;
    dirtyHeader = true;
    if (screen == Screen::Player) {  // wipe everything; the card/progress/buttons don't tile the screen exactly
      M5.Display.fillRect(0, TOP_H, 320, 240 - TOP_H, C_BG);
      dirtyNow = true;
    }
    else if (screen == Screen::Albums) drawAlbums();
    else if (screen == Screen::Tracks) drawTracks();
    else if (isQuiz(screen)) drawQuiz(screen);
    else if (screen == Screen::PlayHome) drawPlayHome();
    else if (screen == Screen::Simon) drawSimon();
    else if (screen == Screen::Memory) drawMemory();
    else if (screen == Screen::Maze) drawMaze();
    else if (screen == Screen::Moles) drawMoles();
    else if (screen == Screen::Sleep) drawSleep();
    else { drawGrownups(); connectDirty = false; }
  }
  if (dirtyHeader) { drawTopBar(); dirtyHeader = false; }
  if (screen == Screen::Player && dirtyNow) {
    dirtyNow = false;
    drawCard();
    drawProgress();
    drawControls();
  }
  if (screen == Screen::Grownups && connectDirty) {  // Bluetooth news: only that tab shows it
    connectDirty = false;
    if (guTab == 1) drawGrownupsBody();
  }
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

static void handlePlayer(const m5::touch_detail_t& t) {
  int dir;
  if (t.base_y > CARD_Y && t.base_y < CARD_Y + CARD_H && flickedX(t, dir)) { buzz(); playTrack(current + dir); return; }
  if (!t.wasClicked()) return;
  if (hit(B_PLAY, t.x, t.y)) {
    buzz();
    if (current < 0) playAlbumTrack(0, 0);
    else togglePause();
  }
  else if (hit(B_PREV, t.x, t.y)) { buzz(); playTrack(current - 1); }
  else if (hit(B_NEXT, t.x, t.y)) { buzz(); playTrack(current + 1); }
  else if (hit(B_VOLDN, t.x, t.y)) { buzz(); setVolLevel(volLevel - 1); }
  else if (hit(B_VOLUP, t.x, t.y)) { buzz(); setVolLevel(volLevel + 1); }
  else if (t.y > CARD_Y && t.y < CARD_Y + CARD_H && !albums.empty()) {
    buzz();
    albumsPage = std::max(0, playAlbum) / TILES;
    showScreen(Screen::Albums);
  }
}

static void handleAlbums(const m5::touch_detail_t& t) {
  int dir;
  if (flickedY(t, dir)) {  // swipe up = next page
    int p = constrain(albumsPage + dir, 0, albumPages() - 1);
    if (p != albumsPage) { buzz(); albumsPage = p; dirtyAll = true; }
    return;
  }
  if (flickedX(t, dir) && dir < 0) { buzz(); showScreen(Screen::Player); return; }  // swipe right = back
  if (!t.wasClicked()) return;
  if (t.x < GRID_X - 3 || t.y < GRID_Y - 4) return;
  int col = (t.x - GRID_X + TILE_GAP_X / 2) / (TILE + TILE_GAP_X), row = (t.y - GRID_Y + TILE_GAP_Y / 2) / (TILE + TILE_GAP_Y);
  int a = albumsPage * TILES + row * 3 + col;
  if (col <= 2 && row <= 1 && a < (int)albums.size()) { buzz(); openTracks(a); }
}

static void handleTracks(const m5::touch_detail_t& t) {
  auto scroll = [&](int by) {
    int top = constrain(tracksTop + by, 0, std::max(0, (int)viewTracks.size() - trRows()));
    if (top != tracksTop) { buzz(); tracksTop = top; dirtyAll = true; }
  };
  auto back = [&] { buzz(); albumsPage = viewAlbum / TILES; showScreen(Screen::Albums); };
  int dir;
  if (flickedY(t, dir)) { scroll(dir * (trRows() - 1)); return; }  // keep one row for context
  if (flickedX(t, dir) && dir < 0) { back(); return; }            // swipe right = back
  if (!t.wasClicked()) return;
  if (t.y < TR_Y - 3) { if (t.y >= TOP_H) back(); return; }       // tap the album header = back
  int r = (t.y - TR_Y) / trRowH(), i = tracksTop + r;
  if (r >= trRows() || i >= (int)viewTracks.size()) return;
  buzz();
  if (viewAlbum == playAlbum && i == current) {  // the song that's already on: don't restart it
    if (paused) togglePause();
  } else {
    playAlbumTrack(viewAlbum, i);
  }
  showScreen(Screen::Player);
}

static void handleInput() {
  auto t = M5.Touch.getDetail();
  if (t.base_y < 0 || t.base_y >= 240) t = m5::touch_detail_t();  // touch-button strip, handled as BtnA..C
  bool anyInput = t.isPressed() || M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed();
  static bool wakeTouch = false;  // the touch that woke the screen; ignored until it lifts
  if (anyInput) {
    lastTouchMs = millis();
    if (dimmed) {
      dimmed = false;
      wakeTouch = true;
      applyBrightness();
      if (screen == Screen::Sleep) dirtyAll = true;  // replace the night-light glow
    }
  }
  if (wakeTouch) {  // otherwise its release would also "click" whatever was under it
    if (!anyInput) wakeTouch = false;
    return;
  }
  if (dimmed) return;

  // hold the top of the screen for 3 seconds -> grown-ups screen
  static bool holdArmed = false;
  static uint32_t lastHoldDraw = 0;
  bool listScreen = screen == Screen::Albums || screen == Screen::Tracks || screen == Screen::PlayHome ||
                    screen == Screen::Memory || screen == Screen::Maze || screen == Screen::Moles;  // content right under the bar
  if (t.isPressed() && t.base_y < (listScreen ? TOP_H + 2 : HOLD_ZONE) && screen != Screen::Grownups) {
    uint32_t held = millis() - t.base_msec;
    if (held > 500 && millis() - lastHoldDraw > 100) {  // quick taps stay quiet
      lastHoldDraw = millis();
      drawHoldProgress(held);
      holdArmed = true;
    }
    if (held >= HOLD_MS) {
      buzz();
      holdArmed = false;
      guTab = 0;
      statsPage = 0;
      beforeGrownups = screen == Screen::Tracks ? Screen::Albums : screen;  // the song list re-reads its album on open
      showScreen(Screen::Grownups);
      return;
    }
  } else if (holdArmed) {
    holdArmed = false;
    dirtyHeader = true;
  }

  // the three touch buttons pick the activity
  int pick = BTN_LEFT.wasClicked() ? 0 : M5.BtnB.wasClicked() ? 1 : BTN_RIGHT.wasClicked() ? 2 : -1;
  if (pick >= 0) {
    buzz();
    if (pick == 0) {  // Listen: from a list, back to the player; on the player, play/pause
      if (screen != Screen::Player) showScreen(Screen::Player);
      else if (current < 0) playAlbumTrack(0, 0);
      else togglePause();
    } else if (pick == 1) {  // Play: the game picker (also from inside a game)
      if (screen != Screen::PlayHome) showScreen(Screen::PlayHome);
    } else {  // Sleep; pressed again there: start/extend the sleep timer
      if (screen != Screen::Sleep) showScreen(Screen::Sleep);
      else { extendSleepTimer(); drawSleepClock(); drawSleepButtons(); }
    }
    return;
  }

  switch (screen) {
    case Screen::Spell:
    case Screen::Math:
    case Screen::Clock:
    case Screen::Words:
    case Screen::Pattern: handleQuiz(screen, t); break;
    case Screen::Memory: handleMemory(t); break;
    case Screen::Maze: handleMaze(t); break;
    case Screen::Moles: handleMoles(t); break;
    case Screen::PlayHome: handlePlayHome(t); break;
    case Screen::Simon: handleSimon(t); break;
    case Screen::Sleep: handleSleep(t); break;
    case Screen::Player: handlePlayer(t); break;
    case Screen::Albums: handleAlbums(t); break;
    case Screen::Tracks: handleTracks(t); break;
    case Screen::Grownups: handleGrownups(t); break;
  }
}

// Dim after 2 minutes untouched; power off after 15 minutes paused on battery.
static void handleIdle() {
  uint32_t idle = millis() - lastTouchMs;
  bool sleepPlace = screen == Screen::Sleep;
  if (!dimmed && idle > (sleepPlace ? 20000UL : 120000UL)) {
    dimmed = true;
    M5.Display.setBrightness(sleepPlace ? (nightLight ? 60 : 8) : 20);
  }
  bool playing = (!paused && current >= 0) || sleepSound || sleepTimerEnd || fadeStart || (nightLight && sleepPlace);
  uint32_t offMin = AUTO_OFF_MIN[autoOffIdx];
  if (offMin && !playing && idle > offMin * 60UL * 1000 && !usbPower())
    M5.Power.Axp192.powerOff();  // straight to the PMIC; M5.Power.powerOff() drags deep-sleep code into IRAM
}

// ---------- serial commands ----------
// Line protocol at 460800 baud (driven by upload.py):
//   PING                 -> PONG bt | PONG wifi [<ip>]
//   BAT                  -> BAT <percent> <charging|discharging> <millivolts> <milliamps>
//   VER                  -> VER <version> <bt|wifi>
//   IMU                  -> IMU <ax> <ay> <az> in hundredths of g (Tilt Maze axis check)
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
// The WiFi build prints "IP <addr>" when it joins the network.
static constexpr size_t UP_CHUNK = 2048;

static void showUploadStatus(const String& name, uint32_t done, uint32_t total) {
  auto& d = M5.Display;
  d.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 18, C_OFF);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(C_WHITE);
  d.setTextDatum(middle_center);
  d.drawString("Adding songs...", 160, CARD_Y + 30);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.drawString(fitLine(name.substring(name.lastIndexOf('/') + 1), CARD_W - 30), 160, CARD_Y + 60);
  d.fillRoundRect(26, CARD_Y + 84, 268, 14, 7, C_ROW);
  d.fillRoundRect(26, CARD_Y + 84, std::max<int>(14, total ? (int64_t)268 * done / total : 0), 14, 7, C_YELLOW);
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

static void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line == "IMU") {  // accelerometer in g, for checking the Tilt Maze axes
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    Serial.printf("IMU %d %d %d\n", (int)(ax * 100), (int)(ay * 100), (int)(az * 100));
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
  } else if (line == "RESCAN") {
    rescanLibrary();
    Serial.printf("OK %u\n", (unsigned)albums.size());
    if (!albums.empty()) playAlbumTrack(0, 0);
    showScreen(Screen::Player);
  } else if (line.startsWith("MV ")) {
    int tab = line.indexOf('\t');
    if (tab < 0) { Serial.println("ERR bad args"); return; }
    String from = line.substring(3, tab), to = line.substring(tab + 1);
    stopPlayback();
    int slash = to.lastIndexOf('/');
    if (slash > 0) SD.mkdir(to.substring(0, slash));
    Serial.println(SD.rename(from, to) ? "OK" : "ERR move failed");
  } else if (line.startsWith("RM ")) {
    stopPlayback();
    Serial.println(removePath(line.substring(3)) ? "OK" : "ERR remove failed");
  } else if (line.startsWith("TIME ")) {  // local time from the host, e.g. upload.py
    int v[7] = {0};
    if (sscanf(line.c_str() + 5, "%d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7) {
      Serial.println("ERR bad args");
      return;
    }
    M5.Rtc.setDateTime(m5::rtc_datetime_t{{(int16_t)v[0], (int8_t)v[1], (int8_t)v[2], (int8_t)v[6]}, {(int8_t)v[3], (int8_t)v[4], (int8_t)v[5]}});
    Serial.println("OK");
    dirtyHeader = true;
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
    dirtyNow = true;
  }
}

void setup() {
  Serial.setRxBufferSize(UP_CHUNK * 4);
  auto cfg = M5.config();
  cfg.serial_baudrate = 460800;  // the Core2's CH9102 garbles 921600
  M5.begin(cfg);
  M5.Display.setRotation(ROTATION);
  prefs.begin("core2mp3");  // settings namespace from the project's first name; renaming it would reset them
  bool nightBoot = prefs.getUChar("nightboot", 0);  // a quiet reboot for the nightly podcast sync
  M5.Display.setBrightness(nightBoot ? 0 : 140);   // the saved brightness is applied below
  M5.Display.fillScreen(C_BG);
  volLevel = prefs.getUChar("lvl", 6);
  maxVolPct = prefs.getUChar("maxvol", 70);
  loadQuizProgress();
  simonBest = prefs.getUShort("simonbest", 0);
  memLevel = constrain((int)prefs.getUChar("memlvl", 1), 1, 4);
  mazeLevel = constrain((int)prefs.getUChar("mazelvl", 1), 1, 5);
  moleBest = prefs.getUShort("molebest", 0);
  brightIdx = constrain((int)prefs.getUChar("bright", 1), 0, 2);
  autoOffIdx = constrain((int)prefs.getUChar("autooff", 0), 0, 2);
  wakeMin = prefs.getUShort("wake", 7 * 60);
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

  drawTitle("Loading songs...", 160, 120, 300, 60, C_WHITE);

  bool sdOk = false;
  for (int tries = 0; tries < 10 && !(sdOk = SD.begin(SD_CS, SPI, 25000000)); tries++) delay(300);
  M5.Display.fillScreen(C_BG);
  if (!sdOk) {
    Serial.println("ERR no SD card");
    drawTitle("Oops! The music card is missing. Ask a grown-up.", 160, 120, 290, 120, rgb(255, 120, 120));
    while (true) delay(1000);
  }

  rescanLibrary();
  prepareCovers();
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
      title = trackTitle(tracks[current], playAlbum);
      paused = true;  // togglePause() starts it
    }
  }
  lastTouchMs = millis();
  if (nightBoot) {  // stay dark; keep the night-light glowing if it was
    prefs.remove("nightboot");
    if (!nightSync) prefs.remove("nightsync");
    nightLight = prefs.getUChar("nightlight", 0);
    if (prefs.getUChar("nightscreen", 0)) screen = Screen::Sleep;
    dimmed = true;
    M5.Display.setBrightness(screen == Screen::Sleep && nightLight ? 60 : 0);
  }
}

void loop() {
  M5.update();
  handleSerial();

  if (mp3->isRunning() && !paused && !mp3->loop()) playTrack(current + 1);

#if CORE2_WIFI
  wifiLoop();
#else
  if (btStateDirty) {
    btStateDirty = false;
    if (btConnected) M5.Speaker.stop(SPK_CH);
    else btHasTarget = false;
    applyVolume();
    dirtyHeader = true;
    connectDirty = true;
  }
#endif

  handleInput();
  buzzService();
  if (isQuiz(screen)) serviceQuiz(screen);
  nightlyCheck();
  if (screen == Screen::Simon) serviceSimon();
  if (screen == Screen::Memory) serviceMemory();
  if (screen == Screen::Maze) serviceMaze();
  if (screen == Screen::Moles) serviceMoles();
  serviceSleep();
  trackListening();
  static uint32_t lastStats = 0;
  if (screen == Screen::Grownups && guTab == 2 && millis() - lastStats > 2000) { lastStats = millis(); drawStats(); }
  if (overlayUntil && millis() > overlayUntil) { overlayUntil = 0; dirtyNow = true; }
  handleIdle();
  redraw();

  static uint32_t lastTick = 0, lastEq = 0, lastBat = 0;
  uint32_t now = millis();
  if (screen == Screen::Player && !overlayUntil && !dimmed && !tracks.empty()) {
    if (now - lastEq > 120) { lastEq = now; drawEqualizer(!paused); }
    if (now - lastTick > 500) { lastTick = now; drawProgress(); }
  }
  if (now - lastBat > 1000) {  // redraw the top bar when the battery or plug state changes (or the minute ticks)
    lastBat = now;
    static int shown = -1;
    m5::rtc_time_t tm;
    int state = M5.Power.getBatteryLevel() * 2 + usbPower() + (M5.Rtc.getTime(&tm) ? tm.minutes * 1000 : 0);
    if (state != shown) { shown = state; dirtyHeader = true; }
  }
}
