#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"
#include "util/HighlightStore.h"

// On-device management of a book's highlights, opened from the reader menu.
// Shows each highlight (chapter/page + extracted snippet). Short Confirm jumps
// to the highlight (returns a SyncResult{spineIndex, page}); long-press Confirm
// deletes it; Back cancels. Operates directly on the reader's highlight vector
// (the reader is suspended on the stack while this is active) and invokes the
// onChanged callback so the reader can persist the Markdown sidecar.
class HighlightListActivity final : public Activity {
  std::vector<highlight::Highlight>& highlights;
  std::function<void()> onChanged;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool deleteArmed = false;  // long-press delete already fired during this hold

  int getPageItems() const;

 public:
  HighlightListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        std::vector<highlight::Highlight>& highlights, std::function<void()> onChanged)
      : Activity("HighlightList", renderer, mappedInput),
        highlights(highlights),
        onChanged(std::move(onChanged)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
