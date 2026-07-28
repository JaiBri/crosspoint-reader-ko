#pragma once

#include <cstdint>
#include <string>
#include <vector>

// HighlightStore — pure model + Markdown (de)serialization for per-book text
// highlights. Deliberately free of any HAL/Storage/Arduino dependency so it can
// be unit-tested natively on the host (see test/highlight_store/). The actual
// SD-card read/write happens at the call site via Storage.readFile/writeFile;
// this module only turns Highlights <-> Markdown text and owns the data model.
//
// HIGHLIGHT persistence is PAGE-RELATIVE: a position is (page, line, ch) within
// a chapter (spine item), valid only while the book is paginated with the same
// layout. The LayoutParams captured per highlight let the renderer detect a
// stale layout and skip drawing the overlay (the text + page label stay
// readable in the file and the on-device list regardless).
//
// BOOKMARK persistence is LAYOUT-INDEPENDENT: a bookmark stores a book-progress
// fraction (`pc=`, see Bookmark::pctX10000) that is re-resolved to a page after
// the target chapter has been paginated under the current settings, so it stays
// correct across font/margin/orientation changes. (spine, page) is still
// written for readability and as the fallback for pre-`pc=` bookmarks.
//
// A highlight is confined to a single chapter (spine) in this version; start
// and end therefore share `spine`. Selecting across a chapter boundary is not
// supported yet (make a second highlight) — page boundaries within a chapter
// are fully supported.

namespace highlight {

// Layout fingerprint — exactly the inputs that determine pagination in
// EpubReaderActivity::render() (the args passed to Section::loadSectionFile).
// If any differ at render time, stored (page,line,ch) anchors are meaningless,
// so the overlay is skipped.
struct LayoutParams {
  int fontId = 0;
  int lineCompressionX100 = 0;  // lineCompression * 100, rounded (kept integral for exact compare)
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  uint8_t paragraphAlignment = 0;
  uint8_t characterWrap = 0;
  uint8_t hyphenationEnabled = 0;
  uint8_t embeddedStyle = 0;
  uint8_t imageRendering = 0;
  uint8_t extraParagraphSpacing = 0;
  uint8_t paragraphIndent = 0;
  // Focus Reading changes line breaking, so it changes pagination and therefore
  // belongs in the fingerprint. Added after the 11-field format shipped; files
  // written before it parse with 0, which matches the default-off setting.
  uint8_t focusReading = 0;

  bool operator==(const LayoutParams&) const = default;
};

// A character boundary position within a chapter, page-relative.
//   page = page index within the chapter's section
//   line = index of the text line on that page (image elements skipped)
//   ch   = codepoint offset within that line's logical string
//          (words joined by single spaces); 0 == before the first character
struct Pos {
  uint16_t page = 0;
  uint16_t line = 0;
  uint16_t ch = 0;
};

// Reading-order comparison of two positions within the same chapter.
inline bool posLess(const Pos& a, const Pos& b) {
  if (a.page != b.page) return a.page < b.page;
  if (a.line != b.line) return a.line < b.line;
  return a.ch < b.ch;
}
inline bool posEqual(const Pos& a, const Pos& b) {
  return a.page == b.page && a.line == b.line && a.ch == b.ch;
}

struct Highlight {
  uint16_t spine = 0;  // chapter (spine item) index; same for start and end
  Pos start;
  Pos end;
  LayoutParams layout;
  std::string text;  // extracted passage (human-readable payload)
  std::string note;  // optional user comment, shown under the quote in the sidecar
};

// Sentinel + scale for the layout-independent bookmark anchor below.
inline constexpr int32_t PCT_ABSENT = -1;
inline constexpr int32_t PCT_SCALE = 10000;
inline constexpr bool pctValid(const int32_t pct) { return pct >= 0 && pct <= PCT_SCALE; }

// A bookmark marks a whole page (not a character range), stored in the SAME
// sidecar file as highlights under a distinct `<!-- cpx-bm v1 ... -->` marker.
struct Bookmark {
  uint16_t spine = 0;  // chapter (spine item) index
  uint16_t page = 0;   // page index within that chapter's section
  // Book progress x10000 (so `pc=4567` reads as 45.67%), or PCT_ABSENT when not
  // yet derived. This is the LAYOUT-INDEPENDENT anchor: it comes from
  // Epub::calculateProgress(), which is computed over uncompressed ZIP byte
  // sizes fixed at EPUB-parse time, so it is immune to font/margin/orientation
  // changes. `page` above is only a fallback for bookmarks written before this
  // field existed. Scaled integer rather than float so the existing parseInt is
  // reused (this file has no float parser), locale decimal separators cannot
  // creep in, and the value stays legible in a file the user may edit.
  int32_t pctX10000 = PCT_ABSENT;
  LayoutParams layout;
  std::string text;  // short label: a snippet of the bookmarked page (human-readable)
  std::string note;  // optional user comment
};

// Page <-> fraction conversion for a chapter of `pageCount` pages.
// Encodes the page CENTRE, which makes the pair self-inverse
// (pageForFraction(pageCentreFraction(p, n), n) == p) and keeps the anchor half
// a page clear of chapter boundaries. Both clamp; pageCount <= 0 yields 0.
float pageCentreFraction(int page, int pageCount);
int pageForFraction(float fraction, int pageCount);

// Book fraction (0.0-1.0) <-> stored anchor. encodePct clamps to [0, PCT_SCALE].
int32_t encodePct(float bookFraction);
// Rounded whole percent for display; returns 0 when the anchor is absent.
int pctToDisplayPercent(int32_t pct);

// Sidecar path for a book: "<bookPath>.highlights.md" — sits next to the book
// so it travels with it when the SD card is moved.
std::string sidecarPath(const std::string& bookPath);

// Reading-order ordering used to sort the file "by page": (spine, start...).
bool highlightLess(const Highlight& a, const Highlight& b);
// Bookmark ordering: (spine, then pctX10000 when both are valid, else page).
// Preferring the anchor over `page` matters once bookmarks carry a percentage —
// otherwise a migrated list would still sort by stale page indices.
bool bookmarkLess(const Bookmark& a, const Bookmark& b);

// Render the in-memory highlights (and optional bookmarks) to the Markdown
// sidecar text. Sorts copies by reading order; `bookTitle` populates the H1.
// Pure: returns the file contents. Both item types share one file, so callers
// must pass BOTH current vectors or the omitted type would be erased on write.
std::string serialize(const std::string& bookTitle, const std::vector<Highlight>& items,
                      const std::vector<Bookmark>& bookmarks = {});

// Parse a Markdown sidecar back into highlights. Tolerant of user-added prose:
// only `<!-- cpx v1 ... -->` metadata lines (and the `>` quote block immediately
// above each) are interpreted. Bookmark (`cpx-bm`) and unknown lines are skipped.
std::vector<Highlight> parse(const std::string& markdown);

// Parse the bookmarks (`<!-- cpx-bm v1 ... -->`) from the same sidecar text.
std::vector<Bookmark> parseBookmarks(const std::string& markdown);

}  // namespace highlight
