#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

// Generic list of sidecar entries (highlights or bookmarks), opened from the
// reader menu. Each row carries a one-line display label plus the data the
// detail window needs. Navigate with the side/page buttons; Confirm opens
// SidecarDetailActivity for the selected row; Back cancels. Edits and deletes
// performed in the detail are applied to the owner's real vector through the
// index-keyed onEdit/onDelete callbacks (and mirrored into this list's own row
// copy so the display stays in sync); a Jump from the detail bubbles up to the
// reader as a SyncResult{spine, page}.
class SidecarListActivity final : public Activity {
 public:
  struct Row {
    std::string label;  // one-line list label (formatted by the caller)
    std::string body;   // full text shown in the detail window
    std::string note;   // current comment
    int spine = 0;
    int page = 0;
  };

 private:
  std::vector<Row> rows;
  std::string title;
  std::string emptyMessage;
  std::function<void(int, const std::string&)> onEdit;  // (index, newNote)
  std::function<void(int)> onDelete;                    // (index)
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  int getPageItems() const;
  void openDetail();

 public:
  SidecarListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<Row> rows,
                      std::string title, std::string emptyMessage, std::function<void(int, const std::string&)> onEdit,
                      std::function<void(int)> onDelete)
      : Activity("SidecarList", renderer, mappedInput),
        rows(std::move(rows)),
        title(std::move(title)),
        emptyMessage(std::move(emptyMessage)),
        onEdit(std::move(onEdit)),
        onDelete(std::move(onDelete)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
