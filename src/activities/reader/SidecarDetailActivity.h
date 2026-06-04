#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../Activity.h"

// Detail window for a single highlight or bookmark, opened from the list.
// Shows the title + body (the quote / page snippet, word-wrapped and paged with
// the side Up/Down buttons when it is longer than one screen) + the user's
// comment underneath. Buttons:
//   Back        -> return to the list (cancelled)
//   Confirm     -> Jump: returns SyncResult{spine, page} to the list
//   page-back   -> Delete: asks to confirm, then calls onDelete() and finishes
//   page-fwd    -> Edit: opens the keyboard, then calls onEditNote(newNote)
// Pure callbacks (no concrete model dependency) so it serves both highlights
// and bookmarks. The owner (list) mutates its vector via the callbacks.
class SidecarDetailActivity final : public Activity {
  std::string title;
  std::string body;
  std::string note;
  int spine;
  int page;
  std::function<void(const std::string&)> onEditNote;
  std::function<void()> onDelete;

  std::vector<std::string> bodyLines;  // body wrapped to the content width (onEnter)
  int pageOffset = 0;                  // index of the first body line shown

  struct Layout {
    int contentX = 0;
    int contentWidth = 0;
    int bodyTop = 0;
    int lineHeight = 1;
    int linesPerPage = 1;
  };
  Layout computeLayout() const;

 public:
  SidecarDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, std::string body,
                        std::string note, int spine, int page, std::function<void(const std::string&)> onEditNote,
                        std::function<void()> onDelete)
      : Activity("SidecarDetail", renderer, mappedInput),
        title(std::move(title)),
        body(std::move(body)),
        note(std::move(note)),
        spine(spine),
        page(page),
        onEditNote(std::move(onEditNote)),
        onDelete(std::move(onDelete)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
