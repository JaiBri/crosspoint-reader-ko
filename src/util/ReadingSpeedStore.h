#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ReadingSpeedStore — pure model + Markdown (de)serialization for per-page
// reading-speed tracking. Deliberately free of any HAL/Storage/Arduino
// dependency so it can be unit-tested natively on the host (see
// test/reading_speed_store/). The SD-card read/write happens at the call site
// via Storage.readFile/writeFile; this module only turns the Library <-> text
// and owns the median/cutoff maths.
//
// Persistence is GLOBAL (one file, not per-book): "/.crosspoint/reading_speed.md".
// The file holds one "## <book>" section per book, sorted by book. Each book's
// `<!-- rs v1 ... -->` HTML-comment line is the machine source of truth (it
// carries the book path, title, and the raw per-page durations); the summary
// line above it is decorative and ignored on parse, so the file stays nice to
// read on a PC.
//
// Durations are stored RAW (every measured page). The unrealistic-time cutoff
// is applied at compute time inside medianSecPerPage(), so the window can be
// re-tuned later without losing data.

namespace readingspeed {

// Realistic per-page window: shorter == flipping to find a page; longer == not
// actively reading that one page (paused, distracted). Durations outside
// [kMinRealisticSec, kMaxRealisticSec] are ignored when taking the median.
inline constexpr uint32_t kMinRealisticSec = 5;
inline constexpr uint32_t kMaxRealisticSec = 60;
// Need at least this many realistic samples before an estimate is meaningful.
inline constexpr std::size_t kMinSamples = 5;
// Cap stored durations per book (bounds the global file + RAM). Newest kept.
inline constexpr std::size_t kMaxDurationsPerBook = 300;

struct BookStats {
  std::string path;                 // SD path; the stable key for a book
  std::string title;                // for the human-readable heading
  std::vector<uint32_t> durations;  // raw per-page reading time, seconds
};

struct Library {
  std::vector<BookStats> books;
};

// Median of the durations that fall within [minSec, maxSec]. `outCount` is set
// to the number of in-window samples used. Returns 0 when none qualify (caller
// should also gate on outCount >= kMinSamples before trusting the value).
uint32_t medianSecPerPage(const std::vector<uint32_t>& durations, uint32_t minSec, uint32_t maxSec,
                          std::size_t& outCount);

// Find a book by path. Returns nullptr if absent.
BookStats* find(Library& lib, const std::string& path);

// Append one page duration for `path` (creating the book entry and setting its
// title if absent; refreshing a non-empty title otherwise). Trims to the newest
// `maxKeep` durations (no trim when maxKeep == 0).
void addDuration(Library& lib, const std::string& path, const std::string& title, uint32_t sec, std::size_t maxKeep);

// Render the whole library to Markdown, sorted by book title (then path).
std::string serialize(const Library& lib);

// Parse the file text back into a Library. Tolerant of hand-edits and a
// missing/garbled file (returns an empty Library). Books are keyed by the
// `path="..."` attribute of each `<!-- rs v1 ... -->` line.
Library parse(const std::string& markdown);

}  // namespace readingspeed
