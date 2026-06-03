#include "HighlightListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long DELETE_HOLD_MS = 800;
constexpr int LINE_HEIGHT = 30;
}  // namespace

int HighlightListActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - LINE_HEIGHT;
  return std::max(1, availableHeight / LINE_HEIGHT);
}

void HighlightListActivity::onEnter() {
  Activity::onEnter();
  if (selectorIndex >= static_cast<int>(highlights.size())) {
    selectorIndex = std::max(0, static_cast<int>(highlights.size()) - 1);
  }
  requestUpdate();
}

void HighlightListActivity::onExit() { Activity::onExit(); }

void HighlightListActivity::loop() {
  const int total = static_cast<int>(highlights.size());

  // Cancel.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (total == 0) {
    return;  // nothing to select or navigate
  }

  // Long-press Confirm deletes the selected highlight (and persists the change).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= DELETE_HOLD_MS &&
      !deleteArmed) {
    deleteArmed = true;
    if (selectorIndex >= 0 && selectorIndex < static_cast<int>(highlights.size())) {
      highlights.erase(highlights.begin() + selectorIndex);
      if (onChanged) onChanged();
      if (selectorIndex >= static_cast<int>(highlights.size())) {
        selectorIndex = std::max(0, static_cast<int>(highlights.size()) - 1);
      }
    }
    requestUpdate();
    return;
  }

  // Short Confirm jumps to the selected highlight's start page.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (deleteArmed) {
      deleteArmed = false;  // release that ended a long-press delete — swallow it
      return;
    }
    if (selectorIndex >= 0 && selectorIndex < static_cast<int>(highlights.size())) {
      const auto& h = highlights[selectorIndex];
      setResult(SyncResult{static_cast<int>(h.spine), static_cast<int>(h.start.page)});
      finish();
    }
    return;
  }

  const int pageItems = getPageItems();
  buttonNavigator.onNextRelease([this, total] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, total);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, total] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, total);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, total, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, total, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, total, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, total, pageItems);
    requestUpdate();
  });
}

void HighlightListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_HIGHLIGHTS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_HIGHLIGHTS), true, EpdFontFamily::BOLD);

  const int total = static_cast<int>(highlights.size());
  if (total == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120 + contentY, tr(STR_NO_HIGHLIGHTS));
    const auto labels0 = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels0.btn1, labels0.btn2, labels0.btn3, labels0.btn4);
    renderer.displayBuffer();
    return;
  }

  const int pageItems = getPageItems();
  const int pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(contentX, 60 + contentY + (selectorIndex % pageItems) * LINE_HEIGHT - 2, contentWidth - 1,
                    LINE_HEIGHT);

  for (int i = 0; i < pageItems; i++) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= total) break;
    const int displayY = 60 + contentY + i * LINE_HEIGHT;
    const bool isSelected = (itemIndex == selectorIndex);
    const auto& h = highlights[itemIndex];

    const std::string marker = h.note.empty() ? "" : "* ";  // '*' flags an attached comment
    const std::string label = marker + "Ch " + std::to_string(h.spine + 1) + " p" +
                              std::to_string(h.start.page + 1) + ": " +
                              (h.text.empty() ? std::string("(empty)") : h.text);
    const std::string shown = renderer.truncatedText(UI_10_FONT_ID, label.c_str(), contentWidth - 40);
    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, shown.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
