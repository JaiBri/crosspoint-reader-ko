// Native host test for src/util/ReadingSpeedStore.{h,cpp}.
// Build/run via test/run_reading_speed_store_test.sh (no PlatformIO needed).
//
// Covers: median with the realistic-time cutoff (drops too-short/too-long;
// odd/even counts), addDuration upsert + newest-N cap, find(), a multi-book
// serialize->parse round-trip (incl. titles/paths with spaces), sorted-by-book
// output, the < kMinSamples sample-count gate, and tolerance of garbled input.

#include <cstdio>
#include <string>
#include <vector>

#include "util/ReadingSpeedStore.h"

using readingspeed::BookStats;
using readingspeed::kMaxDurationsPerBook;
using readingspeed::kMaxRealisticSec;
using readingspeed::kMinRealisticSec;
using readingspeed::kMinSamples;
using readingspeed::Library;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

static void testMedianCutoffOdd() {
  // 2 and 600 are out of the [5,60] window and must be dropped.
  // Remaining {10, 30, 50} -> median 30, count 3.
  std::vector<uint32_t> d = {2, 10, 30, 50, 600};
  std::size_t count = 0;
  const uint32_t m = readingspeed::medianSecPerPage(d, kMinRealisticSec, kMaxRealisticSec, count);
  CHECK(count == 3);
  CHECK(m == 30);
}

static void testMedianCutoffEven() {
  // In-window {10, 20, 30, 40} -> even -> average of 20 and 30 = 25.
  std::vector<uint32_t> d = {1, 10, 20, 30, 40, 1000};
  std::size_t count = 0;
  const uint32_t m = readingspeed::medianSecPerPage(d, kMinRealisticSec, kMaxRealisticSec, count);
  CHECK(count == 4);
  CHECK(m == 25);
}

static void testMedianBoundariesInclusive() {
  // Exactly min and max must be kept.
  std::vector<uint32_t> d = {kMinRealisticSec, kMaxRealisticSec};
  std::size_t count = 0;
  const uint32_t m = readingspeed::medianSecPerPage(d, kMinRealisticSec, kMaxRealisticSec, count);
  CHECK(count == 2);
  CHECK(m == (kMinRealisticSec + kMaxRealisticSec + 1) / 2);
}

static void testMedianNoneInWindow() {
  std::vector<uint32_t> d = {1, 2, 3, 4, 500, 9000};
  std::size_t count = 0;
  const uint32_t m = readingspeed::medianSecPerPage(d, kMinRealisticSec, kMaxRealisticSec, count);
  CHECK(count == 0);
  CHECK(m == 0);
}

static void testAddDurationUpsertAndTitle() {
  Library lib;
  readingspeed::addDuration(lib, "/books/A.epub", "Alpha", 10, kMaxDurationsPerBook);
  readingspeed::addDuration(lib, "/books/A.epub", "Alpha", 20, kMaxDurationsPerBook);
  readingspeed::addDuration(lib, "/books/B.epub", "Beta", 30, kMaxDurationsPerBook);
  CHECK(lib.books.size() == 2);

  BookStats* a = readingspeed::find(lib, "/books/A.epub");
  CHECK(a != nullptr);
  CHECK(a->durations.size() == 2);
  CHECK(a->durations[0] == 10 && a->durations[1] == 20);
  CHECK(a->title == "Alpha");

  // A later non-empty title refreshes; an empty title leaves it untouched.
  readingspeed::addDuration(lib, "/books/A.epub", "Alpha v2", 40, kMaxDurationsPerBook);
  CHECK(a->title == "Alpha v2");
  readingspeed::addDuration(lib, "/books/A.epub", "", 50, kMaxDurationsPerBook);
  CHECK(a->title == "Alpha v2");

  CHECK(readingspeed::find(lib, "/books/missing.epub") == nullptr);
}

static void testAddDurationCapKeepsNewest() {
  Library lib;
  const std::size_t cap = 300;
  for (uint32_t i = 1; i <= 305; i++) {
    readingspeed::addDuration(lib, "/books/C.epub", "Cap", i, cap);
  }
  BookStats* c = readingspeed::find(lib, "/books/C.epub");
  CHECK(c != nullptr);
  CHECK(c->durations.size() == cap);
  // Oldest five (1..5) dropped; window is 6..305.
  CHECK(c->durations.front() == 6);
  CHECK(c->durations.back() == 305);
}

static void testRoundTripMultiBook() {
  Library lib;
  // Title and path with spaces — must survive because we extract by quote, not
  // by splitting the line on spaces.
  readingspeed::addDuration(lib, "/books/On Giving Up.epub", "On Giving Up", 40, kMaxDurationsPerBook);
  readingspeed::addDuration(lib, "/books/On Giving Up.epub", "On Giving Up", 35, kMaxDurationsPerBook);
  readingspeed::addDuration(lib, "/books/Dune.epub", "Dune", 12, kMaxDurationsPerBook);

  const std::string md = readingspeed::serialize(lib);

  // Sorted by title: "Dune" before "On Giving Up".
  CHECK(md.find("## Dune") < md.find("## On Giving Up"));
  CHECK(md.find("<!-- rs v1 path=\"/books/Dune.epub\" title=\"Dune\" durs=12 -->") != std::string::npos);
  CHECK(md.find("<!-- rs v1 path=\"/books/On Giving Up.epub\" title=\"On Giving Up\" durs=40,35 -->") !=
        std::string::npos);

  const Library back = readingspeed::parse(md);
  CHECK(back.books.size() == 2);

  Library mut = back;
  BookStats* dune = readingspeed::find(mut, "/books/Dune.epub");
  BookStats* ogu = readingspeed::find(mut, "/books/On Giving Up.epub");
  CHECK(dune != nullptr && ogu != nullptr);
  CHECK(dune->title == "Dune");
  CHECK(dune->durations.size() == 1 && dune->durations[0] == 12);
  CHECK(ogu->title == "On Giving Up");
  CHECK(ogu->durations.size() == 2 && ogu->durations[0] == 40 && ogu->durations[1] == 35);
}

static void testSummaryReflectsSampleGate() {
  Library lib;
  // 3 realistic samples (< kMinSamples) -> "Not enough data yet".
  for (uint32_t i = 0; i < 3; i++) readingspeed::addDuration(lib, "/books/Few.epub", "Few", 30, kMaxDurationsPerBook);
  std::string md = readingspeed::serialize(lib);
  CHECK(md.find("Not enough data yet (3 pages logged)") != std::string::npos);

  // Push to kMinSamples realistic samples -> "Median ... pages sampled".
  for (uint32_t i = 3; i < kMinSamples; i++)
    readingspeed::addDuration(lib, "/books/Few.epub", "Few", 30, kMaxDurationsPerBook);
  md = readingspeed::serialize(lib);
  CHECK(md.find("Median 30 s/page") != std::string::npos);
  CHECK(md.find("pages sampled") != std::string::npos);
}

static void testParseTolerantOfGarbage() {
  CHECK(readingspeed::parse("").books.empty());
  CHECK(readingspeed::parse("# Reading speed\n\njust some prose\n## Heading only\n").books.empty());

  // A line missing the path attribute is skipped; a valid one is kept.
  const std::string md =
      "<!-- rs v1 title=\"No Path\" durs=10,20 -->\n"
      "<!-- rs v1 path=\"/books/Good.epub\" title=\"Good\" durs=7,8,garbage,9 -->\n";
  const Library lib = readingspeed::parse(md);
  CHECK(lib.books.size() == 1);
  Library mut = lib;
  BookStats* g = readingspeed::find(mut, "/books/Good.epub");
  CHECK(g != nullptr);
  // "garbage" token dropped; the numeric ones kept in order.
  CHECK(g->durations.size() == 3);
  CHECK(g->durations[0] == 7 && g->durations[1] == 8 && g->durations[2] == 9);
}

int main() {
  testMedianCutoffOdd();
  testMedianCutoffEven();
  testMedianBoundariesInclusive();
  testMedianNoneInWindow();
  testAddDurationUpsertAndTitle();
  testAddDurationCapKeepsNewest();
  testRoundTripMultiBook();
  testSummaryReflectsSampleGate();
  testParseTolerantOfGarbage();

  if (g_failures == 0) {
    std::printf("ReadingSpeedStore: ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("ReadingSpeedStore: %d CHECK(s) FAILED\n", g_failures);
  return 1;
}
