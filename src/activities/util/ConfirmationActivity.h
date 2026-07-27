#pragma once
#include <functional>
#include <string>

#include "../../fontIds.h"
#include "../Activity.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;
  // Optional custom button labels; empty falls back to STR_CONFIRM / STR_CANCEL.
  std::string confirmLabel;
  std::string cancelLabel;

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;

  std::string safeHeading;
  std::string safeBody;
  int startY = 0;
  int lineHeight = 0;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, const std::string& confirmLabel = "",
                       const std::string& cancelLabel = "");

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};