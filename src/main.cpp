#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <SdFontFamily.h>
#include <WiFi.h>
#include <builtinFonts/all.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FontManager.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
FontCacheManager fontCacheManager(renderer.getFontMap());
static unsigned long allowSleepAt = 0;

// UI Font (Pretendard 10pt) - Regular only, synthetic bold applied by renderer
EpdFont pretendard10RegularFont(&pretendard_10_regular);
EpdFontFamily uiFontFamily(&pretendard10RegularFont);

// Korean EPUB reader font (KoPub Batang 14pt) - Regular only, synthetic bold applied by renderer
EpdFont kopub14RegularFont(&kopub_14_regular);
EpdFontFamily kopub14FontFamily(&kopub14RegularFont);

// Korean fonts loading from SD card is disabled due to memory constraints
// Font files should be in /.crosspoint/fonts/ directory
constexpr char FONT_DIR[] = "/.crosspoint/fonts";

// Helper function to safely load an SD font with comprehensive error handling
// Returns true if loading succeeded
bool trySdFontLoad(GfxRenderer& renderer, int fontId, const char* name, const char* regularPath,
                   const char* boldPath = nullptr) {
  // First check if the file exists before attempting to create SdFontFamily
  if (!Storage.exists(regularPath)) {
    LOG_ERR("FNT", "%s not found: %s", name, regularPath);
    return false;
  }

  SdFontFamily* font = nullptr;
  bool success = false;

  // Create font family - use regular new since ESP32 doesn't always support nothrow
  font = new SdFontFamily(regularPath, boldPath);
  if (font == nullptr) {
    LOG_ERR("FNT", "Failed to allocate memory for %s", name);
    return false;
  }

  if (font->load()) {
    renderer.insertSdFont(fontId, font);
    LOG_DBG("FNT", "Loaded %s from SD", name);
    success = true;
  } else {
    LOG_ERR("FNT", "Failed to load %s", name);
    delete font;
  }

  return success;
}

// Load custom reader font from SD card if configured
// Returns true if custom font was loaded successfully
bool loadCustomReaderFont(GfxRenderer& gfxRenderer) {
  if (!SETTINGS.hasCustomFont()) {
    LOG_DBG("FNT", "No custom font configured, using default KoPub Batang");
    return false;
  }

  const char* fontPath = SETTINGS.customFontPath;
  LOG_DBG("FNT", "Loading custom font: %s", fontPath);

  if (!Storage.exists(fontPath)) {
    LOG_ERR("FNT", "Custom font file not found: %s", fontPath);
    // Clear invalid font path
    SETTINGS.customFontPath[0] = '\0';
    SETTINGS.saveToFile();
    return false;
  }

  // Try to load the custom font
  if (trySdFontLoad(gfxRenderer, CUSTOM_FONT_ID, "CustomReaderFont", fontPath)) {
    LOG_DBG("FNT", "Custom reader font loaded successfully");
    return true;
  }

  LOG_ERR("FNT", "Failed to load custom font, clearing setting to use default");
  // Clear invalid font path so getReaderFontId() returns default font
  SETTINGS.customFontPath[0] = '\0';
  SETTINGS.saveToFile();
  return false;
}

// Reload custom reader font - removes old font and loads new one
// Call this when font settings change to apply immediately without reboot
bool reloadCustomReaderFont() {
  LOG_DBG("FNT", "Reloading custom reader font...");

  // Remove existing custom font if any
  if (renderer.hasFont(CUSTOM_FONT_ID)) {
    renderer.removeFont(CUSTOM_FONT_ID);
    LOG_DBG("FNT", "Removed previous custom font");
  }

  // Load new custom font if configured
  return loadCustomReaderFont(renderer);
}

// Load the UI system font from SD card (if configured) and make it the primary UI font,
// with Pretendard kept as its glyph-level fallback. The whole UI (all UI_FONT_ID slots)
// renders with the SD font; any codepoint the SD font lacks falls back to Pretendard.
// When no system font is configured (or the user clears it), the UI uses Pretendard only.
// Returns true if a system font is now active.
bool loadSystemFont(GfxRenderer& gfxRenderer) {
  if (!SETTINGS.hasSystemFont()) {
    LOG_DBG("FNT", "No system font configured, UI uses Pretendard only");
    return false;
  }

  const char* fontPath = SETTINGS.systemFontPath;
  LOG_DBG("FNT", "Loading system font: %s", fontPath);

  if (!Storage.exists(fontPath)) {
    LOG_ERR("FNT", "System font file not found: %s", fontPath);
    SETTINGS.systemFontPath[0] = '\0';
    SETTINGS.saveToFile();
    return false;
  }

  if (trySdFontLoad(gfxRenderer, SYSTEM_FONT_ID, "SystemFont", fontPath)) {
    // SD system font becomes the primary UI font; Pretendard backs it as the glyph-level
    // fallback for codepoints the SD font lacks. The redirect points every UI_FONT_ID
    // request (all UI slots alias it) at the SD font slot, so the whole UI switches over.
    gfxRenderer.setGlyphFallback(SYSTEM_FONT_ID, UI_FONT_ID);
    gfxRenderer.setFontRedirect(UI_FONT_ID, SYSTEM_FONT_ID);
    LOG_DBG("FNT", "System font loaded as primary UI font (Pretendard fallback)");
    return true;
  }

  LOG_ERR("FNT", "Failed to load system font, clearing setting");
  SETTINGS.systemFontPath[0] = '\0';
  SETTINGS.saveToFile();
  return false;
}

// Reload UI system font - removes old SD system font and loads the configured one.
// Call this when the system-font setting changes to apply immediately without reboot.
// If the user cleared the setting, the UI reverts to Pretendard.
bool reloadSystemFont() {
  LOG_DBG("FNT", "Reloading system font...");

  // Drop the redirect first so UI_FONT_ID stops resolving to a font we are about to free,
  // then detach the SD font's fallback wiring before removing it.
  renderer.clearFontRedirect();
  renderer.clearGlyphFallback(SYSTEM_FONT_ID);

  if (renderer.hasFont(SYSTEM_FONT_ID)) {
    renderer.removeFont(SYSTEM_FONT_ID);
    LOG_DBG("FNT", "Removed previous system font");
  }

  return loadSystemFont(renderer);
}

// Get reference to global renderer (for font operations from other modules)
GfxRenderer& getGlobalRenderer() { return renderer; }

// SD font loading is disabled - Korean fonts need to be embedded in flash
// due to ESP32-C3 memory constraints. SD card loading causes crashes.
void loadSdFonts(GfxRenderer& /*renderer*/) {
  // SD font loading disabled - use flash-embedded fonts instead
  LOG_DBG("FNT", "SD font loading disabled (use flash fonts)");
}

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() < calibratedPressDuration);
    abort = gpio.getPowerButtonHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    powerManager.startDeepSleep(gpio);
  }
}
void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  // Korean build: upstream NotoSerif/NotoSans/Ubuntu built-ins are not loaded.
  // KoPub Batang is the default reader font; Pretendard drives all UI sizes.

  // UI font (Pretendard 10pt) - used for all UI sizes in Korean version
  renderer.insertFont(UI_FONT_ID, &uiFontFamily);
  renderer.insertFont(UI_10_FONT_ID, &uiFontFamily);
  renderer.insertFont(UI_12_FONT_ID, &uiFontFamily);
  renderer.insertFont(SMALL_FONT_ID, &uiFontFamily);

  // Korean EPUB reader font (KoPub Batang 14pt) - always register as fallback
  renderer.insertFont(KOPUB_14_FONT_ID, &kopub14FontFamily);

  // Try to load custom reader font from SD card
  loadCustomReaderFont(renderer);

  // Set fallback font to Pretendard UI
  renderer.setFallbackFont(UI_FONT_ID);

  // Load the optional UI system font from SD and wire it as the UI glyph-level fallback
  // (lets Hanja/Kana book titles render even though Pretendard is Hangul/Latin only).
  loadSystemFont(renderer);

  // SD card fonts loading disabled due to memory constraints
  loadSdFonts(renderer);

  LOG_DBG("MAIN", "Fonts setup complete");
}

void setup() {
  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif

  HalSystem::begin();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  // Force CPU to 160 MHz before HalPowerManager records normalFreq, otherwise
  // we inherit whatever the second-stage bootloader left us with. On locked X3
  // devices the original Xteink bootloader hands off at a much lower clock
  // (observed ~10–40 MHz), and HalPowerManager would then treat that low
  // clock as "normal" forever — every operation in the app runs 4–16× slower
  // than it should, manifesting as the "lag-like" pauses on activity exit
  // and the long book-load times that don't reproduce on unlocked X3.
  setCpuFrequencyMhz(160);
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

#ifdef ENABLE_SERIAL_LOG
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    const unsigned long start = millis();
    while (!Serial && (millis() - start) < 500) {
      delay(10);
    }
  }
#endif

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon.
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  // cppcheck-suppress unreadVariable  ; referenced only inside LOG_DBG, which compiles out at LOG_LEVEL<2
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
