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
// Persistence model is PAGE-RELATIVE (MVP): a position is (page, line, ch)
// within a chapter (spine item), valid only while the book is paginated with
// the same layout. The LayoutParams captured per highlight let the renderer
// detect a stale layout and skip drawing the overlay (the text + page label
// stay readable in the file and the on-device list regardless).
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

// Sidecar path for a book: "<bookPath>.highlights.md" — sits next to the book
// so it travels with it when the SD card is moved.
std::string sidecarPath(const std::string& bookPath);

// Reading-order ordering used to sort the file "by page": (spine, start...).
bool highlightLess(const Highlight& a, const Highlight& b);

// Render the in-memory highlights to the Markdown sidecar text. Sorts a copy by
// reading order; `bookTitle` populates the H1. Pure: returns the file contents.
std::string serialize(const std::string& bookTitle, const std::vector<Highlight>& items);

// Parse a Markdown sidecar back into highlights. Tolerant of user-added prose:
// only `<!-- cpx ... -->` metadata lines (and the `>` quote block immediately
// above each) are interpreted. Unknown/old schema lines are skipped.
std::vector<Highlight> parse(const std::string& markdown);

}  // namespace highlight
