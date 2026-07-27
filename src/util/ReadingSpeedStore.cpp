#include "ReadingSpeedStore.h"

#include <algorithm>
#include <cstdlib>

namespace readingspeed {

namespace {

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

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

// Make a string safe to embed in a key="..." attribute / a heading: collapse
// CR/LF to spaces and swap any double-quote for a single quote. (SD paths on
// FAT/exFAT can contain neither, so the path key survives a round-trip; this
// only ever sanitises pathological titles.)
std::string sanitizeAttr(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in) {
    if (c == '\n' || c == '\r')
      out += ' ';
    else if (c == '"')
      out += '\'';
    else
      out += c;
  }
  return out;
}

// Extract the value of `keyEq` (e.g. `path="`) up to the next double-quote.
bool parseQuoted(const std::string& s, const char* keyEq, std::string& out) {
  const size_t k = s.find(keyEq);
  if (k == std::string::npos) return false;
  const size_t start = k + std::char_traits<char>::length(keyEq);
  const size_t end = s.find('"', start);
  if (end == std::string::npos) return false;
  out = s.substr(start, end - start);
  return true;
}

}  // namespace

uint32_t medianSecPerPage(const std::vector<uint32_t>& durations, uint32_t minSec, uint32_t maxSec,
                          std::size_t& outCount) {
  std::vector<uint32_t> f;
  f.reserve(durations.size());
  for (const uint32_t v : durations) {
    if (v >= minSec && v <= maxSec) f.push_back(v);
  }
  outCount = f.size();
  if (f.empty()) return 0;

  std::sort(f.begin(), f.end());
  const size_t n = f.size();
  if (n % 2 == 1) return f[n / 2];
  // Even count: average the two middle values, rounded to nearest.
  const uint64_t a = f[n / 2 - 1];
  const uint64_t b = f[n / 2];
  return static_cast<uint32_t>((a + b + 1) / 2);
}

BookStats* find(Library& lib, const std::string& path) {
  for (auto& b : lib.books) {
    if (b.path == path) return &b;
  }
  return nullptr;
}

void addDuration(Library& lib, const std::string& path, const std::string& title, uint32_t sec,
                 std::size_t maxKeep) {
  BookStats* b = find(lib, path);
  if (!b) {
    lib.books.push_back(BookStats{path, title, {}});
    b = &lib.books.back();
  } else if (!title.empty()) {
    b->title = title;  // keep the most recently seen title
  }
  b->durations.push_back(sec);
  if (maxKeep > 0 && b->durations.size() > maxKeep) {
    b->durations.erase(b->durations.begin(),
                       b->durations.begin() + static_cast<long>(b->durations.size() - maxKeep));
  }
}

std::string serialize(const Library& lib) {
  // Sort by title (then path) for a stable, human-friendly "sorted by book" file.
  std::vector<const BookStats*> ordered;
  ordered.reserve(lib.books.size());
  for (const auto& b : lib.books) ordered.push_back(&b);
  std::sort(ordered.begin(), ordered.end(), [](const BookStats* a, const BookStats* b) {
    if (a->title != b->title) return a->title < b->title;
    return a->path < b->path;
  });

  std::string out = "# Reading speed\n\n";

  for (const BookStats* b : ordered) {
    const std::string heading = b->title.empty() ? b->path : b->title;
    out += "## " + sanitizeAttr(heading) + "\n";

    std::size_t count = 0;
    const uint32_t median = medianSecPerPage(b->durations, kMinRealisticSec, kMaxRealisticSec, count);
    if (count >= kMinSamples) {
      out += "Median " + std::to_string(median) + " s/page · " + std::to_string(count) + " pages sampled\n";
    } else {
      out += "Not enough data yet (" + std::to_string(b->durations.size()) + " pages logged)\n";
    }

    out += "<!-- rs v1 path=\"" + sanitizeAttr(b->path) + "\" title=\"" + sanitizeAttr(b->title) + "\" durs=";
    for (size_t i = 0; i < b->durations.size(); i++) {
      if (i) out += ",";
      out += std::to_string(b->durations[i]);
    }
    out += " -->\n\n";
  }

  return out;
}

Library parse(const std::string& markdown) {
  Library lib;
  for (auto line : splitChar(markdown, '\n')) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!startsWith(line, "<!-- rs v1 ")) continue;

    BookStats bs;
    if (!parseQuoted(line, "path=\"", bs.path)) continue;  // path is the key; skip if missing
    parseQuoted(line, "title=\"", bs.title);               // title optional

    const size_t d = line.find("durs=");
    if (d != std::string::npos) {
      std::string rest = line.substr(d + 5);  // strlen("durs=") == 5
      const size_t sp = rest.find(' ');        // durs CSV has no spaces; stop before " -->"
      if (sp != std::string::npos) rest = rest.substr(0, sp);
      for (const auto& tok : splitChar(rest, ',')) {
        long v;
        if (parseUInt(tok, v)) bs.durations.push_back(static_cast<uint32_t>(v));
      }
    }
    lib.books.push_back(std::move(bs));
  }
  return lib;
}

}  // namespace readingspeed
