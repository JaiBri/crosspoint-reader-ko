// Native host test for src/util/HighlightStore.{h,cpp}.
// Build/run via test/run_highlight_store_test.sh (no PlatformIO needed).
//
// Covers: serialize->parse round-trip (anchors + layout + text), reading-order
// sort ("by page"), tolerance of user-added prose / unknown schema, the empty
// case, and sidecarPath().

#include <cstdio>
#include <string>
#include <vector>

#include "util/HighlightStore.h"

using highlight::Bookmark;
using highlight::Highlight;
using highlight::LayoutParams;
using highlight::Pos;

static int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

static LayoutParams sampleLayout() {
  LayoutParams l;
  l.fontId = 4;
  l.lineCompressionX100 = 90;
  l.viewportWidth = 400;
  l.viewportHeight = 560;
  l.paragraphAlignment = 1;
  l.characterWrap = 0;
  l.hyphenationEnabled = 1;
  l.embeddedStyle = 1;
  l.imageRendering = 2;
  l.extraParagraphSpacing = 0;
  l.paragraphIndent = 1;
  return l;
}

static Highlight makeHl(uint16_t spine, Pos start, Pos end, const std::string& text) {
  Highlight h;
  h.spine = spine;
  h.start = start;
  h.end = end;
  h.layout = sampleLayout();
  h.text = text;
  return h;
}

static bool sameHl(const Highlight& a, const Highlight& b) {
  return a.spine == b.spine && highlight::posEqual(a.start, b.start) && highlight::posEqual(a.end, b.end) &&
         a.layout == b.layout && a.text == b.text && a.note == b.note;
}

static void testRoundTrip() {
  std::vector<Highlight> items;
  items.push_back(makeHl(2, Pos{1, 0, 5}, Pos{1, 2, 22}, "the spice must flow"));
  items.push_back(makeHl(0, Pos{0, 3, 0}, Pos{0, 3, 12}, "call me Ishmael"));

  const std::string md = highlight::serialize("Dune", items);
  const std::vector<Highlight> back = highlight::parse(md);

  CHECK(back.size() == 2);
  if (back.size() == 2) {
    // parse() returns file order; serialize sorts by reading order, so
    // (spine 0 ...) comes before (spine 2 ...).
    CHECK(back[0].spine == 0);
    CHECK(back[0].text == "call me Ishmael");
    CHECK(back[1].spine == 2);
    CHECK(back[1].text == "the spice must flow");

    // Match each input to a parsed entry regardless of order.
    for (const auto& in : items) {
      bool found = false;
      for (const auto& got : back) {
        if (sameHl(in, got)) found = true;
      }
      CHECK(found);
    }
  }
}

static void testSortByPage() {
  std::vector<Highlight> items;
  items.push_back(makeHl(1, Pos{5, 0, 0}, Pos{5, 0, 4}, "later"));
  items.push_back(makeHl(1, Pos{2, 0, 0}, Pos{2, 0, 4}, "earlier"));
  items.push_back(makeHl(0, Pos{9, 0, 0}, Pos{9, 0, 4}, "first chapter"));

  const std::string md = highlight::serialize("Book", items);
  // "first chapter" (spine 0) must appear before the spine-1 entries, and
  // within spine 1, page 2 before page 5.
  const size_t pFirst = md.find("first chapter");
  const size_t pEarlier = md.find("earlier");
  const size_t pLater = md.find("later");
  CHECK(pFirst != std::string::npos);
  CHECK(pEarlier != std::string::npos);
  CHECK(pLater != std::string::npos);
  CHECK(pFirst < pEarlier);
  CHECK(pEarlier < pLater);

  // Heading reflects 1-based chapter + page.
  CHECK(md.find("## Chapter 1, Page 10") != std::string::npos);  // spine 0, page 9 -> "Chapter 1, Page 10"
  CHECK(md.find("## Chapter 2, Page 3") != std::string::npos);   // spine 1, page 2 -> "Chapter 2, Page 3"
}

static void testToleranceOfProse() {
  // A user has hand-edited the file: extra prose, a note, and an unknown
  // future-schema comment that must be ignored.
  const std::string md =
      "# Highlights — My Book\n"
      "\n"
      "Some notes I typed myself.\n"
      "\n"
      "## Chapter 3, Page 12\n"
      "> war is peace\n"
      "<!-- cpx v1 sp=2 a=11,1,0 b=11,1,12 ly=4,90,400,560,1,0,1,1,2,0,1 -->\n"
      "\n"
      "<!-- cpx v2 sp=9 a=0,0,0 b=0,0,1 ly=1 -->\n"  // unknown version -> ignored
      "\n"
      "> orphan quote with no metadata\n"
      "\n";

  const std::vector<Highlight> back = highlight::parse(md);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    CHECK(back[0].spine == 2);
    CHECK(back[0].start.page == 11);
    CHECK(back[0].end.ch == 12);
    CHECK(back[0].text == "war is peace");
    CHECK(back[0].layout == sampleLayout());
  }
}

static void testEmpty() {
  const std::vector<Highlight> none;
  const std::string md = highlight::serialize("Empty", none);
  CHECK(md.find("_No highlights yet._") != std::string::npos);
  CHECK(highlight::parse(md).empty());
  CHECK(highlight::parse("").empty());
}

static void testSidecarPath() {
  CHECK(highlight::sidecarPath("/books/Dune.epub") == "/books/Dune.epub.highlights.md");
}

static void testNoteRoundTrip() {
  std::vector<Highlight> items;
  Highlight withNote = makeHl(1, Pos{3, 0, 0}, Pos{3, 0, 4}, "a passage");
  withNote.note = "my thoughts on this";
  Highlight noNote = makeHl(1, Pos{4, 0, 0}, Pos{4, 0, 4}, "another passage");
  items.push_back(withNote);
  items.push_back(noNote);

  const std::string md = highlight::serialize("Book", items);
  // The note appears as a **Note:** line; the no-note entry must omit it.
  CHECK(md.find("**Note:** my thoughts on this") != std::string::npos);
  // Exactly one **Note:** line total.
  CHECK(md.find("**Note:**") == md.rfind("**Note:**"));

  const auto back = highlight::parse(md);
  CHECK(back.size() == 2);
  if (back.size() == 2) {
    // back[0] is page 3 (the noted one), back[1] is page 4.
    CHECK(back[0].note == "my thoughts on this");
    CHECK(back[1].note.empty());
    for (const auto& in : items) {
      bool found = false;
      for (const auto& got : back) {
        if (sameHl(in, got)) found = true;
      }
      CHECK(found);
    }
  }
}

static void testMultiLineQuoteJoin() {
  // A serialized passage is single-line, but a hand-wrapped multi-line quote
  // should be joined with spaces on parse.
  const std::string md =
      "## Chapter 1, Page 1\n"
      "> first part\n"
      "> second part\n"
      "<!-- cpx v1 sp=0 a=0,0,0 b=0,1,5 ly=4,90,400,560,1,0,1,1,2,0,1 -->\n";
  const auto back = highlight::parse(md);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    CHECK(back[0].text == "first part second part");
  }
}

static void testNegativeFontId() {
  // Regression: getReaderFontId() is a hash that can be negative. The layout
  // fingerprint's signed ints (fontId, lineCompressionX100) must round-trip;
  // otherwise the entry is silently dropped on load and the highlight vanishes.
  std::vector<Highlight> items;
  Highlight h = makeHl(9, Pos{0, 0, 0}, Pos{0, 5, 3}, "On Giving Up");
  h.layout.fontId = -1446433084;       // real-world negative hashed font id
  h.layout.lineCompressionX100 = -25;  // also signed
  items.push_back(h);

  const std::string md = highlight::serialize("On Giving Up", items);
  const auto back = highlight::parse(md);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    CHECK(back[0].layout.fontId == -1446433084);
    CHECK(back[0].layout.lineCompressionX100 == -25);
    CHECK(back[0].layout == h.layout);
    CHECK(back[0].text == "On Giving Up");
  }
}

static void testBookmarkRoundTrip() {
  std::vector<Highlight> none;
  std::vector<Bookmark> bms;
  Bookmark b;
  b.spine = 4;
  b.page = 11;
  b.layout = sampleLayout();
  b.layout.fontId = -1446433084;  // hashed font id is signed — must round-trip
  b.text = "On giving up the idea that";
  b.note = "revisit this";
  bms.push_back(b);

  const std::string md = highlight::serialize("On Giving Up", none, bms);
  CHECK(md.find("## Bookmark — Chapter 5, Page 12") != std::string::npos);
  CHECK(md.find("<!-- cpx-bm v1 sp=4 p=11 ly=-1446433084,") != std::string::npos);

  const auto back = highlight::parseBookmarks(md);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    CHECK(back[0].spine == 4);
    CHECK(back[0].page == 11);
    CHECK(back[0].layout.fontId == -1446433084);
    CHECK(back[0].layout == b.layout);
    CHECK(back[0].text == "On giving up the idea that");
    CHECK(back[0].note == "revisit this");
  }
  // A bookmark-only file has no highlights.
  CHECK(highlight::parse(md).empty());
}

static void testMixedFileNoCrossContamination() {
  // One highlight and one bookmark serialized into the SAME file must parse back
  // cleanly: parse() sees only the highlight, parseBookmarks() only the bookmark.
  std::vector<Highlight> hs;
  hs.push_back(makeHl(2, Pos{1, 0, 5}, Pos{1, 2, 22}, "the spice must flow"));
  hs.back().note = "Dune ref";
  std::vector<Bookmark> bms;
  Bookmark b;
  b.spine = 7;
  b.page = 3;
  b.layout = sampleLayout();
  b.text = "chapter opener";
  bms.push_back(b);

  const std::string md = highlight::serialize("Mixed", hs, bms);

  const auto gotH = highlight::parse(md);
  CHECK(gotH.size() == 1);
  if (gotH.size() == 1) {
    CHECK(gotH[0].spine == 2);
    CHECK(gotH[0].text == "the spice must flow");
    CHECK(gotH[0].note == "Dune ref");
  }

  const auto gotB = highlight::parseBookmarks(md);
  CHECK(gotB.size() == 1);
  if (gotB.size() == 1) {
    CHECK(gotB[0].spine == 7);
    CHECK(gotB[0].page == 3);
    CHECK(gotB[0].text == "chapter opener");
  }
}

int main() {
  testRoundTrip();
  testSortByPage();
  testToleranceOfProse();
  testEmpty();
  testSidecarPath();
  testNoteRoundTrip();
  testMultiLineQuoteJoin();
  testNegativeFontId();
  testBookmarkRoundTrip();
  testMixedFileNoCrossContamination();

  if (g_failures == 0) {
    std::printf("HighlightStore: ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("HighlightStore: %d CHECK(s) FAILED\n", g_failures);
  return 1;
}
