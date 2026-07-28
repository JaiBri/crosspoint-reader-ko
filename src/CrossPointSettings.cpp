#include "CrossPointSettings.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>
#include <string>

#include "I18nKeys.h"
#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

void readAndValidate(HalFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 6;  // Binary format version for migration
constexpr uint8_t SETTINGS_COUNT = 31;
constexpr char SETTINGS_FILE_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";
constexpr char SETTINGS_FILE_BAK[] = "/.crosspoint/settings.bin.bak";
constexpr char LANG_FILE_BIN[] = "/.crosspoint/language.bin";
constexpr char LANG_FILE_BAK[] = "/.crosspoint/language.bin.bak";

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_LEFT;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
  }
}

}  // namespace

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

bool CrossPointSettings::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON);
}

bool CrossPointSettings::loadFromFile() {
  // Try JSON first
  if (Storage.exists(SETTINGS_FILE_JSON)) {
    String json = Storage.readFile(SETTINGS_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = JsonSettingsIO::loadSettings(*this, json.c_str(), &resave);
      if (result && resave) {
        if (saveToFile()) {
          LOG_DBG("CPS", "Resaved settings to update format");
        } else {
          LOG_ERR("CPS", "Failed to resave settings after format update");
        }
      }
      migrateLanguageBinaryFile();
      return result;
    }
  }

  // Fall back to binary migration
  if (Storage.exists(SETTINGS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      migrateLanguageBinaryFile();
      if (saveToFile()) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
        LOG_DBG("CPS", "Migrated settings.bin to settings.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save migrated settings to JSON");
        return false;
      }
    }
  }

  // No settings files at all -- check for standalone language.bin
  return migrateLanguageBinaryFile();
}

bool CrossPointSettings::migrateLanguageBinaryFile() {
  // V1_LANGUAGES / V1_LANGUAGE_COUNT are emitted by gen_i18n.py with the
  // frozen enum order from 2f969a9.
  if (!Storage.exists(LANG_FILE_BIN)) return false;

  HalFile f;
  if (Storage.openFileForRead("CPS", LANG_FILE_BIN, f)) {
    uint8_t version;
    serialization::readPod(f, version);
    if (version == 1) {
      uint8_t oldIndex;
      serialization::readPod(f, oldIndex);
      if (oldIndex < V1_LANGUAGE_COUNT) {
        language = static_cast<uint8_t>(V1_LANGUAGES[oldIndex]);
      }
    }
  }
  Storage.rename(LANG_FILE_BIN, LANG_FILE_BAK);
  saveToFile();
  LOG_DBG("CPS", "Migrated language.bin into settings.json");
  return true;
}

bool CrossPointSettings::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }

  // Check file size for sanity
  const uint32_t fileSize = inputFile.size();
  if (fileSize < 2 || fileSize > 4096) {
    LOG_ERR("CPS", "Settings file corrupted (size=%u), deleting", fileSize);
    inputFile.close();
    Storage.remove(SETTINGS_FILE_BIN);
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);

  // Accept versions 1 through SETTINGS_FILE_VERSION for binary migration
  if (version < 1 || version > SETTINGS_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    Storage.remove(SETTINGS_FILE_BIN);
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  // Sanity check settings count
  if (fileSettingsCount > 50) {
    LOG_ERR("CPS", "Settings count invalid (%u), deleting settings file", fileSettingsCount);
    inputFile.close();
    Storage.remove(SETTINGS_FILE_BIN);
    return false;
  }

  uint8_t settingsRead = 0;
  bool frontButtonMappingRead = false;
  do {
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    // Skip fontFamily and fontSize from older versions (Korean version ignores these)
    if (version == 1) {
      uint8_t dummy;
      serialization::readPod(inputFile, dummy);  // fontFamily (ignored)
      if (++settingsRead >= fileSettingsCount) break;
      serialization::readPod(inputFile, dummy);  // fontSize (ignored)
      if (++settingsRead >= fileSettingsCount) break;
    }

    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    // Skip systemFontFamily from version 2
    if (version == 2) {
      uint8_t dummy;
      serialization::readPod(inputFile, dummy);  // systemFontFamily (ignored)
      if (++settingsRead >= fileSettingsCount) break;
    }

    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    uint8_t legacySleepTimeout = SLEEP_10_MIN;
    readAndValidate(inputFile, legacySleepTimeout, SLEEP_TIMEOUT_COUNT);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacySleepTimeout);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, longPressButtonBehavior, LONG_PRESS_BUTTON_BEHAVIOR_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    // Version 4+: Custom font path
    if (version >= 4) {
      std::string fontPathStr;
      serialization::readString(inputFile, fontPathStr);
      strncpy(customFontPath, fontPathStr.c_str(), sizeof(customFontPath) - 1);
      customFontPath[sizeof(customFontPath) - 1] = '\0';
      if (++settingsRead >= fileSettingsCount) break;
    }
    // Version 5+: Character wrap
    if (version >= 5) {
      serialization::readPod(inputFile, characterWrap);
      if (++settingsRead >= fileSettingsCount) break;
    }
    // Version 6+: Paragraph indent
    if (version >= 6) {
      serialization::readPod(inputFile, paragraphIndent);
      if (++settingsRead >= fileSettingsCount) break;
    }
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, uiTheme);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    frontButtonMappingRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, embeddedStyle);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, frontButtonFollowOrientation);
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  if (frontButtonMappingRead) {
    CrossPointSettings::validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

  LOG_DBG("CPS", "Settings loaded from binary file");
  return true;
}

float CrossPointSettings::getReaderLineCompression() const {
  // Korean line spacing: identical for the default KoPub Batang flash font AND any SD .epdfont
  // reader font. The upstream font-family-conditional values (Bookerly/NotoSans) are not used in
  // the Korean build — every reader font gets the same Korean line-height multiplier so line
  // spacing matches 1.2.0-ko regardless of which font is selected.
  switch (lineSpacing) {
    case TIGHT:
      return 1.00f;
    case NORMAL:
    default:
      return 1.20f;
    case WIDE:
      return 1.40f;
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

int CrossPointSettings::getReaderFontId() const {
  // When a custom SD reader font is set, the reader must render with the same id the font was
  // registered under (CUSTOM_FONT_ID). CUSTOM_FONT_ID is also the cache-invalidation key, so
  // switching custom fonts naturally invalidates section caches. Otherwise the built-in KoPub.
  return hasCustomFont() ? CUSTOM_FONT_ID : KOPUB_14_FONT_ID;
}

int CrossPointSettings::getUiFontId() const {
  // Fixed to Pretendard 10pt
  return UI_FONT_ID;
}

const char* CrossPointSettings::getCustomFontName() const {
  if (!hasCustomFont()) {
    return "KoPub 바탕 (기본)";
  }
  // Extract filename from path (e.g., "/.crosspoint/fonts/MyFont.bin" -> "MyFont")
  const char* lastSlash = strrchr(customFontPath, '/');
  const char* filename = lastSlash ? lastSlash + 1 : customFontPath;
  // Remove .bin extension for display
  static char nameBuffer[32];
  strncpy(nameBuffer, filename, sizeof(nameBuffer) - 1);
  nameBuffer[sizeof(nameBuffer) - 1] = '\0';
  char* dot = strrchr(nameBuffer, '.');
  if (dot) {
    *dot = '\0';
  }
  return nameBuffer;
}

const char* CrossPointSettings::getSystemFontName() const {
  if (!hasSystemFont()) {
    return "Pretendard (기본)";
  }
  // Extract filename from path (e.g., "/fonts/NotoCJK.epdfont" -> "NotoCJK")
  const char* lastSlash = strrchr(systemFontPath, '/');
  const char* filename = lastSlash ? lastSlash + 1 : systemFontPath;
  // Remove extension for display
  static char nameBuffer[32];
  strncpy(nameBuffer, filename, sizeof(nameBuffer) - 1);
  nameBuffer[sizeof(nameBuffer) - 1] = '\0';
  char* dot = strrchr(nameBuffer, '.');
  if (dot) {
    *dot = '\0';
  }
  return nameBuffer;
}
