#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SidecarListActivity.h"
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
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // The highlight/bookmark sidecar lives NEXT TO the book and is keyed by its
  // path, so it must follow the move or every highlight and bookmark the reader
  // made in this book is silently orphaned.
  const std::string oldSidecar = highlight::sidecarPath(srcPath);
  if (Storage.exists(oldSidecar.c_str())) {
    const std::string newSidecar = highlight::sidecarPath(dstPath);
    if (!Storage.rename(oldSidecar.c_str(), newSidecar.c_str())) {
      LOG_ERR("ERS", "Failed to move highlight sidecar %s -> %s (highlights orphaned)", oldSidecar.c_str(),
              newSidecar.c_str());
    }
  }

  // Reading-speed history is keyed by book path inside one global file, so
  // re-key it too; otherwise the book's samples are stranded under a dead path.
  readingspeed::Library lib;
  if (Storage.exists(readingspeed::kLibraryPath)) {
    const String content = Storage.readFile(readingspeed::kLibraryPath);
    if (content.length() > 0) {
      lib = readingspeed::parse(std::string(content.c_str()));
      if (readingspeed::rekeyBook(lib, srcPath, dstPath)) {
        const std::string md = readingspeed::serialize(lib);
        if (!Storage.writeFile(readingspeed::kLibraryPath, String(md.c_str()))) {
          LOG_ERR("ERS", "Failed to re-key reading speed for %s (non-fatal)", dstPath.c_str());
        }
      }
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

// Focus Reading is an English-only feature: the per-character spacing it
// applies is unnecessary (and cache-key-distorting) for CJK scripts where
// character boundaries are already visually distinct. Gate it so the on-SD
// section cache key stays consistent with what is actually rendered — a
// language change (e.g. EN→KO) correctly invalidates cached sections.
static inline bool effectiveFocusReading() {
  return I18N.getLanguage() == Language::EN && SETTINGS.focusReadingEnabled != 0;
}

// True when a section cache was paginated under exactly the layout a sidecar
// entry recorded. lineCompression is compared through the same x100 rounding
// EpubReaderActivity::currentLayout() applies, so the two representations agree
// bit-for-bit; the bools are normalised to 0/1 to match the stored uint8_t.
bool sameLayout(const Section::CachedPagination& cached, const highlight::LayoutParams& stored) {
  return cached.fontId == stored.fontId &&
         static_cast<int>(cached.lineCompression * 100.0f + 0.5f) == stored.lineCompressionX100 &&
         cached.viewportWidth == stored.viewportWidth && cached.viewportHeight == stored.viewportHeight &&
         cached.paragraphAlignment == stored.paragraphAlignment &&
         static_cast<uint8_t>(cached.characterWrap ? 1 : 0) == stored.characterWrap &&
         static_cast<uint8_t>(cached.hyphenationEnabled ? 1 : 0) == stored.hyphenationEnabled &&
         static_cast<uint8_t>(cached.embeddedStyle ? 1 : 0) == stored.embeddedStyle &&
         cached.imageRendering == stored.imageRendering &&
         static_cast<uint8_t>(cached.extraParagraphSpacing ? 1 : 0) == stored.extraParagraphSpacing &&
         static_cast<uint8_t>(cached.paragraphIndent ? 1 : 0) == stored.paragraphIndent &&
         static_cast<uint8_t>(cached.focusReadingEnabled ? 1 : 0) == stored.focusReading;
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

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
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

  // Load the global reading-speed log and prime this book's median estimate.
  loadReadingSpeed();

  // Load existing highlights for this book (sidecar Markdown next to the file).
  loadSidecarIfNeeded();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Persist accumulated session time before tearing down the book.
  readingTimer.stop();

  // Final flush of any page durations recorded since the last periodic save.
  saveReadingSpeed();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
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

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  // Highlight sub-mode takes over all input until accept (Confirm) or cancel (Back).
  // Placed AFTER the end-of-book bookkeeping above: that block must keep running
  // every frame, and this branch returns early.
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

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
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
                  menuParagraphAlignment != SETTINGS.paragraphAlignment ||
                  menuEmbeddedStyle != SETTINGS.embeddedStyle ||
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
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
    if (!showBookmarkMessage) {
      addBookmark();
      showBookmarkMessage = true;
      ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
      bookmarkMessageTime = millis();
      requestUpdate();
    }
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

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
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
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
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
  const float spineFraction =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  jumpToSpineFraction(targetSpineIndex, spineFraction);
}

// Defer repositioning until render() has loaded the target section, at which
// point section->pageCount is authoritative for the CURRENT layout (see the
// pendingPercentJump handling in render()). Resolving to a page any earlier
// would have to trust a cached page count that may have been produced under a
// different layout.
void EpubReaderActivity::jumpToSpineFraction(const int spineIndex, const float spineFraction) {
  float f = spineFraction;
  if (!(f >= 0.0f)) {
    f = 0.0f;  // also catches NaN
  } else if (f > 1.0f) {
    f = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    pendingSpineProgress = f;
    currentSpineIndex = spineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

// Decode a bookmark's book-progress anchor into (spine, intra-spine fraction).
//
// The stored spine index is itself layout-independent (spine sizes come from
// uncompressed ZIP bytes), so it is trusted directly rather than re-derived by
// scanning cumulative sizes for the percentage. That is both cheaper — two
// lookups instead of up to spineCount — and makes it impossible for float
// rounding to land the jump in a neighbouring chapter.
bool EpubReaderActivity::bookPctToSpineTarget(const highlight::Bookmark& bm, int& spineOut, float& fracOut) const {
  if (!epub || !highlight::pctValid(bm.pctX10000)) {
    return false;
  }
  const int spineCount = epub->getSpineItemsCount();
  const int spine = static_cast<int>(bm.spine);
  if (spine < 0 || spine >= spineCount) {
    return false;
  }

  const size_t bookSize = epub->getBookSize();
  const size_t prevCumulative = (spine > 0) ? epub->getCumulativeSpineItemSize(spine - 1) : 0;
  const size_t cumulative = epub->getCumulativeSpineItemSize(spine);
  if (bookSize == 0 || cumulative <= prevCumulative) {
    return false;
  }

  const double fraction = static_cast<double>(bm.pctX10000) / static_cast<double>(highlight::PCT_SCALE);
  auto target = static_cast<size_t>(fraction * static_cast<double>(bookSize) + 0.5);
  // Clamp into the bookmark's own chapter: the anchor is quantised, so a
  // bookmark at a chapter edge can round just outside it.
  if (target < prevCumulative) target = prevCumulative;
  if (target > cumulative - 1) target = cumulative - 1;

  const size_t spineSize = cumulative - prevCumulative;
  spineOut = spine;
  fracOut = static_cast<float>(target - prevCumulative) / static_cast<float>(spineSize);
  return true;
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
        RenderLock lock(*this);
        currentSpineIndex = sync.spineIndex;
        nextPageNumber = sync.page;
        section.reset();
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
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
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
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
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
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
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
        std::optional<uint16_t> paragraphIndex;
        if (section && currentPage >= 0 && currentPage < section->pageCount) {
          const uint16_t paragraphPage =
              currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
          if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
            paragraphIndex = *pIdx;
          }
        }

        // Pre-compute local KO position and chapter name while Epub is still in RAM.
        CrossPointPosition localPos = getCurrentPosition();
        SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
        const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
        std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
        const std::string savedEpubPath = epub->getPath();

        // Persist current position so the reader resumes at the right page on return.
        // goToReader() depends on this file, so abort the sync if the write fails.
        if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
          LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
          pendingSyncSaveError = true;
          requestUpdate();
          return;
        }

        // Release Epub and Section to free ~65KB RAM for the TLS handshake.
        LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
        {
          RenderLock lock(*this);
          if (section) {
            nextPageNumber = section->currentPage;
          }
          section.reset();
          epub.reset();
        }
        LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

        activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
            renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
            std::move(localChapterName), paragraphIndex));
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
      // There is nothing to select on the End-of-Book screen: render() returns
      // before renderContents(), so pageGeom stays empty and every button except
      // Back would be dead with no hints drawn.
      if (section && section->pageCount > 0) {
        enterHighlightMode();
      } else {
        requestUpdate();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::HIGHLIGHTS: {
      openHighlightsList();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK: {
      addBookmarkForCurrentPage();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_BOOKMARKS: {
      openBookmarksList();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS:
      // Reader Options is launched as a sub-activity of the menu itself,
      // never dispatched here. Layout/orientation changes are reconciled
      // when the menu finally exits (see the menu's result handler in loop()).
      break;
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS:
      // Not offered in the menu (see EpubReaderMenuActivity::buildMenuItems):
      // upstream's bookmark list reads a separate JSON store that never sees the
      // entries in our Markdown sidecar. VIEW_BOOKMARKS below is the one UI.
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
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
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
  // Reading-speed: a forward turn ends a page of sequential reading. Record the
  // active-reading seconds spent on the page being left (menu/idle time is
  // already excluded by readingTimer). Skipped during auto page-turn (its fixed
  // cadence isn't reading speed) and when the snapshot doesn't match the page
  // currently on screen (e.g. right after a jump, before its render re-anchors).
  if (isForwardTurn && !automaticPageTurnActive && section && currentSpineIndex == timedSpine &&
      section->currentPage == timedPage) {
    const uint32_t nowSec = readingTimer.totalSeconds();
    if (nowSec >= pageStartActiveSec) recordPageDuration(nowSec - pageStartActiveSec);
  }

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
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

void EpubReaderActivity::loadReadingSpeed() {
  readingLibrary = readingspeed::parse(std::string(Storage.readFile(readingspeed::kLibraryPath).c_str()));
  refreshMedianCache();
}

void EpubReaderActivity::refreshMedianCache() {
  medianSecPerPageCache = 0;
  medianSampleCount = 0;
  if (!epub) return;
  if (auto* b = readingspeed::find(readingLibrary, epub->getPath())) {
    medianSecPerPageCache = readingspeed::medianSecPerPage(b->durations, readingspeed::kMinRealisticSec,
                                                           readingspeed::kMaxRealisticSec, medianSampleCount);
  }
}

void EpubReaderActivity::recordPageDuration(uint32_t seconds) {
  if (!epub) return;
  readingspeed::addDuration(readingLibrary, epub->getPath(), epub->getTitle(), seconds,
                            readingspeed::kMaxDurationsPerBook);
  refreshMedianCache();
  // Buffer writes: flush every few pages instead of on every turn (SD wear).
  if (++recordedSinceFlush >= 8) {
    saveReadingSpeed();
    recordedSinceFlush = 0;
  }
}

void EpubReaderActivity::saveReadingSpeed() {
  Storage.mkdir("/.crosspoint");
  Storage.writeFile(readingspeed::kLibraryPath, String(readingspeed::serialize(readingLibrary).c_str()));
  recordedSinceFlush = 0;
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

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

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
    showPendingSyncSaveError();
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
                                  SETTINGS.embeddedStyle, SETTINGS.imageRendering, effectiveFocusReading())) {
      LOG_DBG("ERS", "Cache not found, building...");

      GUI.drawPopup(renderer, tr(STR_INDEXING));

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphIndent,
                                      SETTINGS.paragraphAlignment, SETTINGS.characterWrap, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, effectiveFocusReading(), popupFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        showPendingSyncSaveError();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
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
      // Apply the pending percent jump now that we know the new section's page
      // count. Shares highlight::pageForFraction with the bookmark anchor
      // encoder, so the page->fraction->page identity the unit tests prove also
      // holds on device (same clamping, same truncation as before).
      section->currentPage = highlight::pageForFraction(pendingSpineProgress, section->pageCount);
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
    showPendingSyncSaveError();
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
      showPendingSyncSaveError();
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

  // Reading-speed timing anchor: when a *different* page becomes visible (via a
  // turn, jump, chapter select, sync, …), (re)start the per-page timer here so
  // the next forward pageTurn() measures genuine on-screen dwell of this page.
  if (currentSpineIndex != timedSpine || section->currentPage != timedPage) {
    timedSpine = currentSpineIndex;
    timedPage = section->currentPage;
    pageStartActiveSec = readingTimer.totalSeconds();
  }
  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, tr(STR_BOOKMARK_ADDED));
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
                                  SETTINGS.embeddedStyle, SETTINGS.imageRendering, effectiveFocusReading())) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(
          SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
          SETTINGS.paragraphIndent, SETTINGS.paragraphAlignment, SETTINGS.characterWrap, viewportWidth, viewportHeight,
          SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering, effectiveFocusReading())) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
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
    if (highlightMode) {
      // Show what the buttons do while selecting. Front buttons: Back, Start|End,
      // and previous/next word ("<" / ">"). Side buttons move by line ("^" / "v").
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), hlSelecting ? tr(STR_HIGHLIGHT_END) : tr(STR_HIGHLIGHT_START),
                                                "<", ">");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // Side hints are drawn rotated 90° CW, so ">"/"<" render as up/down
      // chevrons (matching the keyboard's Up/Down side-button convention).
      GUI.drawSideButtonHints(renderer, ">", "<");
    }
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
        // That re-render repaints the page's glyphs over the highlight overlay's
        // white knockout text, which would leave every highlighted span as a
        // solid black bar. Re-composite the overlay before displaying. pageGeom
        // is still valid here, so no rebuild is needed.
        if (highlightMode || hasHighlightsForCurrentView()) {
          drawHighlightOverlay();
        }
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      } else {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (SETTINGS.textAntiAliasing && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (SETTINGS.textAntiAliasing) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      // Skip storeBwBuffer entirely when heap is stressed — chunked 8KB allocs
      // compete with whatever paint cycle still needs to finish (ko.17 fix).
      if (heapStressed) {
        LOG_DBG("ERS", "Heap stressed — skipping fallback grayscale pass");
      } else {
        renderer.storeBwBuffer();
        const auto tBwStore = millis();

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
        renderer.restoreBwBuffer();
        const auto tBwRestore = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
                "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
                tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
      }  // !heapStressed
    } else {
      // No anti-aliasing: BW frame already displayed above, no grayscale to
      // render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
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

  // Estimate "time left in book" from the median realistic page time. There is
  // no whole-book page count, so derive pages-remaining from the byte fraction
  // (bookProgress) and the current chapter's bytes-per-page; the density is
  // recomputed every render, so the estimate self-corrects across chapters.
  std::string timeLeft;
  if (SETTINGS.showTimeRemaining && medianSecPerPageCache > 0 && medianSampleCount >= readingspeed::kMinSamples) {
    const float p = bookProgress / 100.0f;  // byte fraction of the book read
    const size_t bookSize = epub->getBookSize();
    if (p > 0.0f && p < 1.0f && bookSize > 0 && pageCount > 0) {
      const size_t prev = currentSpineIndex >= 1 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
      const float chapterBytes = static_cast<float>(epub->getCumulativeSpineItemSize(currentSpineIndex) - prev);
      const float bytesPerPage = chapterBytes / pageCount;
      if (bytesPerPage > 0.0f) {
        const float pagesLeft = (static_cast<float>(bookSize) * (1.0f - p)) / bytesPerPage;
        if (pagesLeft > 0.0f) {
          const uint32_t secsLeft = static_cast<uint32_t>(pagesLeft * static_cast<float>(medianSecPerPageCache));
          char buf[12];
          ReadingStats::format(secsLeft, buf, sizeof(buf));
          timeLeft = std::string("~") + buf;  // e.g. "~2h 15m"
        }
      }
    }
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, timeLeft);
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

void EpubReaderActivity::addBookmark() {
  // Quick, promptless bookmark bound to Confirm-hold.
  //
  // Upstream's original implementation wrote a BookmarkEntry into a separate
  // JSON store under /.crosspoint/bookmarks/. We keep a single store — the
  // Markdown sidecar next to the book — so this now shares capturePendingBookmark()
  // with the menu-driven path. A comment can be added later from the list.
  //
  // (The original also declared a local `std::vector<BookmarkEntry> bookmarks`
  // that shadowed our member of the same name: it compiled cleanly and silently
  // operated on the wrong container.)
  if (!capturePendingBookmark()) {
    return;
  }
  storePendingBookmark();
  showBookmarkMessage = true;
  bookmarkMessageTime = millis();
  requestUpdate();
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
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

void EpubReaderActivity::loadSidecarIfNeeded() {
  if (sidecarLoaded || !epub) {
    return;
  }
  sidecarLoaded = true;
  const std::string path = highlight::sidecarPath(epub->getPath());
  if (!Storage.exists(path.c_str())) {
    return;
  }
  const String content = Storage.readFile(path.c_str());
  if (content.length() > 0) {
    // Highlights and bookmarks share one .md file; parse each from it.
    const std::string md(content.c_str());
    highlights = highlight::parse(md);
    bookmarks = highlight::parseBookmarks(md);
    std::sort(highlights.begin(), highlights.end(), highlight::highlightLess);
    std::sort(bookmarks.begin(), bookmarks.end(), highlight::bookmarkLess);
    LOG_DBG("ERS", "Loaded %u highlights, %u bookmarks", static_cast<uint32_t>(highlights.size()),
            static_cast<uint32_t>(bookmarks.size()));
    migrateBookmarkPercentages();
  }
}

// Give pre-`pc=` bookmarks a layout-independent anchor, but ONLY where it can be
// derived exactly — that is, where the chapter's section cache was paginated
// under the very layout the bookmark recorded, so its stored page index is
// provably meaningful.
//
// Placement matters: this runs from onEnter() BEFORE the first requestUpdate(),
// so nothing has repaginated yet this session and each bookmarked spine's cache
// still holds the layout it was last read under. Move this after any render and
// the exact-match rate collapses.
//
// Bookmarks that cannot be converted exactly are left completely alone. An
// approximate conversion would be actively harmful: when the cache has already
// been rebuilt at today's layout, encoding page P and decoding it again returns
// P, so it would "convert" the bookmark to an anchor meaning today's stale page
// — freezing an error that otherwise self-heals the next time the chapter is
// paginated under the original layout.
void EpubReaderActivity::migrateBookmarkPercentages() {
  if (!epub) {
    return;
  }
  const bool anyMissing = std::any_of(bookmarks.begin(), bookmarks.end(), [](const highlight::Bookmark& b) {
    return !highlight::pctValid(b.pctX10000);
  });
  if (!anyMissing) {
    return;
  }

  // Bookmarks are sorted by spine, so a single-entry memo collapses the common
  // case of several bookmarks in one chapter to one header read.
  int cachedSpine = -1;
  bool cachedOk = false;
  Section::CachedPagination cached;

  int converted = 0, skipped = 0;
  for (auto& bm : bookmarks) {
    if (highlight::pctValid(bm.pctX10000)) {
      continue;
    }
    if (static_cast<int>(bm.spine) != cachedSpine) {
      cachedSpine = static_cast<int>(bm.spine);
      cachedOk = Section::readCachedPagination(Section::cacheFilePath(epub->getCachePath(), cachedSpine), cached);
    }
    if (!cachedOk || cached.pageCount == 0 || bm.page >= cached.pageCount || !sameLayout(cached, bm.layout)) {
      skipped++;
      continue;
    }
    const float spineFraction = highlight::pageCentreFraction(bm.page, cached.pageCount);
    bm.pctX10000 = highlight::encodePct(epub->calculateProgress(cachedSpine, spineFraction));
    converted++;
  }

  LOG_DBG("ERS", "Bookmark migration: %d converted, %d left on the page fallback", converted, skipped);
  if (converted > 0) {
    std::sort(bookmarks.begin(), bookmarks.end(), highlight::bookmarkLess);
    saveSidecar();
  }
}

void EpubReaderActivity::saveSidecar() {
  if (!epub) {
    return;
  }
  // Both types live in one file, so always serialize BOTH — writing only one
  // would erase the other.
  std::sort(highlights.begin(), highlights.end(), highlight::highlightLess);
  std::sort(bookmarks.begin(), bookmarks.end(), highlight::bookmarkLess);
  const std::string md = highlight::serialize(epub->getTitle(), highlights, bookmarks);
  const std::string path = highlight::sidecarPath(epub->getPath());
  if (!Storage.writeFile(path.c_str(), String(md.c_str()))) {
    LOG_ERR("ERS", "Failed to write sidecar: %s", path.c_str());
  }
}

void EpubReaderActivity::enterHighlightMode() {
  // Auto page turn must stop: highlight mode's early return in loop() means its
  // cancel keys never run, and a page turning under an in-progress selection
  // would invalidate hlAnchor/hlCursor.
  automaticPageTurnActive = false;
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
  // After a page turn the cursor and page geometry are resynced by the render
  // task (syncHighlightCursorToPage clears hlPending). Until that happens — or
  // while a render is in flight — ignore movement/commit so we never act on
  // geometry that doesn't match the cursor's page. Back (above) still cancels.
  if (hlPending != HlPending::None || RenderLock::peek()) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!hlSelecting) {
      // First press locks the start word. Capture both its start (hlAnchor) and
      // end (hlAnchorEnd) so a later backward extension can use the start word's
      // end as the far boundary of the range.
      hlAnchor = hlCursor;
      hlAnchorEnd = highlight::Pos{hlCursor.page, hlCursor.line, static_cast<uint16_t>(hlCursorWordEnd())};
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

  // 2D word selection: front Left/Right move by word, side Up/Down move by line
  // (to the word nearest the cursor's x). Every direction is always allowed —
  // moving before the locked start just makes the start word the far end of the
  // range (commitHighlight/drawHighlightOverlay branch on direction), so the
  // highlight can grow either way.
  // Use the same swap the page-turn path and the on-screen '<' / '>' hints use.
  // Reading Left/Right raw would step the word cursor opposite to the label the
  // user is looking at in INVERTED / LANDSCAPE_CCW.
  const bool swapFront = ReaderUtils::frontButtonsSwapped();
  const auto forwardButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const auto backwardButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  if (mappedInput.wasReleased(forwardButton)) {
    hlMoveForwardWord();
    return;
  }
  if (mappedInput.wasReleased(backwardButton)) {
    hlMoveBackwardWord();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    hlMoveLine(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    hlMoveLine(-1);
    return;
  }
}

int EpubReaderActivity::hlWordIndex(const HlLine& line, int ch) const {
  if (line.words.empty()) return -1;
  for (size_t i = 0; i < line.words.size(); i++) {
    const auto& w = line.words[i];
    if (ch >= w.logicalStart && ch < w.logicalStart + w.chars) return static_cast<int>(i);
  }
  // ch landed on a space/end boundary: snap to the last word that starts at or before ch.
  int best = 0;
  for (size_t i = 0; i < line.words.size(); i++) {
    if (line.words[i].logicalStart <= ch) best = static_cast<int>(i);
  }
  return best;
}

int EpubReaderActivity::hlCursorWordEnd() const {
  if (hlCursor.line >= pageGeom.size()) return hlCursor.ch;
  const HlLine& line = pageGeom[hlCursor.line];
  const int wi = hlWordIndex(line, hlCursor.ch);
  if (wi < 0) return hlCursor.ch;
  return line.words[wi].logicalStart + line.words[wi].chars;
}

void EpubReaderActivity::hlMoveForwardWord() {
  if (pageGeom.empty()) {
    highlightPageTurn(true);
    return;
  }
  const HlLine& line = pageGeom[hlCursor.line];
  const int wi = hlWordIndex(line, hlCursor.ch);
  if (wi >= 0 && wi + 1 < static_cast<int>(line.words.size())) {
    hlCursor.ch = static_cast<uint16_t>(line.words[wi + 1].logicalStart);
    requestUpdate();
    return;
  }
  for (int li = hlCursor.line + 1; li < static_cast<int>(pageGeom.size()); li++) {
    if (!pageGeom[li].words.empty()) {
      hlCursor.line = static_cast<uint16_t>(li);
      hlCursor.ch = static_cast<uint16_t>(pageGeom[li].words.front().logicalStart);
      requestUpdate();
      return;
    }
  }
  highlightPageTurn(true);  // past the last word on the page -> next page
}

void EpubReaderActivity::hlMoveBackwardWord() {
  if (pageGeom.empty()) {
    highlightPageTurn(false);
    return;
  }
  const HlLine& line = pageGeom[hlCursor.line];
  const int wi = hlWordIndex(line, hlCursor.ch);
  if (wi > 0) {
    hlCursor.ch = static_cast<uint16_t>(line.words[wi - 1].logicalStart);
    requestUpdate();
    return;
  }
  for (int li = static_cast<int>(hlCursor.line) - 1; li >= 0; li--) {
    if (!pageGeom[li].words.empty()) {
      hlCursor.line = static_cast<uint16_t>(li);
      hlCursor.ch = static_cast<uint16_t>(pageGeom[li].words.back().logicalStart);
      requestUpdate();
      return;
    }
  }
  highlightPageTurn(false);  // before the first word on the page -> previous page
}

void EpubReaderActivity::hlMoveLine(int dir) {
  if (pageGeom.empty()) {
    highlightPageTurn(dir > 0);
    return;
  }
  // Horizontal target = the x of the cursor's current word, so the cursor lands
  // roughly above/below where it is now.
  int targetX = 0;
  {
    const HlLine& cur = pageGeom[hlCursor.line];
    const int wi = hlWordIndex(cur, hlCursor.ch);
    if (wi >= 0) targetX = cur.words[wi].x;
  }
  for (int li = static_cast<int>(hlCursor.line) + dir; li >= 0 && li < static_cast<int>(pageGeom.size()); li += dir) {
    const HlLine& line = pageGeom[li];
    if (line.words.empty()) continue;
    // Pick the word whose first-glyph x is nearest the target x.
    int bestW = 0;
    int bestDx = line.words[0].x - targetX;
    if (bestDx < 0) bestDx = -bestDx;
    for (size_t i = 1; i < line.words.size(); i++) {
      int dx = line.words[i].x - targetX;
      if (dx < 0) dx = -dx;
      if (dx < bestDx) {
        bestDx = dx;
        bestW = static_cast<int>(i);
      }
    }
    hlCursor.line = static_cast<uint16_t>(li);
    hlCursor.ch = static_cast<uint16_t>(line.words[bestW].logicalStart);
    requestUpdate();
    return;
  }
  // No further non-empty line on this page in that direction: turn the page and
  // let syncHighlightCursorToPage place the cursor on the new page.
  highlightPageTurn(dir > 0);
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
  // Build a whole-word [a, b] range from the locked start word to the cursor
  // word in either direction. Forward: start word's start -> cursor word's end.
  // Backward (cursor before the start): cursor word's start -> start word's end,
  // so the start word becomes the far end — matching the live preview.
  highlight::Pos a, b;
  if (highlight::posLess(hlCursor, hlAnchor)) {
    a = hlCursor;
    b = hlAnchorEnd;
  } else {
    a = hlAnchor;
    b = highlight::Pos{hlCursor.page, hlCursor.line, static_cast<uint16_t>(hlCursorWordEnd())};
  }
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
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_ADD_COMMENT_PROMPT), "", tr(STR_YES),
                                             tr(STR_NO)),
      [this](const ActivityResult& confirmResult) {
        if (!confirmResult.isCancelled) {
          // User wants a comment — open the keyboard.
          startActivityForResult(
              std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ADD_COMMENT), "", 0, InputType::Text),
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
  saveSidecar();
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
  // Must use the same gated value the section cache is keyed on, not the raw
  // setting — see effectiveFocusReading().
  l.focusReading = effectiveFocusReading() ? 1 : 0;
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
      const auto& lw = pageGeom.back().words;
      hlCursor.ch = static_cast<uint16_t>(lw.empty() ? 0 : lw.back().logicalStart);
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

  if (hlSelecting) {
    // Live, in-progress selection from the locked start word to the word under
    // the moving cursor, clipped to the portion on the current page. Going
    // backward past the start makes the start word the far end (see
    // commitHighlight), so the preview matches what will be committed.
    highlight::Pos a, b;
    if (highlight::posLess(hlCursor, hlAnchor)) {
      a = hlCursor;
      b = hlAnchorEnd;
    } else {
      a = hlAnchor;
      b = highlight::Pos{hlCursor.page, hlCursor.line, static_cast<uint16_t>(hlCursorWordEnd())};
    }
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
  } else {
    // Not yet selecting: preview the whole word under the cursor (this also
    // shows where the cursor is, so no separate caret is needed).
    if (hlCursor.page == p && hlCursor.line < pageGeom.size()) {
      const HlLine& line = pageGeom[hlCursor.line];
      const int wi = hlWordIndex(line, hlCursor.ch);
      if (wi >= 0) {
        drawHighlightSpan(line, line.words[wi].logicalStart, line.words[wi].logicalStart + line.words[wi].chars);
      }
    }
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

// Shared handler for a SidecarListActivity result: a ProgressChangeResult means "jump
// here"; anything else (Back / after an in-detail edit or delete) just refreshes.
void EpubReaderActivity::openHighlightsList() {
  std::sort(highlights.begin(), highlights.end(), highlight::highlightLess);
  std::vector<SidecarListActivity::Row> rows;
  rows.reserve(highlights.size());
  for (const auto& h : highlights) {
    SidecarListActivity::Row r;
    const std::string marker = h.note.empty() ? "" : "* ";  // '*' flags an attached comment
    r.label = marker + "Ch " + std::to_string(h.spine + 1) + " p" + std::to_string(h.start.page + 1) + ": " +
              (h.text.empty() ? std::string("(empty)") : h.text);
    r.body = h.text;
    r.note = h.note;
    r.spine = static_cast<int>(h.spine);
    r.page = static_cast<int>(h.start.page);
    rows.push_back(std::move(r));
  }
  startActivityForResult(
      std::make_unique<SidecarListActivity>(
          renderer, mappedInput, std::move(rows), std::string(tr(STR_HIGHLIGHTS)), std::string(tr(STR_NO_HIGHLIGHTS)),
          [this](int idx, const std::string& note) {
            if (idx >= 0 && idx < static_cast<int>(highlights.size())) {
              highlights[idx].note = note;
              saveSidecar();
            }
          },
          [this](int idx) {
            if (idx >= 0 && idx < static_cast<int>(highlights.size())) {
              highlights.erase(highlights.begin() + idx);
              saveSidecar();
            }
          }),
      [this](const ActivityResult& result) {
        if (!result.isCancelled && std::holds_alternative<ProgressChangeResult>(result.data)) {
          const auto& jump = std::get<ProgressChangeResult>(result.data);
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

void EpubReaderActivity::openBookmarksList() {
  std::sort(bookmarks.begin(), bookmarks.end(), highlight::bookmarkLess);
  std::vector<SidecarListActivity::Row> rows;
  rows.reserve(bookmarks.size());
  for (const auto& b : bookmarks) {
    SidecarListActivity::Row r;
    const std::string marker = b.note.empty() ? "" : "* ";
    // Position goes BEFORE the snippet: the list draws one truncated line, so
    // anything after the snippet would be the first thing clipped. A migrated
    // bookmark shows its book percentage, one still on the page fallback keeps
    // the old "pN" form — so the list also reads as migration state.
    const std::string position = highlight::pctValid(b.pctX10000)
                                     ? std::to_string(highlight::pctToDisplayPercent(b.pctX10000)) + "%"
                                     : "p" + std::to_string(b.page + 1);
    r.label = marker + "Ch " + std::to_string(b.spine + 1) + " " + position +
              (b.text.empty() ? std::string("") : ": " + b.text);
    r.body = b.text;
    r.note = b.note;
    r.spine = static_cast<int>(b.spine);
    r.page = static_cast<int>(b.page);
    rows.push_back(std::move(r));
  }
  startActivityForResult(
      std::make_unique<SidecarListActivity>(
          renderer, mappedInput, std::move(rows), std::string(tr(STR_VIEW_BOOKMARKS)),
          std::string(tr(STR_NO_BOOKMARKS)),
          [this](int idx, const std::string& note) {
            if (idx >= 0 && idx < static_cast<int>(bookmarks.size())) {
              bookmarks[idx].note = note;
              saveSidecar();
            }
          },
          [this](int idx) {
            if (idx >= 0 && idx < static_cast<int>(bookmarks.size())) {
              bookmarks.erase(bookmarks.begin() + idx);
              saveSidecar();
            }
          }),
      [this](const ActivityResult& result) {
        if (!result.isCancelled && std::holds_alternative<ProgressChangeResult>(result.data)) {
          const auto& jump = std::get<ProgressChangeResult>(result.data);
          // Rows carry (spine, page), so find the bookmark they came from to
          // recover its anchor. Matching on the pair rather than a row index
          // keeps ProgressChangeResult — which is shared with the KOReader sync flow —
          // untouched.
          const auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const highlight::Bookmark& b) {
            return static_cast<int>(b.spine) == jump.spineIndex && static_cast<int>(b.page) == jump.page;
          });

          int targetSpine = 0;
          float targetFraction = 0.0f;
          if (it != bookmarks.end() && bookPctToSpineTarget(*it, targetSpine, targetFraction)) {
            // Layout-independent path: render() resolves the fraction to a page
            // once the section has been paginated under the current settings.
            jumpToSpineFraction(targetSpine, targetFraction);
          } else if (currentSpineIndex != jump.spineIndex || (section && section->currentPage != jump.page)) {
            // Pre-`pc=` bookmark: fall back to the stored page as before.
            RenderLock lock(*this);
            currentSpineIndex = jump.spineIndex;
            nextPageNumber = jump.page;
            section.reset();
          }
        }
        requestUpdate();
      });
}

bool EpubReaderActivity::capturePendingBookmark() {
  if (!section) {
    return false;
  }
  // Everything the bookmark needs must be captured HERE: promptForBookmarkComment()
  // runs asynchronously across two sub-activities via pendingBookmark, and
  // `section` may be gone by the time its callbacks fire.
  //
  // The capture is locked because render() runs on its own FreeRTOS task and
  // owns section->currentPage / pageCount / hlViewportW/H. pageLineTexts()
  // transiently mutates currentPage, and a torn read of pageCount == 0 would
  // make the progress division produce NaN — which converts to a garbage anchor
  // that then gets written permanently to the user's sidecar. The lock is
  // released before the prompt: holding it across startActivityForResult() is a
  // deadlock risk.
  {
    RenderLock lock(*this);
    pendingBookmark = highlight::Bookmark{};
    pendingBookmark.spine = static_cast<uint16_t>(currentSpineIndex);
    pendingBookmark.page = static_cast<uint16_t>(section->currentPage);
    pendingBookmark.layout = currentLayout();
    if (epub && section->pageCount > 0) {
      const float spineFraction = highlight::pageCentreFraction(section->currentPage, section->pageCount);
      pendingBookmark.pctX10000 = highlight::encodePct(epub->calculateProgress(currentSpineIndex, spineFraction));
    }
    // Human-readable label = the first non-empty line of the current page.
    const auto texts = pageLineTexts(section->currentPage);
    for (const auto& t : texts) {
      if (!t.empty()) {
        pendingBookmark.text = t;
        break;
      }
    }
  }
  return true;
}

void EpubReaderActivity::addBookmarkForCurrentPage() {
  if (!capturePendingBookmark()) {
    return;
  }
  promptForBookmarkComment();  // ask whether to attach a comment, then store
}

void EpubReaderActivity::promptForBookmarkComment() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_ADD_COMMENT_PROMPT), "", tr(STR_YES),
                                             tr(STR_NO)),
      [this](const ActivityResult& confirmResult) {
        if (!confirmResult.isCancelled) {
          startActivityForResult(
              std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ADD_COMMENT), "", 0, InputType::Text),
              [this](const ActivityResult& kbResult) {
                if (!kbResult.isCancelled) {
                  pendingBookmark.note = std::get<KeyboardResult>(kbResult.data).text;
                }
                storePendingBookmark();
              });
        } else {
          storePendingBookmark();  // no comment
        }
      });
}

void EpubReaderActivity::storePendingBookmark() {
  bookmarks.push_back(std::move(pendingBookmark));
  pendingBookmark = highlight::Bookmark{};
  saveSidecar();
  LOG_DBG("ERS", "Stored bookmark (%u bookmarks total)", static_cast<uint32_t>(bookmarks.size()));
  requestUpdate();
}
