// Text and tag helpers for Tiny Tunes: title clean-up, ASCII folding, and the ID3v2 tag
// reader. Pure functions on Arduino Strings (no SD card, no display), kept apart from
// tiny-tunes.ino so test/ can compile and test them on a computer.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctype.h>

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

static uint32_t be32(const uint8_t* b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static uint32_t synchsafe(const uint8_t* b) { return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }

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

// "Wow in the World: The Moon" in the "Wow in the World" show -> "The Moon"
static String dropShowName(String t, const String& show) {
  if (show.length() < 3 || t.length() <= show.length() + 2 || !startsNoCase(t.c_str(), show.c_str(), show.length())) return t;
  int i = show.length();
  while (i < (int)t.length() && strchr(" :-|", t[i])) i++;
  return i < (int)t.length() ? t.substring(i) : t;
}

// "yoto-fleetwood" -> "Yoto Fleetwood"; names with spaces are left as typed.
static String prettyAlbumName(String n) {
  if (n.indexOf(' ') < 0) { n.replace('-', ' '); n.replace('_', ' '); }
  for (int i = 0; i < (int)n.length(); i++)
    if ((i == 0 || n[i - 1] == ' ') && n[i] >= 'a' && n[i] <= 'z') n.setCharAt(i, n[i] - 32);
  return n;
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

// What an MP3's ID3v2 tag says (see "tags").
struct Id3Info {
  String title, artist, album, albumArtist;
  int track = 0, disc = 0;          // TRCK / TPOS ("3/12" -> 3); 0 = not given
  uint32_t picOff = 0, picLen = 0;  // the embedded picture (APIC) inside the file
  bool picPng = false;
  uint32_t audioStart = 0;          // where the audio begins, after the tag
};

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
template <typename FileT>
static bool readId3(FileT& f, Id3Info& out, bool wantPic) {
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
