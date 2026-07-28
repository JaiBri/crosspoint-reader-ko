#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const std::string& confirmLabel, const std::string& cancelLabel)
    : Activity("Confirmation", renderer, mappedInput),
      heading(heading),
      body(body),
      confirmLabel(confirmLabel),
      cancelLabel(cancelLabel) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
  }

  int totalHeight = 0;
  if (!safeHeading.empty()) totalHeight += lineHeight;
  if (!safeBody.empty()) totalHeight += lineHeight;
  if (!safeHeading.empty() && !safeBody.empty()) totalHeight += spacing;

  startY = (renderer.getScreenHeight() - totalHeight) / 2;

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  if (!safeBody.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
  }

  // Draw UI Elements. Left = cancel, Right = confirm; labels are overridable
  // (e.g. "No" / "Yes" for the optional-comment prompt) and otherwise default
  // to the generic Cancel / Confirm strings.
  const char* cancelText = cancelLabel.empty() ? I18N.get(StrId::STR_CANCEL) : cancelLabel.c_str();
  const char* confirmText = confirmLabel.empty() ? I18N.get(StrId::STR_CONFIRM) : confirmLabel.c_str();
  const auto labels = mappedInput.mapLabels("", "", cancelText, confirmText);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }

  // Physical Back behaves like Cancel so callers always get a defined result
  // (an unhandled Back would otherwise yield a default isCancelled=false).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}