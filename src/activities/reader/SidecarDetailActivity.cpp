#include "SidecarDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MARGIN = 20;
constexpr int TITLE_AREA = 50;  // vertical space reserved above the body for the title
}  // namespace

SidecarDetailActivity::Layout SidecarDetailActivity::computeLayout() const {
  Layout L;
  L.lineHeight = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  const int pageWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const bool isLandscape = isLandscapeCw || isLandscapeCcw;

  const int hintGutterWidth = isLandscape ? 30 : 0;
  L.contentX = isLandscapeCw ? hintGutterWidth : 0;
  L.contentWidth = pageWidth - hintGutterWidth;
  const int topGutter = isPortraitInverted ? 50 : 0;                      // inverted portrait: hints at top
  const int bottomHint = (!isLandscape && !isPortraitInverted) ? 44 : 0;  // normal portrait: hints at bottom

  L.bodyTop = topGutter + TITLE_AREA;
  const int noteReserve = note.empty() ? 0 : (2 * L.lineHeight + 8);
  const int bodyBottom = screenHeight - bottomHint - noteReserve - 6;
  L.linesPerPage = std::max(1, (bodyBottom - L.bodyTop) / L.lineHeight);
  return L;
}

void SidecarDetailActivity::onEnter() {
  Activity::onEnter();
  const Layout L = computeLayout();
  bodyLines = renderer.wrappedText(UI_10_FONT_ID, body.c_str(), L.contentWidth - 2 * MARGIN, 200);
  requestUpdate(true);
}

void SidecarDetailActivity::loop() {
  // Back -> return to the list (no jump).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }

  // Confirm -> Jump to the location.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(ProgressChangeResult{spine, page});
    finish();
    return;
  }

  // page-back -> Delete (confirm first, then onDelete + return to list).
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_CONFIRM), ""),
                           [this](const ActivityResult& r) {
                             if (!r.isCancelled) {
                               if (onDelete) onDelete();
                               ActivityResult res;
                               res.isCancelled = true;  // no jump; the list refreshes
                               setResult(std::move(res));
                               finish();
                             } else {
                               requestUpdate();
                             }
                           });
    return;
  }

  // page-fwd -> Edit comment (keyboard prefilled with the current note).
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ADD_COMMENT), note, 0, InputType::Text),
        [this](const ActivityResult& r) {
          if (!r.isCancelled) {
            note = std::get<KeyboardResult>(r.data).text;
            if (onEditNote) onEditNote(note);
          }
          requestUpdate();
        });
    return;
  }

  // Side buttons page the body when it is longer than one screen.
  const Layout L = computeLayout();
  const int total = static_cast<int>(bodyLines.size());
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (pageOffset + L.linesPerPage < total) {
      pageOffset += L.linesPerPage;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (pageOffset > 0) {
      pageOffset = std::max(0, pageOffset - L.linesPerPage);
      requestUpdate();
    }
    return;
  }
}

void SidecarDetailActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int fontId = UI_10_FONT_ID;
  const Layout L = computeLayout();
  const int total = static_cast<int>(bodyLines.size());
  if (pageOffset >= total) pageOffset = 0;

  // Title (bold, centered), with an "n/N" page indicator when the body pages.
  const int titleY = (L.bodyTop - TITLE_AREA) + 15;
  const std::string shownTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), L.contentWidth - 2 * MARGIN, EpdFontFamily::BOLD);
  const int titleX =
      L.contentX + (L.contentWidth - renderer.getTextWidth(UI_12_FONT_ID, shownTitle.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, titleY, shownTitle.c_str(), true, EpdFontFamily::BOLD);

  if (total > L.linesPerPage) {
    const int pageNo = pageOffset / L.linesPerPage + 1;
    const int pageCnt = (total + L.linesPerPage - 1) / L.linesPerPage;
    const std::string ind = std::to_string(pageNo) + "/" + std::to_string(pageCnt);
    const int indX = L.contentX + L.contentWidth - MARGIN - renderer.getTextWidth(fontId, ind.c_str());
    renderer.drawText(fontId, indX, titleY, ind.c_str(), true);
  }

  // Body window.
  int y = L.bodyTop;
  for (int i = pageOffset; i < total && i < pageOffset + L.linesPerPage; i++) {
    renderer.drawText(fontId, L.contentX + MARGIN, y, bodyLines[i].c_str(), true);
    y += L.lineHeight;
  }

  // Comment underneath (up to two lines), prefixed with the localized "Note" label.
  if (!note.empty()) {
    const int screenHeight = renderer.getScreenHeight();
    const auto orientation = renderer.getOrientation();
    const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                             orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
    const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
    const int bottomHint = (!isLandscape && !isPortraitInverted) ? 44 : 0;
    const std::string noteText = std::string(tr(STR_NOTE)) + ": " + note;
    const auto noteLines = renderer.wrappedText(fontId, noteText.c_str(), L.contentWidth - 2 * MARGIN, 2);
    int ny = screenHeight - bottomHint - static_cast<int>(noteLines.size()) * L.lineHeight - 4;
    for (const auto& ln : noteLines) {
      renderer.drawText(fontId, L.contentX + MARGIN, ny, ln.c_str(), true);
      ny += L.lineHeight;
    }
  }

  // Hints: Back / Jump / Delete / Edit  (back, confirm, previous, next).
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_JUMP), tr(STR_DELETE), tr(STR_EDIT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
