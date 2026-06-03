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
  highlight::Pos hlAnchor;             // selection start (page-relative, current chapter)
  highlight::Pos hlCursor;             // moving cursor
  highlight::Highlight pendingHighlight;  // built on commit, stored after the optional-comment chain
  std::vector<highlight::Highlight> highlights;  // loaded for this book
  bool highlightsLoaded = false;
  std::vector<HlLine> pageGeom;        // current page geometry (rebuilt each render)
  int hlMarginLeft = 0;
  int hlMarginTop = 0;
  uint16_t hlViewportW = 0;            // pagination viewport (for layout fingerprint)
  uint16_t hlViewportH = 0;

  void loadHighlightsIfNeeded();
  void saveHighlights();
  void enterHighlightMode();
  void exitHighlightMode();
  void handleHighlightInput();
  void highlightPageTurn(bool forward);  // within-chapter page step for the cursor
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

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);

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
