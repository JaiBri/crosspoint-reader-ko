#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <cstdint>
#include <string>
#include <vector>

#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"
#include "util/HighlightStore.h"
#include "util/ReadingSpeedStore.h"
#include "util/ReadingTimer.h"

class Page;

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedFontId = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;

  // Per-book reading-time accumulator. Loaded in onEnter, ticked in loop,
  // saved on mid-cadence and onExit. See util/ReadingTimer.h for semantics.
  ReadingTimer readingTimer;

  // Reading-speed tracking: per-page durations -> median -> "time left" estimate.
  // A page's duration is the delta of readingTimer.totalSeconds() across a
  // forward page turn, so menu/idle time is already excluded. Raw durations are
  // logged to the global /.crosspoint/reading_speed.md (util/ReadingSpeedStore.h).
  readingspeed::Library readingLibrary;
  uint32_t pageStartActiveSec = 0;     // readingTimer.totalSeconds() when the current page became visible
  int timedSpine = -1;                 // (spine,page) that pageStartActiveSec refers to; -1 = none yet
  int timedPage = -1;                  // re-anchored in render() whenever the displayed page changes
  uint32_t medianSecPerPageCache = 0;  // median over this book's realistic durations (0 = none)
  size_t medianSampleCount = 0;        // #realistic samples behind the median
  int recordedSinceFlush = 0;          // durations recorded since the last SD write

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // ---------------------------------------------------------------------------
  // Highlight feature (character-level, page-relative, in-chapter selection).
  // Implemented as an internal sub-mode (mirrors automaticPageTurnActive): a
  // flag changes input handling in loop() and adds an overlay pass in
  // renderContents(). Reuses the reader's live Section + page-turn machinery so
  // selection flows across page boundaries within a chapter.
  // ---------------------------------------------------------------------------
  // Per-word screen geometry for the current page, used to map a character
  // offset to an on-screen pixel position.
  struct HlWord {
    int16_t x = 0;             // absolute pixel x of the word's first glyph
    uint8_t style = 0;         // EpdFontFamily::Style bitmask
    int logicalStart = 0;      // ch index of this word's first char in the line's logical string
    int chars = 0;             // codepoint count of the word
    std::string text;          // word bytes (UTF-8)
    std::vector<int16_t> bx;   // size chars+1, absolute x of each char boundary
  };
  struct HlLine {
    int16_t yTop = 0;          // absolute pixel y of the line's top
    int charCount = 0;         // total logical chars (words joined by single spaces)
    std::string text;          // logical line string
    std::vector<HlWord> words;
  };
  enum class HlPending { None, PageStart, PageEnd };

  bool highlightMode = false;          // in highlight-selection sub-mode
  bool hlSelecting = false;            // start anchor placed, extending to cursor
  HlPending hlPending = HlPending::None;
  highlight::Pos hlAnchor;             // locked start word's start (page-relative, current chapter)
  highlight::Pos hlAnchorEnd;          // locked start word's end (used when extending backward)
  highlight::Pos hlCursor;             // moving cursor
  highlight::Highlight pendingHighlight;  // built on commit, stored after the optional-comment chain
  std::vector<highlight::Highlight> highlights;  // loaded for this book
  std::vector<highlight::Bookmark> bookmarks;    // loaded for this book (same sidecar file)
  highlight::Bookmark pendingBookmark;           // built on create, stored after the optional-comment chain
  bool sidecarLoaded = false;
  std::vector<HlLine> pageGeom;        // current page geometry (rebuilt each render)
  int hlMarginLeft = 0;
  int hlMarginTop = 0;
  uint16_t hlViewportW = 0;            // pagination viewport (for layout fingerprint)
  uint16_t hlViewportH = 0;

  void loadSidecarIfNeeded();  // loads BOTH highlights and bookmarks from the one .md file
  void saveSidecar();          // writes BOTH back (highlights-only write would erase bookmarks)
  void enterHighlightMode();
  void exitHighlightMode();
  void handleHighlightInput();
  void highlightPageTurn(bool forward);  // within-chapter page step for the cursor
  // Word-granular cursor navigation over the current page geometry (pageGeom).
  int hlWordIndex(const HlLine& line, int ch) const;  // word containing/at ch, or -1 if line empty
  int hlCursorWordEnd() const;                        // logical-char end of the word under the cursor
  void hlMoveForwardWord();   // Right: next word, turning the page at the end
  void hlMoveBackwardWord();  // Left: previous word, turning the page at the start
  void hlMoveLine(int dir);   // Up(-1)/Down(+1): word nearest the cursor's x in the adjacent line
  void commitHighlight();
  void promptForHighlightComment();
  void storePendingHighlight();
  highlight::LayoutParams currentLayout() const;
  bool hasHighlightsForCurrentView() const;
  void rebuildPageGeom(const Page& page);
  void syncHighlightCursorToPage();
  void drawHighlightOverlay();
  void drawHighlightSpan(const HlLine& line, int chFrom, int chTo);
  int16_t boundaryXAt(const HlLine& line, int ch) const;
  std::string extractText(const highlight::Pos& a, const highlight::Pos& b);
  std::vector<std::string> pageLineTexts(int pageIndex);
  void openHighlightsList();

  // Bookmarks (whole-page marks; stored in the same sidecar file).
  void addBookmarkForCurrentPage();  // build pendingBookmark, then the optional-comment chain
  void promptForBookmarkComment();
  void storePendingBookmark();
  void openBookmarksList();
  // Give pre-`pc=` bookmarks a layout-independent anchor where it can be derived
  // exactly. Must run before the first render — see the definition.
  void migrateBookmarkPercentages();
  // Decode a bookmark's book-progress anchor into (spine, intra-spine fraction).
  // False when the bookmark has no anchor or the book metadata is unusable.
  bool bookPctToSpineTarget(const highlight::Bookmark& bm, int& spineOut, float& fracOut) const;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  // Jump to a normalized 0.0-1.0 position within a known spine item. The page is
  // resolved by render() once the section is paginated at the current settings,
  // so this is safe across layout changes (unlike a stored page index).
  void jumpToSpineFraction(int spineIndex, float spineFraction);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);

  // Reading-speed helpers (see the readingLibrary members above).
  void loadReadingSpeed();                     // parse the global file + prime the median cache
  void recordPageDuration(uint32_t seconds);   // append one page's reading time + periodic flush
  void saveReadingSpeed();                     // serialize the whole library to SD
  void refreshMedianCache();                   // recompute median/sample-count for the current book

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  // Prevent auto-sleep while auto page-turn is running so long unattended
  // reads don't get cut off by the global inactivity timer.
  bool preventAutoSleep() override { return automaticPageTurnActive; }
};
