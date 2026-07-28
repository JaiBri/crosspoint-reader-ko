#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

/**
 * Activity for selecting a custom font from /.crosspoint/fonts folder.
 * Lists .bin font files and allows the user to select one.
 */
class FontSelectionActivity final : public ActivityWithSubactivity {
 public:
  // Which font slot this picker configures:
  //   Reader  -> SETTINGS.customFontPath (EPUB body font; default KoPub Batang)
  //   System  -> SETTINGS.systemFontPath (UI glyph fallback; default Pretendard)
  enum class Target { Reader, System };

  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Target target = Target::Reader)
      : ActivityWithSubactivity("FontSelection", renderer, mappedInput), target_(target) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t displayMutex = nullptr;
  bool updateRequired = false;

  Target target_ = Target::Reader;
  int selectedIndex = 0;
  std::vector<std::string> fontFiles;  // List of font file paths
  std::vector<std::string> fontNames;  // Display names (without path and extension)

  bool isSystemTarget() const { return target_ == Target::System; }

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void loadFontList();
  void handleSelection();

  static constexpr const char* FONTS_DIR = "/.crosspoint/fonts";
  static constexpr const char* ROOT_FONTS_DIR = "/fonts";
  static constexpr const char* HIDDEN_FONTS_DIR = "/.fonts";  // upstream hidden font root

  // Scans dirPath for .epdfont files; when recurseIntoSubdirs is true, also descends one level
  // into per-family subfolders (layout /fonts/<Family>/<Family>_<size>.epdfont).
  void scanFontsInDirectory(const char* dirPath, bool recurseIntoSubdirs = true);
};
