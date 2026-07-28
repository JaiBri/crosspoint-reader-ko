#include "SidecarListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "MappedInputManager.h"
#include "SidecarDetailActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int LINE_HEIGHT = 30;
}  // namespace

int SidecarListActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - LINE_HEIGHT;
  return std::max(1, availableHeight / LINE_HEIGHT);
}

void SidecarListActivity::onEnter() {
  Activity::onEnter();
  if (selectorIndex >= static_cast<int>(rows.size())) {
    selectorIndex = std::max(0, static_cast<int>(rows.size()) - 1);
  }
  requestUpdate();
}

void SidecarListActivity::onExit() { Activity::onExit(); }

void SidecarListActivity::openDetail() {
  if (selectorIndex < 0 || selectorIndex >= static_cast<int>(rows.size())) return;
  const int idx = selectorIndex;
  const Row& r = rows[idx];
  startActivityForResult(
      std::make_unique<SidecarDetailActivity>(
          renderer, mappedInput, title, r.body, r.note, r.spine, r.page,
          [this, idx](const std::string& n) {
            if (idx < static_cast<int>(rows.size())) rows[idx].note = n;
            if (onEdit) onEdit(idx, n);
          },
          [this, idx]() {
            if (onDelete) onDelete(idx);
            if (idx < static_cast<int>(rows.size())) rows.erase(rows.begin() + idx);
          }),
      [this](const ActivityResult& res) {
        if (!res.isCancelled && std::holds_alternative<ProgressChangeResult>(res.data)) {
          const auto sync = std::get<ProgressChangeResult>(res.data);
          setResult(ProgressChangeResult{sync.spineIndex, sync.page});  // bubble the jump up to the reader
          finish();
          return;
        }
        if (selectorIndex >= static_cast<int>(rows.size())) {
          selectorIndex = std::max(0, static_cast<int>(rows.size()) - 1);
        }
        requestUpdate();
      });
}

void SidecarListActivity::loop() {
  // Back -> cancel.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  const int total = static_cast<int>(rows.size());
  if (total == 0) {
    return;  // nothing to open or navigate
  }

  // Confirm opens the detail window for the selected entry.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openDetail();
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

void SidecarListActivity::render(RenderLock&&) {
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
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, title.c_str(), true, EpdFontFamily::BOLD);

  const int total = static_cast<int>(rows.size());
  if (total == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120 + contentY, emptyMessage.c_str());
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
    const std::string shown = renderer.truncatedText(UI_10_FONT_ID, rows[itemIndex].label.c_str(), contentWidth - 40);
    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, shown.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
