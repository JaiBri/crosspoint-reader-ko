// Native host test for src/util/HighlightStore.{h,cpp}.
// Build/run via test/run_highlight_store_test.sh (no PlatformIO needed).
//
// Covers: serialize->parse round-trip (anchors + layout + text), reading-order
// sort ("by page"), tolerance of user-added prose / unknown schema, the empty
// case, and sidecarPath().

#include <cstdio>
#include <string>
#include <utility>
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
  l.focusReading = 1;
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

static Bookmark makeBm(uint16_t spine, uint16_t page, int32_t pct, const std::string& text) {
  Bookmark b;
  b.spine = spine;
  b.page = page;
  b.pctX10000 = pct;
  b.layout = sampleLayout();
  b.text = text;
  return b;
}

static bool sameBm(const Bookmark& a, const Bookmark& b) {
  return a.spine == b.spine && a.page == b.page && a.pctX10000 == b.pctX10000 && a.layout == b.layout &&
         a.text == b.text && a.note == b.note;
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
    LayoutParams legacyLayout = sampleLayout();
    legacyLayout.focusReading = 0;  // 11-field ly= predates the field
    CHECK(back[0].layout == legacyLayout);
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
  b.pctX10000 = 4567;
  bms.push_back(b);

  const std::string md = highlight::serialize("On Giving Up", none, bms);
  CHECK(md.find("## Bookmark — Chapter 5, Page 12 (46%)") != std::string::npos);
  // The pre-`pc=` prefix must be byte-identical; `pc=` is appended after `ly=`.
  CHECK(md.find("<!-- cpx-bm v1 sp=4 p=11 ly=-1446433084,") != std::string::npos);
  CHECK(md.find(" pc=4567 -->") != std::string::npos);

  const auto back = highlight::parseBookmarks(md);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    CHECK(back[0].spine == 4);
    CHECK(back[0].page == 11);
    CHECK(back[0].pctX10000 == 4567);
    CHECK(back[0].layout.fontId == -1446433084);
    CHECK(back[0].layout == b.layout);
    CHECK(back[0].text == "On giving up the idea that");
    CHECK(back[0].note == "revisit this");
    CHECK(sameBm(back[0], b));
  }
  // A bookmark-only file has no highlights.
  CHECK(highlight::parse(md).empty());
}

// A bookmark with no anchor must not emit `pc=` at all, and must read back as
// PCT_ABSENT rather than silently becoming 0%.
static void testBookmarkPercentAbsentOmitted() {
  std::vector<Highlight> none;
  std::vector<Bookmark> bms;
  bms.push_back(makeBm(1, 2, highlight::PCT_ABSENT, "no anchor yet"));

  const std::string md = highlight::serialize("Absent", none, bms);
  CHECK(md.find(" pc=") == std::string::npos);
  CHECK(md.find("(0%)") == std::string::npos);
  CHECK(md.find("## Bookmark — Chapter 2, Page 3\n") != std::string::npos);

  const auto back = highlight::parseBookmarks(md);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    CHECK(back[0].pctX10000 == highlight::PCT_ABSENT);
    CHECK(!highlight::pctValid(back[0].pctX10000));
  }
}

// A sidecar written before `pc=` existed must still load, on the page fallback.
static void testBookmarkLegacyLineParses() {
  const std::string md =
      "# Highlights — Legacy\n\n"
      "## Bookmark — Chapter 3, Page 8\n"
      "> legacy bookmark\n"
      "<!-- cpx-bm v1 sp=2 p=7 ly=4,90,400,560,1,0,1,1,2,0,1 -->\n\n";

  const auto back = highlight::parseBookmarks(md);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    CHECK(back[0].spine == 2);
    CHECK(back[0].page == 7);
    CHECK(back[0].pctX10000 == highlight::PCT_ABSENT);
    CHECK(back[0].text == "legacy bookmark");
    LayoutParams legacyLayout = sampleLayout();
    legacyLayout.focusReading = 0;  // 11-field ly= predates the field
    CHECK(back[0].layout == legacyLayout);
  }
}

// Unknown keys are ignored rather than rejecting the line. This is the property
// that lets `pc=` be added without a version bump, so it is worth locking in.
static void testBookmarkUnknownKeyIgnored() {
  const std::string md =
      "## Bookmark — Chapter 1, Page 1\n"
      "> forward compatible\n"
      "<!-- cpx-bm v1 sp=0 p=0 zz=9 ly=4,90,400,560,1,0,1,1,2,0,1 xp=/body/x -->\n\n";

  const auto back = highlight::parseBookmarks(md);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    CHECK(back[0].spine == 0);
    CHECK(back[0].page == 0);
    CHECK(back[0].text == "forward compatible");
  }
}

// A malformed or out-of-range `pc=` must degrade the bookmark to the page
// fallback, NOT drop it — unlike sp/p/ly, which are required.
static void testBookmarkBadPercentIsIgnoredNotFatal() {
  // "pc=45.67" covers the no-float-parser path: parseInt requires the whole
  // value to be consumed, so a decimal is rejected rather than truncated to 45.
  const char* const bad[] = {"pc=99999", "pc=-7", "pc=abc", "pc=", "pc=10001", "pc=45.67", "pc=0x10"};
  for (const char* tok : bad) {
    const std::string md = std::string(
                               "## Bookmark — Chapter 1, Page 2\n"
                               "> resilient\n"
                               "<!-- cpx-bm v1 sp=0 p=1 ly=4,90,400,560,1,0,1,1,2,0,1 ") +
                           tok + " -->\n\n";
    const auto back = highlight::parseBookmarks(md);
    CHECK(back.size() == 1);
    if (back.size() == 1) {
      CHECK(back[0].spine == 0);
      CHECK(back[0].page == 1);
      CHECK(back[0].pctX10000 == highlight::PCT_ABSENT);
    }
  }
  // Boundary values that ARE valid must still be accepted.
  for (const auto& [tok, want] : std::vector<std::pair<std::string, int32_t>>{{"pc=0", 0}, {"pc=10000", 10000}}) {
    const std::string md = "<!-- cpx-bm v1 sp=0 p=1 ly=4,90,400,560,1,0,1,1,2,0,1 " + tok + " -->\n";
    const auto back = highlight::parseBookmarks(md);
    CHECK(back.size() == 1);
    if (back.size() == 1) {
      CHECK(back[0].pctX10000 == want);
    }
  }
}

// Encoding the page CENTRE makes page->fraction->page an exact identity, which
// is what stops a jump drifting every time a bookmark is re-resolved.
static void testPageFractionRoundTrip() {
  for (const int n : {1, 2, 3, 7, 50, 997}) {
    for (int p = 0; p < n; p++) {
      CHECK(highlight::pageForFraction(highlight::pageCentreFraction(p, n), n) == p);
    }
  }
  // Clamping and degenerate inputs.
  CHECK(highlight::pageCentreFraction(-5, 10) == highlight::pageCentreFraction(0, 10));
  CHECK(highlight::pageCentreFraction(99, 10) == highlight::pageCentreFraction(9, 10));
  CHECK(highlight::pageCentreFraction(0, 0) == 0.0f);
  CHECK(highlight::pageForFraction(-1.0f, 10) == 0);
  CHECK(highlight::pageForFraction(2.0f, 10) == 9);
  CHECK(highlight::pageForFraction(0.5f, 0) == 0);
}

static void testEncodePctClamp() {
  CHECK(highlight::encodePct(-0.1f) == 0);
  CHECK(highlight::encodePct(0.0f) == 0);
  CHECK(highlight::encodePct(1.0f) == highlight::PCT_SCALE);
  CHECK(highlight::encodePct(1.5f) == highlight::PCT_SCALE);
  CHECK(highlight::encodePct(0.45674f) == 4567);
  CHECK(highlight::pctToDisplayPercent(4567) == 46);
  CHECK(highlight::pctToDisplayPercent(4500) == 45);
  CHECK(highlight::pctToDisplayPercent(0) == 0);
  CHECK(highlight::pctToDisplayPercent(highlight::PCT_SCALE) == 100);
  CHECK(highlight::pctToDisplayPercent(highlight::PCT_ABSENT) == 0);
}

// Once bookmarks carry an anchor the list must order by it — sorting by the
// stored page would keep using indices that a layout change already invalidated.
static void testBookmarkSortUsesPercent() {
  std::vector<Highlight> none;
  std::vector<Bookmark> bms;
  bms.push_back(makeBm(3, 9, 5000, "later in the book"));
  bms.push_back(makeBm(3, 2, 1000, "earlier in the book"));

  const std::string md = highlight::serialize("Sorted", none, bms);
  const auto back = highlight::parseBookmarks(md);
  CHECK(back.size() == 2);
  if (back.size() == 2) {
    CHECK(back[0].pctX10000 == 1000);
    CHECK(back[1].pctX10000 == 5000);
  }

  // With no anchors, ordering falls back to the page index.
  std::vector<Bookmark> legacy;
  legacy.push_back(makeBm(3, 9, highlight::PCT_ABSENT, "page nine"));
  legacy.push_back(makeBm(3, 2, highlight::PCT_ABSENT, "page two"));
  const auto backLegacy = highlight::parseBookmarks(highlight::serialize("Legacy", none, legacy));
  CHECK(backLegacy.size() == 2);
  if (backLegacy.size() == 2) {
    CHECK(backLegacy[0].page == 2);
    CHECK(backLegacy[1].page == 9);
  }
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


// An 11-field ly= (written before focusReading joined the fingerprint) must
// still parse, with the new field defaulting to 0. Guards the compatibility
// that lets the fingerprint grow without a version bump.
static void testLegacyElevenFieldLayoutParses() {
  const std::string md =
      "## Bookmark — Chapter 1, Page 1\n"
      "> eleven field layout\n"
      "<!-- cpx-bm v1 sp=0 p=0 ly=4,90,400,560,1,0,1,1,2,0,1 -->\n\n";
  const auto back = highlight::parseBookmarks(md);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    CHECK(back[0].layout.focusReading == 0);
    CHECK(back[0].layout.paragraphIndent == 1);
  }
  // A 12-field ly= round-trips the new field.
  std::vector<Highlight> none;
  std::vector<Bookmark> bms;
  bms.push_back(makeBm(0, 0, 5000, "twelve"));
  const auto back12 = highlight::parseBookmarks(highlight::serialize("T", none, bms));
  CHECK(back12.size() == 1);
  if (back12.size() == 1) {
    CHECK(back12[0].layout.focusReading == 1);
    CHECK(back12[0].layout == sampleLayout());
  }
  // A wrong-arity ly= is still rejected outright.
  CHECK(highlight::parseBookmarks("<!-- cpx-bm v1 sp=0 p=0 ly=4,90,400 -->\n").empty());
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
  testBookmarkPercentAbsentOmitted();
  testBookmarkLegacyLineParses();
  testBookmarkUnknownKeyIgnored();
  testBookmarkBadPercentIsIgnoredNotFatal();
  testPageFractionRoundTrip();
  testEncodePctClamp();
  testBookmarkSortUsesPercent();
  testLegacyElevenFieldLayoutParses();

  if (g_failures == 0) {
    std::printf("HighlightStore: ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("HighlightStore: %d CHECK(s) FAILED\n", g_failures);
  return 1;
}
