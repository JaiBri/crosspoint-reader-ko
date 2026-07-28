/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <string>
#include <utility>

#include "activities/Activity.h"

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage();
  // XTG 1-bit page render via row-aligned band streaming (no large page buffer).
  void renderStreaming1Bit(uint16_t pageWidth, uint16_t pageHeight);
  void renderStatusBarOverlay(StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc)
      : Activity("XtcReader", renderer, mappedInput), xtc(std::move(xtc)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
