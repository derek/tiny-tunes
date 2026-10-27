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

static void clearCoverCache();

// Cover image preference: cover.* > folder.* > front.* > album.* > any other JPG/PNG;
// between two of the same kind (cover.png and cover.jpg), the more recently changed.
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
  time_t coverTime = 0;
  cover = "";
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) return false;
  while (File f = dir.openNextFile()) {
    const char* n = f.name();
    if (f.isDirectory()) { if (subdirs && !isHidden(n)) subdirs->push_back(n); }
    else if (isMp3(n)) hasMp3 = true;
    else if (int r = coverRank_(n); r < 99 && (r < coverRank || (r == coverRank && f.getLastWrite() > coverTime))) {
      coverRank = r;
      coverTime = f.getLastWrite();
      cover = n;
    }
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
  addShelf("Pods", "/podcasts", ShelfKind::Podcasts);  // short, to fit Home's card
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
// Albums opened or played lately, newest first (their folders, up to 40), kept on the
// card in /.recent so each shelf's carousel can open on the latest.
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

static bool recentUnsaved = false;  // moved in memory, not written yet

// Moves the album to the front, to be written later (opening an album: no card write
// while the music plays, which could make it skip).
static void noteRecentLater(const String& path) {
  if (!recentAlbums.empty() && recentAlbums[0] == path) return;
  recentAlbums.erase(std::remove(recentAlbums.begin(), recentAlbums.end(), path), recentAlbums.end());
  recentAlbums.insert(recentAlbums.begin(), path);
  if (recentAlbums.size() > RECENT_MAX) recentAlbums.resize(RECENT_MAX);
  recentUnsaved = true;
}

static void saveRecent() {
  recentUnsaved = false;
  File f = SD.open(RECENT_FILE, FILE_WRITE);
  if (!f) return;
  for (auto& p : recentAlbums) f.println(p);
  f.close();
}

// Playing an album: to the front and written now (a new song is starting anyway).
static void noteRecent(const String& path) {
  noteRecentLater(path);
  if (recentUnsaved) saveRecent();
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
// (cleanTitle, asciiFold and friends are in tt_text.h)
static bool isPodcastAlbum(int a) {
  return a >= 0 && a < (int)albums.size() && albums[a].shelf >= 0 && albums[a].shelf < (int)shelves.size() &&
         shelves[albums[a].shelf].kind == ShelfKind::Podcasts;
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

// Songs are named by their ID3v2 tags where they have them (title, artist, album, album
// artist, track and disc numbers, and a picture for albums without a cover image), and
// by their file and folder names otherwise. An album's tags are read once per rescan.

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

// The five steps are even in loudness: step 1 is always 18 dB under full (quiet, never
// silent), step 5 is the grown-ups' Loudest (100% = full, 70% = -6 dB, 50% = -12 dB, as
// before), evenly between: at 100%, 4.5 dB a step. volLevel counts half steps (0..10;
// 0 is off, the kids' buttons stop at step 1). The speaker squares its volume setting,
// so it gets the square root.
static constexpr float VOL_FLOOR_DB = -18.0f;
static float volumeGain() {  // amplitude, 0..1
  if (volLevel <= 0) return 0;
  float capDb = std::max(VOL_FLOOR_DB, 40.0f * log10f(maxVolPct / 100.0f));
  float db = VOL_FLOOR_DB + (capDb - VOL_FLOOR_DB) * (volLevel - 2) / 8.0f;
  return powf(10.0f, db / 20.0f);
}

static void applyVolume() {
  float g = volumeGain();
  volume = (uint8_t)lroundf(255.0f * sqrtf(g));
  M5.Speaker.setVolume(volume);
#if !CORE2_WIFI
  btVolume.setFactor(std::min(4095L, lroundf(4096.0f * g)));
#endif
}

static void setVolLevel(int lvl) {
  volLevel = constrain(lvl, 0, 10);
  applyVolume();
}

// ---------- mode switching ----------
// Reboots into the firmware in the other app slot, carrying volume and track over.
static void switchMode(bool quiet) {
  if (recentUnsaved) saveRecent();
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
    d.fillRect(0, 176, 320, 46, C_BG);  // under the splash title
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextColor(TFT_WHITE);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::FreeSansBold9pt7b);
    d.drawString("Getting album pictures ready", 160, 188);
    d.fillRoundRect(40, 204, 240, 10, 5, C_ROW);
    d.fillRoundRect(40, 204, std::max<int>(10, 240 * (i + 1) / todo.size()), 10, 5, C_YELLOW);
    delete loadCoverMaster(albums[todo[i]]);
  }
}

static void prepareCarouselArt();  // see "albums"
