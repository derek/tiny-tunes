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
// Files can also be uploaded over USB with upload.py (see the serial protocol in 70_serial.ino).
//
// The sketch is split into tabs. arduino-cli joins them into one file, this one first and
// then the rest in name order, so the numbers keep definitions ahead of their uses:
//   tiny-tunes.ino       settings, colors, types, player state (types used in function
//                        signatures must be declared here)
//   10_network.ino       Bluetooth discovery / WiFi: clock, web server (page: web/index.html), podcasts
//   20_library.ino       the card's library, recently played, resume, playback, covers
//   30_ui.ino            drawing: icons, top bar, Home, albums, songs, player, snackbars
//   40_nightly_sync.ino  the nightly switch to WiFi for podcast downloads
//   50_grownups.ino      the grown-ups screen
//   60_input.ino         screen switching, touch and buttons, idle and auto-off
//   70_serial.ino        the USB serial protocol (upload.py, screenshot.py)
//   80_setup.ino         splash, setup() and loop()

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
#include "tt_text.h"  // titles, ASCII folding, the ID3 tag reader (tested in test/)
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

// ---------- colors ----------
// Themes, picked on the grown-ups screen (Settings -> Colors) and kept in prefs;
// 0xRRGGBB. Candy and Daylight are light: dark words and marks (ink) on the background
// (Candy's pastel shelves get dark words too; Daylight's bright ones white).
struct Theme {
  const char* name;
  uint32_t ink;  // words and marks on the background, cards and bars
  uint32_t bg, bar, card, row, off, dim, music, accent, accentInk, pods, books, level, badge, panel,
      offDash, dimInk, litCtrl, litVol, litPlay, snackBg, snackText, modeBtn, other;
  uint32_t shelfInk;  // words and marks on a shelf's color (Home's cards, the top bar past Home)
};
static const Theme THEMES[] = {
    {"Forest", 0xFFFFFF, 0x14261E, 0x0D1B15, 0x20392D, 0x244032, 0x2C4A3B, 0x8FB5A0, 0x34A39B, 0xF2C14E, 0x14261E, 0xC9606E, 0xE0A33A,
     0xF2C14E, 0xF4EFE3, 0x1E362A, 0x3C604E, 0x96B4A5, 0x6ECDC3, 0x466E5A, 0xFADA8C, 0x123A38, 0xCDDED4, 0x345646, 0x788C46, 0xFFFFFF},
    {"Navy", 0xFFFFFF, 0x121630, 0x0E1128, 0x1E244E, 0x282E50, 0x32385C, 0x7882AA, 0x1996DC, 0x2EC478, 0xFFFFFF, 0x8A50EC, 0xFF924C,
     0xFFD200, 0xFFD200, 0x1C2142, 0x4A5180, 0x969CBE, 0x6EC3F5, 0x646CA0, 0x78E6AA, 0x102846, 0xC8CDE6, 0x3C426E, 0x00AAAA, 0xFFFFFF},
    {"Ocean", 0xFFFFFF, 0x0B2A33, 0x071E25, 0x123A45, 0x15414D, 0x1A4A56, 0x7FB2BC, 0x1FB5A8, 0xFF8C69, 0xFFFFFF, 0x6C7CFF, 0xF2B84B,
     0xF2B84B, 0xFF8C69, 0x103641, 0x2A5E6B, 0x96BCC4, 0x70D8CE, 0x2E6270, 0xFFB49C, 0x0E3E48, 0xCFE2E6, 0x245866, 0xC9A0DC, 0xFFFFFF},
    {"Sunset", 0xFFFFFF, 0x2A1533, 0x1E0E25, 0x3D2147, 0x42264E, 0x4A2A55, 0xC59AC9, 0xFF9F43, 0xFF7A59, 0xFFFFFF, 0xFF4F8B, 0xE0B020,
     0xFFD43B, 0xFFFFFF, 0x361C40, 0x5E3A6A, 0xC8A8CC, 0xFFC48A, 0x6A4476, 0xFFAA90, 0x4A2440, 0xE8D4EA, 0x563262, 0x5EC8C8, 0xFFFFFF},
    {"Midnight", 0xFFFFFF, 0x000000, 0x000000, 0x161616, 0x1E1E1E, 0x262626, 0x8C8C8C, 0x00B7FF, 0x39E58C, 0x000000, 0xB44BFF, 0xFF8A00,
     0xF5FF3B, 0xF5FF3B, 0x141414, 0x3A3A3A, 0xA0A0A0, 0x70D8FF, 0x444444, 0x8CF5BE, 0x0A2A3A, 0xD0D0D0, 0x303030, 0x00C8B4, 0xFFFFFF},
    {"Candy", 0x3B2A5C, 0xF3EAFF, 0xE4D4FF, 0xFFFFFF, 0xFFFFFF, 0xE4D4FF, 0x8A78AE, 0x8FD8F8, 0xFF6FA8, 0xFFFFFF, 0xC3ADFF, 0xFFC98A,
     0xFF6FA8, 0xFF6FA8, 0xFFFFFF, 0xD6C6F0, 0x9A88BD, 0xBDE9FB, 0xD6C6F0, 0xFF9CC4, 0x6A55A0, 0xE8E0F8, 0xD9C8F5, 0xA8E6CF, 0x3B2A5C},
    {"Daylight", 0x1E2240, 0xF4F1EA, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xE6E2D9, 0x8A8FA8, 0x2FA3E6, 0x22B573, 0xFFFFFF, 0x9B6BF2, 0xFF9A4D,
     0x22B573, 0xFFD200, 0xFFFFFF, 0xD8D3C8, 0x8A8FA8, 0x8FD0F2, 0xD8D3C8, 0x7FD9AE, 0x2A4A78, 0xE0E6F0, 0xE6E2D9, 0x3CBFAE, 0xFFFFFF},
};
static constexpr int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);
static int themeIdx = 0;
// (a macro, not a function: the first function in the sketch is where Arduino puts its
// prototypes, which need the structs further down)
#define c565(c) ((uint16_t)((((c) >> 8) & 0xF800) | (((c) >> 5) & 0x07E0) | (((c) >> 3) & 0x001F)))
#define TH(field) c565(THEMES[themeIdx].field)
#define C_BG     TH(bg)
#define C_BAR    TH(bar)     // Home's top bar (a shelf's screens use its color)
#define C_KL     TH(card)    // Home's Keep listening card
#define C_ROW    TH(row)
#define C_OFF    TH(off)
#define C_DIM    TH(dim)
#define C_BLUE   TH(music)   // Music, previous / next
#define C_ACCENT TH(accent)  // play, the now-playing button
#define C_ACCENT_INK TH(accentInk)  // the marks on it
#define C_BERRY  TH(pods)    // Pods
#define C_ORANGE TH(books)   // Books
#define C_YELLOW TH(level)   // the volume steps, progress
#define C_CREAM  TH(badge)   // new podcast episodes
#define C_PANEL  TH(panel)
#define C_WHITE  TH(ink)          // words and marks (white, or Candy's dark plum)
#define C_SHELF_INK TH(shelfInk)  // on a shelf's color
#define C_PURE   c565(0xFFFFFF)  // white whatever the theme: on snackbars, the playing row, plain covers
#define C_SHADE  c565(0x000000)
#define C_GREEN  c565(0x2EC478)  // charging, all good
struct RoundButton { int x, r; };
struct TrackTag { const char* file; const char* title; uint16_t track, disc; };  // one song's tag (text in PSRAM, see arenaDup)
struct CoverEntry { String path; int size; bool dim; M5Canvas* canvas; uint32_t used; };  // see "album covers"
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
struct Feed { String url, name; int keep; };  // a podcast feed (WiFi mode): see "podcasts"

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
// What it was doing, kept through a crash or watchdog reset (not a power-off), so the
// next boot can say whether Bluetooth audio was running: see noteLastRestart().
RTC_NOINIT_ATTR static uint32_t rtcCrumb;
static constexpr uint32_t CRUMB_MAGIC = 0x7A7E0000;

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
  // Volume is ours (applyVolume() -> btVolume), not the library's curve, and isn't sent
  // to the headphones/speaker: their own volume stays wherever a grown-up left it.
  void set_volume(uint8_t) override {}
  bool isSaved(const uint8_t* addr) {  // the last headphones/speaker we connected to
    static const uint8_t none[ESP_BD_ADDR_LEN] = {0};
    return memcmp(last_connection, none, ESP_BD_ADDR_LEN) && !memcmp(addr, last_connection, ESP_BD_ADDR_LEN);
  }
};
static TinyA2DP a2dp;

// Scales the Bluetooth audio by a factor out of 4096 (= as decoded), set by applyVolume().
class TinyVolume : public A2DPVolumeControl {
 public:
  void setFactor(int32_t f) { volumeFactor = f; is_volume_used = true; }
  void set_volume(uint8_t) override {}
};
static TinyVolume btVolume;

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
#if !CORE2_WIFI
    if (btConnected) { _idx = 0; return; }  // leftovers from before Bluetooth: playing them would switch the amp back on
#endif
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
static bool musicIsPlaying();
static void stopPlayback();
static String trackTitle(const String& path, int album);
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b);

static std::vector<String> listTracks(int a);
static void loadTags(int a, const std::vector<String>& files);
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
