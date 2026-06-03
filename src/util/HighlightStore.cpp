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

// Replace CR/LF with spaces so a passage never breaks the single-line `>` quote.
std::string oneLine(const std::string& in) {
  std::string out = in;
  for (char& c : out) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  return out;
}

std::string metaComment(const Highlight& h) {
  const auto& l = h.layout;
  std::string s = "<!-- cpx v1 sp=";
  s += std::to_string(h.spine);
  s += " a=" + std::to_string(h.start.page) + "," + std::to_string(h.start.line) + "," + std::to_string(h.start.ch);
  s += " b=" + std::to_string(h.end.page) + "," + std::to_string(h.end.line) + "," + std::to_string(h.end.ch);
  s += " ly=";
  s += std::to_string(l.fontId) + "," + std::to_string(l.lineCompressionX100) + "," + std::to_string(l.viewportWidth) +
       "," + std::to_string(l.viewportHeight) + "," + std::to_string(l.paragraphAlignment) + "," +
       std::to_string(l.characterWrap) + "," + std::to_string(l.hyphenationEnabled) + "," +
       std::to_string(l.embeddedStyle) + "," + std::to_string(l.imageRendering) + "," +
       std::to_string(l.extraParagraphSpacing) + "," + std::to_string(l.paragraphIndent);
  s += " -->";
  return s;
}

// Parse one "<!-- cpx v1 ... -->" line into a Highlight (text left empty).
// Returns false if the schema/version doesn't match or fields are malformed.
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
      const auto parts = splitChar(val, ',');
      if (parts.size() != 11) return false;
      long n[11];
      for (int i = 0; i < 11; i++) {
        if (!parseUInt(parts[i], n[i])) return false;
      }
      LayoutParams& l = out.layout;
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
      haveLy = true;
    }
  }
  return haveSp && haveA && haveB && haveLy;
}

}  // namespace

std::string sidecarPath(const std::string& bookPath) { return bookPath + ".highlights.md"; }

bool highlightLess(const Highlight& a, const Highlight& b) {
  if (a.spine != b.spine) return a.spine < b.spine;
  if (!posEqual(a.start, b.start)) return posLess(a.start, b.start);
  return posLess(a.end, b.end);
}

std::string serialize(const std::string& bookTitle, const std::vector<Highlight>& items) {
  std::vector<Highlight> sorted = items;
  std::sort(sorted.begin(), sorted.end(), highlightLess);

  std::string out = "# Highlights — " + bookTitle + "\n\n";
  if (sorted.empty()) {
    out += "_No highlights yet._\n";
    return out;
  }
  for (const auto& h : sorted) {
    out += "## Chapter " + std::to_string(h.spine + 1) + ", Page " + std::to_string(h.start.page + 1) + "\n";
    out += "> " + oneLine(h.text) + "\n";
    if (!h.note.empty()) {
      out += "**Note:** " + oneLine(h.note) + "\n";
    }
    out += metaComment(h) + "\n\n";
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

}  // namespace highlight
