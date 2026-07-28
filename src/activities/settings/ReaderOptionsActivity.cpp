#include "ReaderOptionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SettingsList.h"
#include "components/UITheme.h"
#include "fontIds.h"

ReaderOptionsActivity::ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool forTxtReader)
    : Activity("ReaderOptions", renderer, mappedInput), forTxtReader(forTxtReader) {}

std::vector<SettingInfo> ReaderOptionsActivity::buildSettings(bool forTxtReader) {
  std::vector<SettingInfo> result;
  result.reserve(20);

  for (const auto& s : getSettingsList()) {
    if (s.type != SettingType::TOGGLE && s.type != SettingType::ENUM && s.type != SettingType::VALUE) {
      continue;
    }
    if (s.valuePtr == nullptr) {
      continue;
    }
    // Reader menu already exposes orientation as an inline rotate cycle, so
    // omit it here to avoid duplicate exposure with subtly different UX
    // (cycle-on-confirm vs. enum-cycle).
    if (s.nameId == StrId::STR_ORIENTATION) {
      continue;
    }
    // Drop EPUB-only settings when hosted by the TXT reader: TXT files have
    // no embedded images and no HTML/CSS styling, so these toggles do
    // nothing and would mislead the user.
    if (forTxtReader && (s.nameId == StrId::STR_IMAGES || s.nameId == StrId::STR_EMBEDDED_STYLE)) {
      continue;
    }
    if (s.category == StrId::STR_CAT_READER || s.category == StrId::STR_CAT_CONTROLS) {
      result.push_back(s);
    }
  }

  const auto& list = getSettingsList();
  const auto fading = std::find_if(list.begin(), list.end(), [](const SettingInfo& s) {
    return s.nameId == StrId::STR_SUNLIGHT_FADING_FIX && s.valuePtr != nullptr;
  });
  if (fading != list.end()) {
    result.push_back(*fading);
  }
  return result;
}

void ReaderOptionsActivity::onEnter() {
  Activity::onEnter();
  settings = buildSettings(forTxtReader);
  settingsCount = static_cast<int>(settings.size());
  selectedSettingIndex = 0;
  requestUpdate();
}

void ReaderOptionsActivity::onExit() {
  // Persist any changes before returning to the reader. The reader will
  // detect render-affecting changes on its next render() pass and invalidate
  // its section cache as needed.
  SETTINGS.saveToFile();
  Activity::onExit();
}

void ReaderOptionsActivity::loop() {
  // Back uses wasReleased so we consume the release edge here — otherwise
  // EpubReaderActivity::loop() would see the same release event and trigger
  // its short-press "go home" handler when we return.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Confirm uses wasPressed (matches SettingsActivity / FontSelectionActivity).
  // If we used wasReleased here, the release edge after a sub-activity (e.g.
  // FontSelection) commits would fire toggleCurrentSetting again and relaunch
  // the sub-activity — an infinite loop on the font row.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount);
    requestUpdate();
  });
}

void ReaderOptionsActivity::toggleCurrentSetting() {
  if (selectedSettingIndex < 0 || selectedSettingIndex >= settingsCount) {
    return;
  }
  const auto& setting = settings[selectedSettingIndex];

  // Mirrors SettingsActivity::toggleCurrentSetting(): the toggle/cycle
  // semantics for shared SettingInfo entries must stay identical between the
  // two screens, otherwise users would see different behavior depending on
  // where they touched the same setting.
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  }
}

void ReaderOptionsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READER_OPTIONS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  const auto& list = settings;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, settingsCount, selectedSettingIndex,
      [&list](int index) { return std::string(I18N.get(list[index].nameId)); }, nullptr, nullptr,
      [&list](int i) {
        const auto& setting = list[i];
        std::string valueText;
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          valueText = std::to_string(SETTINGS.*(setting.valuePtr));
        }
        return valueText;
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
