// Tests for tt_text.h (titles, ASCII folding, the ID3 tag reader), on a computer.
// Run: test/run.sh

#include <cstdio>
#include <vector>

#include "arduino_string.h"
#include "../tt_text.h"

static int failures = 0, checks = 0;

static void expectEq(const String& got, const char* want, const char* what, int line) {
  checks++;
  if (got == want) return;
  failures++;
  printf("FAIL line %d: %s\n  got:  \"%s\"\n  want: \"%s\"\n", line, what, got.c_str(), want);
}
static void expectNum(long got, long want, const char* what, int line) {
  checks++;
  if (got == want) return;
  failures++;
  printf("FAIL line %d: %s\n  got:  %ld\n  want: %ld\n", line, what, got, want);
}
#define EQ(expr, want) expectEq((expr), (want), #expr, __LINE__)
#define NUM(expr, want) expectNum((long)(expr), (long)(want), #expr, __LINE__)

// An MP3 file in memory, with the two calls readId3() makes.
struct MemFile {
  std::vector<uint8_t> data;
  size_t at = 0;
  bool seek(uint32_t p) { at = p; return p <= data.size(); }
  size_t read(uint8_t* buf, size_t n) {
    size_t k = at < data.size() ? std::min(n, data.size() - at) : 0;
    memcpy(buf, data.data() + at, k);
    at += k;
    return k;
  }
};

// Builds an ID3v2 tag (version 3 or 4) from frames, followed by a little "audio".
struct TagBuilder {
  int version;
  uint8_t flags = 0;
  std::vector<uint8_t> frames;
  explicit TagBuilder(int v) : version(v) {}
  void size4(std::vector<uint8_t>& out, uint32_t n, bool synchsafe) {
    if (synchsafe) for (int s = 21; s >= 0; s -= 7) out.push_back((n >> s) & 0x7F);
    else for (int s = 24; s >= 0; s -= 8) out.push_back((n >> s) & 0xFF);
  }
  TagBuilder& frame(const char* id, const std::vector<uint8_t>& body) {
    frames.insert(frames.end(), id, id + 4);
    size4(frames, body.size(), version == 4);
    frames.push_back(0);
    frames.push_back(0);
    frames.insert(frames.end(), body.begin(), body.end());
    return *this;
  }
  TagBuilder& text(const char* id, const char* utf8) {  // UTF-8 (v2.4) or Latin-1 / ASCII (v2.3)
    std::vector<uint8_t> b{(uint8_t)(version == 4 ? 3 : 0)};
    b.insert(b.end(), utf8, utf8 + strlen(utf8));
    return frame(id, b);
  }
  MemFile build(size_t padding = 16) {
    MemFile f;
    uint8_t h[10] = {'I', 'D', '3', (uint8_t)version, 0, flags};
    std::vector<uint8_t> sz;
    size4(sz, frames.size() + padding, true);
    memcpy(h + 6, sz.data(), 4);
    f.data.insert(f.data.end(), h, h + 10);
    f.data.insert(f.data.end(), frames.begin(), frames.end());
    f.data.insert(f.data.end(), padding, 0);
    const uint8_t audio[] = {0xFF, 0xFB, 0x90, 0x64, 1, 2, 3, 4};
    f.data.insert(f.data.end(), audio, audio + sizeof audio);
    return f;
  }
};

static void testAsciiFold() {
  EQ(asciiFold("Caf\xc3\xa9"), "Cafe");
  EQ(asciiFold("\xc3\x9c" "ber"), "Uber");
  EQ(asciiFold("Don\xe2\x80\x99t Stop"), "Don't Stop");                // a curly apostrophe
  EQ(asciiFold("\xe2\x80\x9cHi\xe2\x80\x9d"), "\"Hi\"");              // curly quotes
  EQ(asciiFold("Wait\xe2\x80\xa6"), "Wait...");                        // an ellipsis
  EQ(asciiFold("Yes \xe2\x80\x94 no"), "Yes - no");                    // an em dash
  EQ(asciiFold("Fun \xf0\x9f\x8e\x89"), "Fun ");                       // an emoji goes
}

static void testCleanTitle() {
  // file names
  EQ(cleanTitle("03 Fleetwood Mac - Everywhere - 2017 Remaster", true, true), "Everywhere");
  EQ(cleanTitle("1-03 - Dreams", true, false), "Dreams");
  EQ(cleanTitle("07_Go_Your_Own_Way", true, false), "Go Your Own Way");
  EQ(cleanTitle("Landslide", true, false), "Landslide");
  // tags kids don't need
  EQ(cleanTitle("Shake It Off (Taylor's Version)", false, false), "Shake It Off");
  EQ(cleanTitle("Love Story (feat. Someone) [Remastered]", false, false), "Love Story");
  EQ(cleanTitle("Dancing Queen - Remastered 2001", false, false), "Dancing Queen");
  EQ(cleanTitle("Under the Sea (From \"The Little Mermaid\")", false, false), "Under the Sea");
  EQ(cleanTitle("Twinkle Twinkle  Little  Star", false, false), "Twinkle Twinkle Little Star");
  EQ(cleanTitle("(Love Song)", false, false), "(Love Song)");  // a title that's all brackets stays
  EQ(cleanTitle("", false, false), "Song");                    // nothing left: a placeholder
}

static void testTagTitleClean() {
  EQ(tagTitleClean("01 - The Cyclone"), "The Cyclone");
  EQ(tagTitleClean("3. Dreams"), "Dreams");
  EQ(tagTitleClean("12 - Who Stole the Tarts?"), "Who Stole the Tarts?");
  EQ(tagTitleClean("99 Luftballons"), "99 Luftballons");  // a number in the title, not a track number
  EQ(tagTitleClean("1989"), "1989");
  EQ(tagTitleClean("Everywhere (2017 Remaster)"), "Everywhere");
  EQ(tagTitleClean(""), "");
}

static void testNames() {
  EQ(dropShowName("Wow in the World: The Moon", "Wow in the World"), "The Moon");
  EQ(dropShowName("wow in the world - Bees", "Wow in the World"), "Bees");
  EQ(dropShowName("The Moon", "Wow in the World"), "The Moon");
  EQ(dropShowName("Wow in the World", "Wow in the World"), "Wow in the World");  // nothing after it
  EQ(prettyAlbumName("yoto-fleetwood"), "Yoto Fleetwood");
  EQ(prettyAlbumName("kpop_demon_hunters"), "Kpop Demon Hunters");
  EQ(prettyAlbumName("Fleetwood Mac - Rumours"), "Fleetwood Mac - Rumours");  // spaces: as typed
  EQ(lowerStr("MiXeD 12"), "mixed 12");
  NUM(startsNoCase("Hello there", "HELLO", 5), true);
  NUM(startsNoCase("Help", "HELLO", 5), false);
}

static void testId3Text() {
  const uint8_t latin1[] = {0, 'C', 'a', 'f', 0xE9};
  EQ(id3Text(latin1, sizeof latin1), "Caf\xc3\xa9");
  const uint8_t utf8[] = {3, 'H', 'i', 0xC3, 0xA9, 0, 'x'};
  EQ(id3Text(utf8, sizeof utf8), "Hi\xc3\xa9");  // stops at the nul
  const uint8_t utf16le[] = {1, 0xFF, 0xFE, 'O', 0, 'z', 0, 0xE9, 0};
  EQ(id3Text(utf16le, sizeof utf16le), "Oz\xc3\xa9");
  const uint8_t utf16be[] = {1, 0xFE, 0xFF, 0, 'O', 0, 'z'};
  EQ(id3Text(utf16be, sizeof utf16be), "Oz");
  const uint8_t utf16beNoBom[] = {2, 0, 'A', 0, 'b'};
  EQ(id3Text(utf16beNoBom, sizeof utf16beNoBom), "Ab");
  const uint8_t spaced[] = {0, ' ', 'x', ' '};
  EQ(id3Text(spaced, sizeof spaced), "x");  // trimmed
}

static void testReadId3() {
  // v2.3: title, artist, album, album artist, track "3/12", disc, and a picture
  TagBuilder t3(3);
  t3.text("TIT2", "Everywhere").text("TPE1", "Fleetwood Mac").text("TALB", "Tango in the Night")
    .text("TPE2", "Fleetwood Mac").text("TRCK", "3/12").text("TPOS", "1/1")
    .text("TXXX", "ignored");
  std::vector<uint8_t> pic{0, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 3, 'c', 'o', 'v', 'e', 'r', 0};
  size_t picAt = pic.size();
  for (int i = 0; i < 40; i++) pic.push_back(i == 0 ? 0xFF : i == 1 ? 0xD8 : 7);
  t3.frame("APIC", pic);
  MemFile f = t3.build();
  Id3Info info;
  NUM(readId3(f, info, true), true);
  EQ(info.title, "Everywhere");
  EQ(info.artist, "Fleetwood Mac");
  EQ(info.album, "Tango in the Night");
  EQ(info.albumArtist, "Fleetwood Mac");
  NUM(info.track, 3);
  NUM(info.disc, 1);
  NUM(info.audioStart, f.data.size() - 8);  // right where the audio starts
  NUM(info.picLen, 40);
  NUM(f.data[info.picOff], 0xFF);           // the picture's first bytes
  NUM(f.data[info.picOff + 1], 0xD8);
  NUM(info.picPng, false);
  (void)picAt;

  // without wantPic, the picture isn't looked at
  Id3Info noPic;
  readId3(f, noPic, false);
  NUM(noPic.picLen, 0);
  EQ(noPic.title, "Everywhere");

  // v2.4: synchsafe frame sizes, UTF-8 text, a frame bigger than 127 bytes
  TagBuilder t4(4);
  std::string longTitle(200, 'a');
  t4.text("TIT2", "Caf\xc3\xa9 Song").text("COMM", longTitle.c_str()).text("TPE1", "Somebody");
  MemFile g = t4.build();
  Id3Info i4;
  NUM(readId3(g, i4, false), true);
  EQ(i4.title, "Caf\xc3\xa9 Song");
  EQ(i4.artist, "Somebody");  // found past the long frame
  NUM(i4.track, 0);

  // no tag at all
  MemFile none;
  const uint8_t audio[] = {0xFF, 0xFB, 0x90, 0x64};
  none.data.assign(audio, audio + sizeof audio);
  Id3Info i0;
  NUM(readId3(none, i0, false), false);
  NUM(i0.audioStart, 0);

  // an unsynchronised tag is skipped (no frames read), but the audio start is right
  TagBuilder tu(3);
  tu.flags = 0x80;
  tu.text("TIT2", "Hidden");
  MemFile u = tu.build();
  Id3Info iu;
  NUM(readId3(u, iu, false), true);
  EQ(iu.title, "");
  NUM(iu.audioStart, u.data.size() - 8);

  // a frame claiming to run past the end of the tag stops the reading, safely
  TagBuilder tb(3);
  tb.text("TIT2", "Fine");
  tb.frames.insert(tb.frames.end(), {'T', 'P', 'E', '1', 0, 0, 0x7F, 0xFF, 0, 0});
  MemFile b = tb.build(0);
  Id3Info ib;
  NUM(readId3(b, ib, false), true);
  EQ(ib.title, "Fine");
  EQ(ib.artist, "");
}

int main() {
  testAsciiFold();
  testCleanTitle();
  testTagTitleClean();
  testNames();
  testId3Text();
  testReadId3();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
