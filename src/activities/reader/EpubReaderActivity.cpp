#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_system.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "HighlightListActivity.h"
#include "activities/settings/ReaderOptionsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HighlightStore.h"
#include "util/ScreenshotUtil.h"

#include <Utf8.h>

#include <algorithm>
#include <utility>

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
// pages per minute, first item is 1 to prevent division by zero if accessed
const std::vector<int> PAGE_TURN_LABELS = {1, 1, 3, 6, 12};

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Cache current font ID for change detection
  cachedFontId = SETTINGS.getReaderFontId();

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Begin per-book reading time accumulation. Cache dir was just ensured above.
  readingTimer.start(epub->getCachePath());

  // Load existing highlights for this book (sidecar Markdown next to the file).
  loadHighlightsIfNeeded();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Persist accumulated session time before tearing down the book.
  readingTimer.stop();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // Accumulate elapsed reading time. Sub-activity gaps are absorbed by the
  // timer's per-tick clamp; idle pause kicks in after IDLE_TIMEOUT_MS without
  // a notifyInput() call.
  readingTimer.tick();

  // Highlight sub-mode takes over all input until accept (Confirm) or cancel (Back).
  if (highlightMode) {
    handleHighlightInput();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      readingTimer.notifyInput();
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      // Auto-turn counts as activity so the 5-min idle pause doesn't cut off
      // long unattended reads on the user's chosen cadence.
      readingTimer.notifyInput();
      pageTurn(true);
      return;
    }
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    readingTimer.notifyInput();
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    // Snapshot every layout-affecting setting + orientation before opening the
    // menu. The menu hosts Reader Options as a sub-activity, so all changes
    // (whether from the menu's inline rotate cycle or from Reader Options
    // direct writes) are reconciled here when the menu finally exits — that
    // way the user sees a single re-layout instead of one per setting touched.
    const uint8_t menuOrientationSnapshot = SETTINGS.orientation;
    const uint8_t menuLineSpacing = SETTINGS.lineSpacing;
    const uint8_t menuScreenMargin = SETTINGS.screenMargin;
    const uint8_t menuParagraphAlignment = SETTINGS.paragraphAlignment;
    const uint8_t menuEmbeddedStyle = SETTINGS.embeddedStyle;
    const uint8_t menuExtraParagraphSpacing = SETTINGS.extraParagraphSpacing;
    const uint8_t menuParagraphIndent = SETTINGS.paragraphIndent;
    const uint8_t menuCharacterWrap = SETTINGS.characterWrap;
    const uint8_t menuTextAntiAliasing = SETTINGS.textAntiAliasing;
    const uint8_t menuImageRendering = SETTINGS.imageRendering;
    const int menuFontId = SETTINGS.getReaderFontId();
    startActivityForResult(
        std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, epub->getTitle(), currentPage, totalPages,
                                                 bookProgressPercent, SETTINGS.orientation,
                                                 !currentPageFootnotes.empty(), readingTimer.totalSeconds()),
        [this, menuOrientationSnapshot, menuLineSpacing, menuScreenMargin, menuParagraphAlignment, menuEmbeddedStyle,
         menuExtraParagraphSpacing, menuParagraphIndent, menuCharacterWrap, menuTextAntiAliasing, menuImageRendering,
         menuFontId](const ActivityResult& result) {
          const auto& menu = std::get<MenuResult>(result.data);

          // Reconcile orientation: the menu's pending rotate-screen cycle and
          // Reader Options' direct write both feed into the same setting.
          // Pick whichever differs from the snapshot (menu cycle wins ties so
          // the existing rotate-screen UX is preserved).
          uint8_t finalOrientation = SETTINGS.orientation;
          if (menu.orientation != menuOrientationSnapshot) {
            finalOrientation = menu.orientation;
          }
          // applyOrientation() expects the SETTING to still hold the OLD
          // value — restore the snapshot first so the helper does its full
          // re-layout when the orientation actually changed.
          SETTINGS.orientation = menuOrientationSnapshot;
          applyOrientation(finalOrientation);
          toggleAutoPageTurn(menu.pageTurnOption);

          // applyOrientation() already invalidates the section when it
          // changes orientation. For other layout-affecting settings we
          // detect changes here and invalidate manually.
          const bool orientationChanged = finalOrientation != menuOrientationSnapshot;
          if (!orientationChanged) {
            const bool layoutChanged =
                menuLineSpacing != SETTINGS.lineSpacing || menuScreenMargin != SETTINGS.screenMargin ||
                menuParagraphAlignment != SETTINGS.paragraphAlignment || menuEmbeddedStyle != SETTINGS.embeddedStyle ||
                menuExtraParagraphSpacing != SETTINGS.extraParagraphSpacing ||
                menuParagraphIndent != SETTINGS.paragraphIndent || menuCharacterWrap != SETTINGS.characterWrap ||
                menuTextAntiAliasing != SETTINGS.textAntiAliasing || menuImageRendering != SETTINGS.imageRendering ||
                menuFontId != SETTINGS.getReaderFontId();
            if (layoutChanged) {
              RenderLock lock(*this);
              section.reset();
            }
          }

          readingTimer.notifyInput();
          if (!result.isCancelled) {
            onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
          }
        });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    readingTimer.notifyInput();
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }
  readingTimer.notifyInput();

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = UINT16_MAX;
      requestUpdate();
    }
    return;
  }

  const bool skipChapter = !fromTilt && SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (skipChapter) {
    lastPageTurnTime = millis();
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          saveProgress(backupSpine, backupPage, backupPageCount);
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->pageCount : 0;
        startActivityForResult(
            std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, epub, epub->getPath(), currentSpineIndex,
                                                   currentPage, totalPages),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& sync = std::get<SyncResult>(result.data);
                if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
                  RenderLock lock(*this);
                  currentSpineIndex = sync.spineIndex;
                  nextPageNumber = sync.page;
                  section.reset();
                }
              }
            });
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::RESET_READING_TIMER: {
      // Two-step destructive action: prompt before zeroing the cumulative time.
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_RESET_READING_TIMER),
                                                                    tr(STR_RESET_READING_TIMER_PROMPT)),
                             [this](const ActivityResult& result) {
                               readingTimer.notifyInput();
                               if (!result.isCancelled) {
                                 readingTimer.reset();
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::HIGHLIGHT: {
      enterHighlightMode();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::HIGHLIGHTS: {
      openHighlightsList();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS:
      // Reader Options is launched as a sub-activity of the menu itself,
      // never dispatched here. Layout/orientation changes are reconciled
      // when the menu finally exits (see the menu's result handler in loop()).
      break;
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= PAGE_TURN_LABELS.size()) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // Detect font change and invalidate section cache
  const int currentFontId = SETTINGS.getReaderFontId();
  if (cachedFontId != 0 && cachedFontId != currentFontId) {
    LOG_DBG("ERS", "Font changed from %d to %d, invalidating section", cachedFontId, currentFontId);
    section.reset();
  }
  cachedFontId = currentFontId;

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // corrupted progress data (spine index beyond book bounds) - reset to start
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    LOG_ERR("ERS", "Spine index %d out of range (max %d), resetting to start", currentSpineIndex,
            epub->getSpineItemsCount());
    currentSpineIndex = 0;
    nextPageNumber = 0;
    section.reset();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  // Record the pagination viewport so highlight anchors can detect a layout
  // change (font/orientation/margins) and skip drawing a now-misaligned overlay.
  hlViewportW = viewportWidth;
  hlViewportH = viewportHeight;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphIndent, SETTINGS.paragraphAlignment,
                                  SETTINGS.characterWrap, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                  SETTINGS.embeddedStyle, SETTINGS.imageRendering)) {
      LOG_DBG("ERS", "Cache not found, building...");

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      if (!section->createSectionFile(
              SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
              SETTINGS.paragraphIndent, SETTINGS.paragraphAlignment, SETTINGS.characterWrap, viewportWidth,
              viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering, popupFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    // Korean: clamp to last page on font-change-induced mismatch instead of showing error.
    LOG_DBG("ERS", "Page out of bounds: %d (max %d), clamping to last page", section->currentPage, section->pageCount);
    section->currentPage = section->pageCount - 1;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphIndent, SETTINGS.paragraphAlignment,
                                  SETTINGS.characterWrap, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                  SETTINGS.embeddedStyle, SETTINGS.imageRendering)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphIndent,
                                     SETTINGS.paragraphAlignment, SETTINGS.characterWrap, viewportWidth, viewportHeight,
                                     SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  FsFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", spineIndex, currentPage);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  // Margins are needed by the highlight overlay to position the cursor/spans.
  hlMarginLeft = orientedMarginLeft;
  hlMarginTop = orientedMarginTop;

  // Heap headroom thresholds (contiguous block, not total free):
  //   - PNG decoder needs ~44 KB contiguous
  //   - Grayscale BW snapshot uses 6× 8 KB chunks (chunked, doesn't need contiguous)
  //   - Page render allocations + status bar add ~10–20 KB
  // Below ~50 KB max-alloc we've seen the cascading PNG/CSS/BW failures that
  // led to the ko.17 priority-inheritance crash. Skip the heaviest paths
  // (image-AA double render, grayscale anti-aliasing) and fall back to a
  // BW-only render when contiguous heap is below the gate.
  static constexpr uint32_t MIN_MAX_ALLOC_FOR_HEAVY_RENDER = 50 * 1024;
  const uint32_t heapBefore = esp_get_free_heap_size();
  const uint32_t maxAllocBefore = ESP.getMaxAllocHeap();
  const uint32_t minFreeBefore = ESP.getMinFreeHeap();
  const bool heapStressed = maxAllocBefore < MIN_MAX_ALLOC_FOR_HEAVY_RENDER;
  if (heapStressed) {
    LOG_ERR("ERS", "Heap stressed: maxAlloc=%lu < %u — disabling grayscale + image-AA double render", maxAllocBefore,
            MIN_MAX_ALLOC_FOR_HEAVY_RENDER);
  }
  LOG_DBG("ERS", "Heap entry: free=%lu maxAlloc=%lu minFree=%lu", heapBefore, maxAllocBefore, minFreeBefore);

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  // cppcheck-suppress unreadVariable  ; referenced only inside LOG_DBG, which compiles out at LOG_LEVEL<2
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap after prewarm: free=%lu delta=%ld", heapAfter, (int32_t)heapAfter - (int32_t)heapBefore);

  // Force special handling for pages with images when anti-aliasing is on,
  // but skip the heavy double-render path under heap stress. The single BW
  // render below still produces a readable page.
  bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing && !heapStressed;

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();

  // Highlight overlay: composite onto the BW buffer after the page + status bar
  // but before storeBwBuffer(), so it is part of the base image shown by both
  // the BW refresh and the grayscale anti-aliasing pass.
  if (highlightMode || hasHighlightsForCurrentView()) {
    rebuildPageGeom(*page);
    if (highlightMode) {
      syncHighlightCursorToPage();
    }
    drawHighlightOverlay();
  }

  fcm->logStats("bw_render");
  const auto tBwRender = millis();

  if (imagePageWithAA) {
    // Mid-cycle heap re-check: even if entry was OK, the first page->render()
    // and image decoding may have fragmented heap further. If max-alloc
    // dropped below the gate, fall back to single HALF_REFRESH.
    const uint32_t maxAllocMid = ESP.getMaxAllocHeap();
    if (maxAllocMid < MIN_MAX_ALLOC_FOR_HEAVY_RENDER) {
      LOG_ERR("ERS", "Heap dropped during render: maxAlloc=%lu — skipping image-AA double render", maxAllocMid);
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
      imagePageWithAA = false;  // also skips grayscale below
    } else {
      // Double FAST_REFRESH with selective image blanking (pablohc's technique):
      // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
      // Instead, blank only the image area and do two fast refreshes.
      // Step 1: Display page with image area blanked (text appears, image area white)
      // Step 2: Re-render with images and display again (images appear clean)
      int16_t imgX, imgY, imgW, imgH;
      if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
        renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);

        // Re-render page content to restore images into the blanked area
        // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
        page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      } else {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
    }
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Save bw buffer to reset buffer state after grayscale data sync.
  // Two early-exits here:
  //   1. heapStressed — entry-time max-alloc was already below the gate; skip
  //      storeBwBuffer entirely so we don't even spend cycles on 6× 8 KB
  //      chunk allocs that compete with whatever paint cycle still has to
  //      finish.
  //   2. storeBwBuffer returns false — chunked alloc itself failed mid-way
  //      (rolled back internally). ko.3 fix.
  // In both cases the BW framebuffer already holds the rendered page, so we
  // leave it untouched and skip the grayscale anti-aliasing pass.
  const bool bwStored = !heapStressed && renderer.storeBwBuffer();
  if (heapStressed) {
    LOG_DBG("ERS", "Heap stressed at render entry — skipping grayscale pass without attempting storeBwBuffer");
  } else if (!bwStored) {
    LOG_ERR("ERS", "storeBwBuffer failed (heap fragmented) — skipping grayscale pass");
  }
  const auto tBwStore = millis();

  // grayscale rendering
  // TODO: Only do this if font supports it
  if (bwStored && SETTINGS.textAntiAliasing) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    fcm->logStats("gray");

    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tBwRestore - tBwStore,
            tEnd - t0);
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

// ===========================================================================
// Highlight feature
// ===========================================================================

namespace {

// Count UTF-8 codepoints in a string.
int utf8Len(const std::string& s) {
  int n = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.c_str());
  while (*p) {
    if (utf8NextCodepoint(&p) == 0) break;
    n++;
  }
  return n;
}

// Substring covering codepoints [from, to).
std::string utf8SubstrCp(const std::string& s, int from, int to) {
  if (from < 0) from = 0;
  if (to < from) return std::string();
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.c_str());
  int idx = 0;
  while (idx < from && *p) {
    if (utf8NextCodepoint(&p) == 0) break;
    idx++;
  }
  const char* startPtr = reinterpret_cast<const char*>(p);
  while (idx < to && *p) {
    if (utf8NextCodepoint(&p) == 0) break;
    idx++;
  }
  const char* endPtr = reinterpret_cast<const char*>(p);
  return std::string(startPtr, static_cast<size_t>(endPtr - startPtr));
}

}  // namespace

void EpubReaderActivity::loadHighlightsIfNeeded() {
  if (highlightsLoaded || !epub) {
    return;
  }
  highlightsLoaded = true;
  const std::string path = highlight::sidecarPath(epub->getPath());
  if (!Storage.exists(path.c_str())) {
    return;
  }
  const String content = Storage.readFile(path.c_str());
  if (content.length() > 0) {
    highlights = highlight::parse(std::string(content.c_str()));
    std::sort(highlights.begin(), highlights.end(), highlight::highlightLess);
    LOG_DBG("ERS", "Loaded %u highlights", static_cast<uint32_t>(highlights.size()));
  }
}

void EpubReaderActivity::saveHighlights() {
  if (!epub) {
    return;
  }
  std::sort(highlights.begin(), highlights.end(), highlight::highlightLess);
  const std::string md = highlight::serialize(epub->getTitle(), highlights);
  const std::string path = highlight::sidecarPath(epub->getPath());
  if (!Storage.writeFile(path.c_str(), String(md.c_str()))) {
    LOG_ERR("ERS", "Failed to write highlights sidecar: %s", path.c_str());
  }
}

void EpubReaderActivity::enterHighlightMode() {
  highlightMode = true;
  hlSelecting = false;
  hlCursor.page = section ? static_cast<uint16_t>(section->currentPage) : 0;
  hlCursor.line = 0;
  hlCursor.ch = 0;
  hlPending = HlPending::PageStart;
  requestUpdate();
}

void EpubReaderActivity::exitHighlightMode() {
  highlightMode = false;
  hlSelecting = false;
  hlPending = HlPending::None;
  requestUpdate();
}

void EpubReaderActivity::handleHighlightInput() {
  readingTimer.notifyInput();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitHighlightMode();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!hlSelecting) {
      // First press drops the start anchor.
      hlAnchor = hlCursor;
      hlSelecting = true;
      requestUpdate();
    } else {
      // Second press commits the [anchor, cursor] range. commitHighlight()
      // exits highlight mode and (for a non-empty selection) starts the
      // optional-comment chain that ultimately stores the highlight.
      commitHighlight();
    }
    return;
  }
  if (pageGeom.empty()) {
    return;
  }

  // Side buttons move the cursor by line; front Left/Right move it by character.
  // At a page edge, Down/Right advance and Left retreats to the adjacent page
  // (within the current chapter), so a selection can flow across pages.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (hlCursor.line > 0) {
      hlCursor.line--;
      const int cc = pageGeom[hlCursor.line].charCount;
      if (hlCursor.ch > cc) hlCursor.ch = static_cast<uint16_t>(cc);
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (hlCursor.line + 1 < static_cast<int>(pageGeom.size())) {
      hlCursor.line++;
      const int cc = pageGeom[hlCursor.line].charCount;
      if (hlCursor.ch > cc) hlCursor.ch = static_cast<uint16_t>(cc);
      requestUpdate();
    } else {
      highlightPageTurn(true);  // bottom of page -> next page
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    const HlLine& line = pageGeom[hlCursor.line];
    if (hlCursor.ch < line.charCount) {
      hlCursor.ch++;
      requestUpdate();
    } else if (hlCursor.line + 1 < static_cast<int>(pageGeom.size())) {
      hlCursor.line++;
      hlCursor.ch = 0;
      requestUpdate();
    } else {
      highlightPageTurn(true);  // end of last line -> next page
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (hlCursor.ch > 0) {
      hlCursor.ch--;
      requestUpdate();
    } else if (hlCursor.line > 0) {
      hlCursor.line--;
      hlCursor.ch = static_cast<uint16_t>(pageGeom[hlCursor.line].charCount);
      requestUpdate();
    } else {
      highlightPageTurn(false);  // start of first line -> previous page
    }
    return;
  }
}

void EpubReaderActivity::highlightPageTurn(bool forward) {
  if (!section) {
    return;
  }
  // Stay within the current chapter (no spine reset) so a single highlight
  // never straddles a chapter boundary.
  if (forward) {
    if (section->currentPage + 1 < section->pageCount) {
      section->currentPage++;
      hlPending = HlPending::PageStart;
      lastPageTurnTime = millis();
      requestUpdate();
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
      hlPending = HlPending::PageEnd;
      lastPageTurnTime = millis();
      requestUpdate();
    }
  }
}

void EpubReaderActivity::commitHighlight() {
  highlight::Pos a = hlAnchor;
  highlight::Pos b = hlCursor;
  if (highlight::posLess(b, a)) std::swap(a, b);
  if (highlight::posEqual(a, b)) {
    exitHighlightMode();  // empty selection — nothing to store
    return;
  }

  // Build the highlight but defer storing it until the optional-comment chain
  // resolves (so a note can be attached). Keep it in pendingHighlight across the
  // async ConfirmationActivity/KeyboardEntryActivity callbacks.
  pendingHighlight = highlight::Highlight{};
  pendingHighlight.spine = static_cast<uint16_t>(currentSpineIndex);
  pendingHighlight.start = a;
  pendingHighlight.end = b;
  pendingHighlight.layout = currentLayout();
  pendingHighlight.text = extractText(a, b);

  exitHighlightMode();          // leave selection mode; return to the reading view
  promptForHighlightComment();  // ask whether to attach a comment, then store
}

void EpubReaderActivity::promptForHighlightComment() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_ADD_COMMENT_PROMPT), ""),
      [this](const ActivityResult& confirmResult) {
        if (!confirmResult.isCancelled) {
          // User wants a comment — open the keyboard.
          startActivityForResult(
              std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ADD_COMMENT), "", 0, false),
              [this](const ActivityResult& kbResult) {
                if (!kbResult.isCancelled) {
                  pendingHighlight.note = std::get<KeyboardResult>(kbResult.data).text;
                }
                storePendingHighlight();
              });
        } else {
          storePendingHighlight();  // no comment
        }
      });
}

void EpubReaderActivity::storePendingHighlight() {
  highlights.push_back(std::move(pendingHighlight));
  pendingHighlight = highlight::Highlight{};
  saveHighlights();
  LOG_DBG("ERS", "Stored highlight (%u highlights total)", static_cast<uint32_t>(highlights.size()));
  requestUpdate();
}

highlight::LayoutParams EpubReaderActivity::currentLayout() const {
  highlight::LayoutParams l;
  l.fontId = SETTINGS.getReaderFontId();
  l.lineCompressionX100 = static_cast<int>(SETTINGS.getReaderLineCompression() * 100.0f + 0.5f);
  l.viewportWidth = hlViewportW;
  l.viewportHeight = hlViewportH;
  l.paragraphAlignment = static_cast<uint8_t>(SETTINGS.paragraphAlignment);
  l.characterWrap = static_cast<uint8_t>(SETTINGS.characterWrap);
  l.hyphenationEnabled = SETTINGS.hyphenationEnabled ? 1 : 0;
  l.embeddedStyle = SETTINGS.embeddedStyle ? 1 : 0;
  l.imageRendering = static_cast<uint8_t>(SETTINGS.imageRendering);
  l.extraParagraphSpacing = SETTINGS.extraParagraphSpacing ? 1 : 0;
  l.paragraphIndent = SETTINGS.paragraphIndent ? 1 : 0;
  return l;
}

bool EpubReaderActivity::hasHighlightsForCurrentView() const {
  if (highlights.empty() || !section) {
    return false;
  }
  const highlight::LayoutParams layout = currentLayout();
  const int p = section->currentPage;
  for (const auto& h : highlights) {
    if (h.spine != currentSpineIndex) continue;
    if (!(h.layout == layout)) continue;
    if (p >= h.start.page && p <= h.end.page) return true;
  }
  return false;
}

void EpubReaderActivity::rebuildPageGeom(const Page& page) {
  pageGeom.clear();
  const int fontId = SETTINGS.getReaderFontId();
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& pl = static_cast<const PageLine&>(*el);
    const auto& blk = pl.getBlock();
    if (!blk) continue;

    HlLine hl;
    hl.yTop = static_cast<int16_t>(hlMarginTop + pl.yPos);
    const int xBase = hlMarginLeft + pl.xPos;

    const auto& words = blk->getWords();
    const auto& xs = blk->getWordXpos();
    const auto& styles = blk->getWordStyles();

    int logical = 0;
    for (size_t w = 0; w < words.size(); w++) {
      if (w > 0) {
        hl.text += ' ';  // inter-word space is one logical character
        logical += 1;
      }
      HlWord hw;
      hw.x = static_cast<int16_t>(xBase + (w < xs.size() ? xs[w] : 0));
      hw.style = static_cast<uint8_t>(w < styles.size() ? styles[w] : EpdFontFamily::REGULAR);
      hw.logicalStart = logical;
      hw.text = words[w];
      hw.bx.push_back(hw.x);  // boundary before the first character

      const unsigned char* p = reinterpret_cast<const unsigned char*>(words[w].c_str());
      int cc = 0;
      while (*p) {
        if (utf8NextCodepoint(&p) == 0) break;
        cc++;
        const int prefixBytes = static_cast<int>(reinterpret_cast<const char*>(p) - words[w].c_str());
        const std::string prefix(words[w].c_str(), static_cast<size_t>(prefixBytes));
        hw.bx.push_back(static_cast<int16_t>(
            hw.x + renderer.getTextAdvanceX(fontId, prefix.c_str(), static_cast<EpdFontStyle>(hw.style))));
      }
      hw.chars = cc;
      logical += cc;
      hl.text += words[w];
      hl.words.push_back(std::move(hw));
    }
    hl.charCount = logical;
    pageGeom.push_back(std::move(hl));
  }
}

void EpubReaderActivity::syncHighlightCursorToPage() {
  hlCursor.page = section ? static_cast<uint16_t>(section->currentPage) : 0;
  if (hlPending == HlPending::PageStart) {
    hlCursor.line = 0;
    hlCursor.ch = 0;
    hlPending = HlPending::None;
  } else if (hlPending == HlPending::PageEnd) {
    if (pageGeom.empty()) {
      hlCursor.line = 0;
      hlCursor.ch = 0;
    } else {
      hlCursor.line = static_cast<uint16_t>(pageGeom.size() - 1);
      hlCursor.ch = static_cast<uint16_t>(pageGeom.back().charCount);
    }
    hlPending = HlPending::None;
  }
  // Always clamp to current geometry (page may have fewer lines/chars).
  if (pageGeom.empty()) {
    hlCursor.line = 0;
    hlCursor.ch = 0;
  } else {
    if (hlCursor.line >= pageGeom.size()) hlCursor.line = static_cast<uint16_t>(pageGeom.size() - 1);
    const int cc = pageGeom[hlCursor.line].charCount;
    if (hlCursor.ch > cc) hlCursor.ch = static_cast<uint16_t>(cc);
  }
}

int16_t EpubReaderActivity::boundaryXAt(const HlLine& line, int ch) const {
  if (ch < 0) ch = 0;
  if (ch > line.charCount) ch = line.charCount;
  for (const auto& w : line.words) {
    if (ch >= w.logicalStart && ch <= w.logicalStart + w.chars) {
      const int idx = ch - w.logicalStart;
      if (idx >= 0 && idx < static_cast<int>(w.bx.size())) return w.bx[idx];
    }
  }
  if (!line.words.empty() && !line.words.back().bx.empty()) return line.words.back().bx.back();
  return static_cast<int16_t>(hlMarginLeft);
}

void EpubReaderActivity::drawHighlightSpan(const HlLine& line, int chFrom, int chTo) {
  if (chFrom < 0) chFrom = 0;
  if (chTo > line.charCount) chTo = line.charCount;
  if (chTo <= chFrom) return;

  const int fontId = SETTINGS.getReaderFontId();
  const int x0 = boundaryXAt(line, chFrom);
  const int x1 = boundaryXAt(line, chTo);
  const int lh = renderer.getLineHeight(fontId);
  if (x1 > x0) {
    renderer.fillRect(x0, line.yTop, x1 - x0, lh, true);  // black fill
  }
  // Redraw the covered glyphs of each word in white so highlighted text stays legible.
  for (const auto& w : line.words) {
    const int a = std::max(chFrom, w.logicalStart);
    const int b = std::min(chTo, w.logicalStart + w.chars);
    if (b <= a) continue;
    const int ca = a - w.logicalStart;
    const int cb = b - w.logicalStart;
    if (ca < 0 || ca >= static_cast<int>(w.bx.size())) continue;
    const std::string sub = utf8SubstrCp(w.text, ca, cb);
    renderer.drawText(fontId, w.bx[ca], line.yTop, sub.c_str(), false, static_cast<EpdFontStyle>(w.style));
  }
}

void EpubReaderActivity::drawHighlightOverlay() {
  const int fontId = SETTINGS.getReaderFontId();
  const int p = section ? section->currentPage : 0;
  const highlight::LayoutParams layout = currentLayout();

  // Committed highlights that intersect this page.
  for (const auto& h : highlights) {
    if (h.spine != currentSpineIndex) continue;
    if (!(h.layout == layout)) continue;  // stale layout -> don't draw a misaligned overlay
    if (p < h.start.page || p > h.end.page) continue;
    const int lineFrom = (p == h.start.page) ? h.start.line : 0;
    const int lineTo = (p == h.end.page) ? h.end.line : static_cast<int>(pageGeom.size()) - 1;
    for (int li = lineFrom; li <= lineTo && li < static_cast<int>(pageGeom.size()); li++) {
      if (li < 0) continue;
      const int cf = (p == h.start.page && li == h.start.line) ? h.start.ch : 0;
      const int ct = (p == h.end.page && li == h.end.line) ? h.end.ch : pageGeom[li].charCount;
      drawHighlightSpan(pageGeom[li], cf, ct);
    }
  }

  if (!highlightMode) {
    return;
  }

  // Live, in-progress selection between the anchor and the moving cursor,
  // clipped to the portion on the current page.
  if (hlSelecting) {
    highlight::Pos a = hlAnchor;
    highlight::Pos b = hlCursor;
    if (highlight::posLess(b, a)) std::swap(a, b);
    if (!(p < a.page || p > b.page)) {
      const int lineFrom = (p == a.page) ? a.line : 0;
      const int lineTo = (p == b.page) ? b.line : static_cast<int>(pageGeom.size()) - 1;
      for (int li = lineFrom; li <= lineTo && li < static_cast<int>(pageGeom.size()); li++) {
        if (li < 0) continue;
        const int cf = (p == a.page && li == a.line) ? a.ch : 0;
        const int ct = (p == b.page && li == b.line) ? b.ch : pageGeom[li].charCount;
        drawHighlightSpan(pageGeom[li], cf, ct);
      }
    }
  }

  // Caret at the moving cursor (a thin vertical bar).
  if (hlCursor.page == p && hlCursor.line < pageGeom.size()) {
    const HlLine& line = pageGeom[hlCursor.line];
    const int x = boundaryXAt(line, hlCursor.ch);
    const int asc = renderer.getFontAscenderSize(fontId);
    renderer.drawLine(x, line.yTop, x, line.yTop + asc, 2, true);
  }
}

std::vector<std::string> EpubReaderActivity::pageLineTexts(int pageIndex) {
  std::vector<std::string> out;
  if (!section || pageIndex < 0 || pageIndex >= section->pageCount) {
    return out;
  }
  const int saved = section->currentPage;
  section->currentPage = pageIndex;
  auto pg = section->loadPageFromSectionFile();
  section->currentPage = saved;
  if (!pg) {
    return out;
  }
  for (const auto& el : pg->elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& pl = static_cast<const PageLine&>(*el);
    const auto& blk = pl.getBlock();
    if (!blk) continue;
    std::string t;
    const auto& words = blk->getWords();
    for (size_t i = 0; i < words.size(); i++) {
      if (i > 0) t += ' ';
      t += words[i];
    }
    out.push_back(std::move(t));
  }
  return out;
}

std::string EpubReaderActivity::extractText(const highlight::Pos& a, const highlight::Pos& b) {
  std::string result;
  if (!section) {
    return result;
  }
  for (int pg = a.page; pg <= b.page; pg++) {
    std::vector<std::string> texts;
    if (pg == section->currentPage && !pageGeom.empty()) {
      texts.reserve(pageGeom.size());
      for (const auto& l : pageGeom) texts.push_back(l.text);
    } else {
      texts = pageLineTexts(pg);
    }
    const int lineFrom = (pg == a.page) ? a.line : 0;
    const int lineTo = (pg == b.page) ? b.line : static_cast<int>(texts.size()) - 1;
    for (int li = lineFrom; li <= lineTo && li < static_cast<int>(texts.size()); li++) {
      if (li < 0) continue;
      const std::string& lt = texts[li];
      const int total = utf8Len(lt);
      int cf = (pg == a.page && li == a.line) ? a.ch : 0;
      int ct = (pg == b.page && li == b.line) ? b.ch : total;
      if (cf < 0) cf = 0;
      if (ct > total) ct = total;
      if (ct > cf) {
        if (!result.empty()) result += ' ';
        result += utf8SubstrCp(lt, cf, ct);
      }
    }
  }
  return result;
}

void EpubReaderActivity::openHighlightsList() {
  startActivityForResult(
      std::make_unique<HighlightListActivity>(renderer, mappedInput, highlights, [this]() { saveHighlights(); }),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& jump = std::get<SyncResult>(result.data);
          if (currentSpineIndex != jump.spineIndex || (section && section->currentPage != jump.page)) {
            RenderLock lock(*this);
            currentSpineIndex = jump.spineIndex;
            nextPageNumber = jump.page;
            section.reset();
          }
        }
        requestUpdate();
      });
}
