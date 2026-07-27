#include "HighlightStore.h"

#include <algorithm>
#include <cstdlib>

namespace highlight {

namespace {

bool startsWith(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

// Split `s` on `sep`. Empty fields are preserved.
std::vector<std::string> splitChar(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

// Parse a non-negative integer. Returns false on any non-numeric content.
bool parseUInt(const std::string& s, long& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end != s.c_str() + s.size() || v < 0) return false;
  out = v;
  return true;
}

// Parse a (possibly negative) integer; the whole string must be numeric.
// Used for the layout fingerprint, whose fontId is a hash that may be negative.
bool parseInt(const std::string& s, long& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end != s.c_str() + s.size()) return false;
  out = v;
  return true;
}

// Replace CR/LF with spaces so a passage never breaks the single-line `>` quote.
std::string oneLine(const std::string& in) {
  std::string out = in;
  for (char& c : out) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  return out;
}

// Serialize the 11-field layout fingerprint as a comma list (shared by both
// highlight `cpx` and bookmark `cpx-bm` metadata).
std::string serializeLayout(const LayoutParams& l) {
  return std::to_string(l.fontId) + "," + std::to_string(l.lineCompressionX100) + "," +
         std::to_string(l.viewportWidth) + "," + std::to_string(l.viewportHeight) + "," +
         std::to_string(l.paragraphAlignment) + "," + std::to_string(l.characterWrap) + "," +
         std::to_string(l.hyphenationEnabled) + "," + std::to_string(l.embeddedStyle) + "," +
         std::to_string(l.imageRendering) + "," + std::to_string(l.extraParagraphSpacing) + "," +
         std::to_string(l.paragraphIndent);
}

// Parse the 11-field `ly=` value into a LayoutParams. fontId/lineCompressionX100
// are signed (hashed/derived); the rest are non-negative but parsed signed-tolerant.
bool parseLayout(const std::string& val, LayoutParams& l) {
  const auto parts = splitChar(val, ',');
  if (parts.size() != 11) return false;
  long n[11];
  for (int i = 0; i < 11; i++) {
    if (!parseInt(parts[i], n[i])) return false;
  }
  l.fontId = static_cast<int>(n[0]);
  l.lineCompressionX100 = static_cast<int>(n[1]);
  l.viewportWidth = static_cast<uint16_t>(n[2]);
  l.viewportHeight = static_cast<uint16_t>(n[3]);
  l.paragraphAlignment = static_cast<uint8_t>(n[4]);
  l.characterWrap = static_cast<uint8_t>(n[5]);
  l.hyphenationEnabled = static_cast<uint8_t>(n[6]);
  l.embeddedStyle = static_cast<uint8_t>(n[7]);
  l.imageRendering = static_cast<uint8_t>(n[8]);
  l.extraParagraphSpacing = static_cast<uint8_t>(n[9]);
  l.paragraphIndent = static_cast<uint8_t>(n[10]);
  return true;
}

std::string metaComment(const Highlight& h) {
  std::string s = "<!-- cpx v1 sp=";
  s += std::to_string(h.spine);
  s += " a=" + std::to_string(h.start.page) + "," + std::to_string(h.start.line) + "," + std::to_string(h.start.ch);
  s += " b=" + std::to_string(h.end.page) + "," + std::to_string(h.end.line) + "," + std::to_string(h.end.ch);
  s += " ly=" + serializeLayout(h.layout);
  s += " -->";
  return s;
}

std::string metaCommentBookmark(const Bookmark& b) {
  std::string s = "<!-- cpx-bm v1 sp=";
  s += std::to_string(b.spine);
  s += " p=" + std::to_string(b.page);
  s += " ly=" + serializeLayout(b.layout);
  // Appended AFTER ly= so the leading portion of the line is unchanged from the
  // pre-`pc=` format. No version bump is needed: parseBookmarkMeta skips tokens
  // whose key matches no branch and requires only sp/p/ly, so older firmware
  // reads these lines fine (it will, however, drop `pc=` when it next writes).
  if (pctValid(b.pctX10000)) {
    s += " pc=" + std::to_string(b.pctX10000);
  }
  s += " -->";
  return s;
}

// Parse one "<!-- cpx v1 ... -->" line into a Highlight (text left empty).
// Returns false if the schema/version doesn't match or fields are malformed.
// Note: a "<!-- cpx-bm v1 ..." bookmark line fails the prefix check and so is
// rejected here (kept distinct from highlights).
bool parseMeta(const std::string& line, Highlight& out) {
  if (!startsWith(line, "<!-- cpx v1")) return false;

  bool haveSp = false, haveA = false, haveB = false, haveLy = false;
  for (const auto& tok : splitChar(line, ' ')) {
    const size_t eq = tok.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = tok.substr(0, eq);
    const std::string val = tok.substr(eq + 1);

    if (key == "sp") {
      long v;
      if (!parseUInt(val, v)) return false;
      out.spine = static_cast<uint16_t>(v);
      haveSp = true;
    } else if (key == "a" || key == "b") {
      const auto parts = splitChar(val, ',');
      if (parts.size() != 3) return false;
      long p, ln, c;
      if (!parseUInt(parts[0], p) || !parseUInt(parts[1], ln) || !parseUInt(parts[2], c)) return false;
      Pos pos{static_cast<uint16_t>(p), static_cast<uint16_t>(ln), static_cast<uint16_t>(c)};
      if (key == "a") {
        out.start = pos;
        haveA = true;
      } else {
        out.end = pos;
        haveB = true;
      }
    } else if (key == "ly") {
      if (!parseLayout(val, out.layout)) return false;
      haveLy = true;
    }
  }
  return haveSp && haveA && haveB && haveLy;
}

// Parse one "<!-- cpx-bm v1 ... -->" line into a Bookmark (text/note left empty).
bool parseBookmarkMeta(const std::string& line, Bookmark& out) {
  if (!startsWith(line, "<!-- cpx-bm v1")) return false;

  bool haveSp = false, haveP = false, haveLy = false;
  for (const auto& tok : splitChar(line, ' ')) {
    const size_t eq = tok.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = tok.substr(0, eq);
    const std::string val = tok.substr(eq + 1);

    if (key == "sp") {
      long v;
      if (!parseUInt(val, v)) return false;
      out.spine = static_cast<uint16_t>(v);
      haveSp = true;
    } else if (key == "p") {
      long v;
      if (!parseUInt(val, v)) return false;
      out.page = static_cast<uint16_t>(v);
      haveP = true;
    } else if (key == "ly") {
      if (!parseLayout(val, out.layout)) return false;
      haveLy = true;
    } else if (key == "pc") {
      // Optional. Unlike sp/p/ly this must NEVER reject the line — a malformed
      // or out-of-range anchor degrades the bookmark to the (spine, page)
      // fallback, whereas returning false would silently drop it entirely.
      long v;
      if (parseInt(val, v) && v >= 0 && v <= PCT_SCALE) {
        out.pctX10000 = static_cast<int32_t>(v);
      }
    }
  }
  return haveSp && haveP && haveLy;
}

}  // namespace

std::string sidecarPath(const std::string& bookPath) { return bookPath + ".highlights.md"; }

bool highlightLess(const Highlight& a, const Highlight& b) {
  if (a.spine != b.spine) return a.spine < b.spine;
  if (!posEqual(a.start, b.start)) return posLess(a.start, b.start);
  return posLess(a.end, b.end);
}

bool bookmarkLess(const Bookmark& a, const Bookmark& b) {
  if (a.spine != b.spine) return a.spine < b.spine;
  // Prefer the layout-independent anchor; `page` may be stale on either side.
  if (pctValid(a.pctX10000) && pctValid(b.pctX10000) && a.pctX10000 != b.pctX10000) {
    return a.pctX10000 < b.pctX10000;
  }
  return a.page < b.page;
}

float pageCentreFraction(const int page, const int pageCount) {
  if (pageCount <= 0) return 0.0f;
  int p = page;
  if (p < 0) p = 0;
  if (p >= pageCount) p = pageCount - 1;
  return (static_cast<float>(p) + 0.5f) / static_cast<float>(pageCount);
}

int pageForFraction(const float fraction, const int pageCount) {
  if (pageCount <= 0) return 0;
  float f = fraction;
  if (!(f >= 0.0f)) f = 0.0f;  // also catches NaN
  if (f > 1.0f) f = 1.0f;
  int page = static_cast<int>(f * static_cast<float>(pageCount));
  if (page >= pageCount) page = pageCount - 1;
  if (page < 0) page = 0;
  return page;
}

int32_t encodePct(const float bookFraction) {
  float f = bookFraction;
  if (!(f >= 0.0f)) f = 0.0f;  // also catches NaN
  if (f > 1.0f) f = 1.0f;
  return static_cast<int32_t>(f * static_cast<float>(PCT_SCALE) + 0.5f);
}

int pctToDisplayPercent(const int32_t pct) {
  if (!pctValid(pct)) return 0;
  return static_cast<int>((pct + PCT_SCALE / 200) / (PCT_SCALE / 100));
}

std::string serialize(const std::string& bookTitle, const std::vector<Highlight>& items,
                      const std::vector<Bookmark>& bookmarks) {
  std::vector<Highlight> sortedH = items;
  std::sort(sortedH.begin(), sortedH.end(), highlightLess);
  std::vector<Bookmark> sortedB = bookmarks;
  std::sort(sortedB.begin(), sortedB.end(), bookmarkLess);

  std::string out = "# Highlights — " + bookTitle + "\n\n";
  if (sortedH.empty() && sortedB.empty()) {
    out += "_No highlights yet._\n";
    return out;
  }
  for (const auto& h : sortedH) {
    out += "## Chapter " + std::to_string(h.spine + 1) + ", Page " + std::to_string(h.start.page + 1) + "\n";
    out += "> " + oneLine(h.text) + "\n";
    if (!h.note.empty()) {
      out += "**Note:** " + oneLine(h.note) + "\n";
    }
    out += metaComment(h) + "\n\n";
  }
  for (const auto& b : sortedB) {
    out += "## Bookmark — Chapter " + std::to_string(b.spine + 1) + ", Page " + std::to_string(b.page + 1);
    // Decorative only (headings are skipped on parse), but it is the point of
    // preferring Markdown over JSON: the file should read well on a PC.
    if (pctValid(b.pctX10000)) {
      out += " (" + std::to_string(pctToDisplayPercent(b.pctX10000)) + "%)";
    }
    out += "\n";
    if (!b.text.empty()) {
      out += "> " + oneLine(b.text) + "\n";
    }
    if (!b.note.empty()) {
      out += "**Note:** " + oneLine(b.note) + "\n";
    }
    out += metaCommentBookmark(b) + "\n\n";
  }
  return out;
}

std::vector<Highlight> parse(const std::string& markdown) {
  std::vector<Highlight> out;
  std::string pendingText;
  std::string pendingNote;
  bool haveText = false;

  for (auto line : splitChar(markdown, '\n')) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (startsWith(line, "> ")) {
      const std::string seg = line.substr(2);
      if (haveText) {
        pendingText += " " + seg;
      } else {
        pendingText = seg;
        haveText = true;
      }
    } else if (line == ">") {
      // empty quote line — keep an (empty) pending text alive
      if (!haveText) {
        pendingText.clear();
        haveText = true;
      }
    } else if (startsWith(line, "**Note:** ")) {
      pendingNote = line.substr(10);  // length of "**Note:** "
    } else if (startsWith(line, "<!-- cpx")) {
      Highlight h;
      if (parseMeta(line, h)) {
        h.text = haveText ? pendingText : std::string();
        h.note = pendingNote;
        out.push_back(std::move(h));
      }
      pendingText.clear();
      pendingNote.clear();
      haveText = false;
    } else {
      // heading / blank / prose — detach any accumulated quote + note
      pendingText.clear();
      pendingNote.clear();
      haveText = false;
    }
  }
  return out;
}

std::vector<Bookmark> parseBookmarks(const std::string& markdown) {
  std::vector<Bookmark> out;
  std::string pendingText;
  std::string pendingNote;
  bool haveText = false;

  for (auto line : splitChar(markdown, '\n')) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (startsWith(line, "> ")) {
      const std::string seg = line.substr(2);
      if (haveText) {
        pendingText += " " + seg;
      } else {
        pendingText = seg;
        haveText = true;
      }
    } else if (line == ">") {
      if (!haveText) {
        pendingText.clear();
        haveText = true;
      }
    } else if (startsWith(line, "**Note:** ")) {
      pendingNote = line.substr(10);
    } else if (startsWith(line, "<!-- cpx")) {
      // Both `cpx` (highlight) and `cpx-bm` (bookmark) lines land here; only the
      // bookmark ones parse — highlight lines clear the pending block and pass.
      Bookmark b;
      if (parseBookmarkMeta(line, b)) {
        b.text = haveText ? pendingText : std::string();
        b.note = pendingNote;
        out.push_back(std::move(b));
      }
      pendingText.clear();
      pendingNote.clear();
      haveText = false;
    } else {
      pendingText.clear();
      pendingNote.clear();
      haveText = false;
    }
  }
  return out;
}

}  // namespace highlight
