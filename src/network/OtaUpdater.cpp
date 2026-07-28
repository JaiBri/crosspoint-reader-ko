#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen before esp_http_client (which includes lwip). Pin this
// order; clang-format would otherwise sort the local header last and break the
// build.
#include "HttpDownloader.h"
#include "FirmwareFlasher.h"
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_wifi.h>
// clang-format on

namespace {
// Korean fork release URL
constexpr char latestReleaseUrl[] =
    "https://api.github.com/repos/crosspoint-reader-ko/crosspoint-reader-ko/releases/latest";
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

bool OtaUpdater::isUpdateNewerKO() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  // Parse version: major.minor.patch-ko.koVersion
  auto parseVersion = [](const std::string& version, int& major, int& minor, int& patch, int& ko) {
    major = minor = patch = ko = 0;

    // Find -ko. suffix
    size_t koPos = version.find("-ko.");
    std::string baseVersion = (koPos != std::string::npos) ? version.substr(0, koPos) : version;

    // Parse ko version if present
    if (koPos != std::string::npos) {
      ko = stoi(version.substr(koPos + 4));
    }

    // Parse major.minor.patch
    size_t firstDot = baseVersion.find('.');
    size_t lastDot = baseVersion.find_last_of('.');

    if (firstDot != std::string::npos) {
      major = stoi(baseVersion.substr(0, firstDot));
      if (lastDot != firstDot) {
        minor = stoi(baseVersion.substr(firstDot + 1, lastDot - firstDot - 1));
        patch = stoi(baseVersion.substr(lastDot + 1));
      } else {
        minor = stoi(baseVersion.substr(firstDot + 1));
      }
    }
  };

  int updateMajor, updateMinor, updatePatch, updateKo;
  int currentMajor, currentMinor, currentPatch, currentKo;

  parseVersion(latestVersion, updateMajor, updateMinor, updatePatch, updateKo);
  parseVersion(CROSSPOINT_VERSION, currentMajor, currentMinor, currentPatch, currentKo);

  if (updateMajor != currentMajor) return updateMajor > currentMajor;
  if (updateMinor != currentMinor) return updateMinor > currentMinor;
  if (updatePatch != currentPatch) return updatePatch > currentPatch;
  return updateKo > currentKo;
}

namespace {
constexpr const char* kOtaSdPath = "/.crosspoint/ota_firmware.bin";

// Stash the activity-side progress callback so the firmware-flasher's free
// progress callback can fan back into it (we need a void* ctx hop).
struct FlashCtx {
  OtaUpdater* updater;
  OtaUpdater::ProgressCallback onProgress;
  void* userCtx;
};

// Per-call download state shared with the event handler.
struct DownloadCtx {
  OtaUpdater* updater;
  HalFile* sdFile;
  size_t written;
  bool writeFailed;
  OtaUpdater::ProgressCallback onProgress;
  void* userCtx;
};

esp_err_t download_event_handler(esp_http_client_event_t* evt) {
  auto* dctx = static_cast<DownloadCtx*>(evt->user_data);
  switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
      // capture Content-Length when the server provides it
      if (evt->header_key && evt->header_value && strcasecmp(evt->header_key, "Content-Length") == 0) {
        const int len = atoi(evt->header_value);
        if (len > 0) dctx->updater->setExpectedSize(static_cast<size_t>(len));
      }
      break;
    case HTTP_EVENT_ON_DATA:
      if (dctx->writeFailed) return ESP_OK;
      if (evt->data_len > 0 && dctx->sdFile && *dctx->sdFile) {
        const size_t want = static_cast<size_t>(evt->data_len);
        const size_t wrote = dctx->sdFile->write(static_cast<const uint8_t*>(evt->data), want);
        if (wrote != want) {
          LOG_ERR("OTA", "SD write short @%u (got=%u want=%u)", static_cast<unsigned>(dctx->written),
                  static_cast<unsigned>(wrote), static_cast<unsigned>(want));
          dctx->writeFailed = true;
          dctx->updater->setLastError("sd_write");
          return ESP_FAIL;
        }
        dctx->written += want;
        dctx->updater->setProcessed(dctx->written);
        if (dctx->onProgress) dctx->onProgress(dctx->userCtx);
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}
}  // namespace

void OtaUpdater::setLastError(const std::string& err) { lastError = err; }
void OtaUpdater::setExpectedSize(size_t s) {
  totalSize = s;
  render = true;
}
void OtaUpdater::setProcessed(size_t s) {
  processedSize = s;
  render = true;
}

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  lastError.clear();
  if (!isUpdateNewerKO()) {
    lastError = "not_newer";
    return UPDATE_OLDER_ERROR;
  }

  // Two-phase: (1) download the patched firmware.bin to SD via
  // esp_http_client_perform (same proven pattern as checkForUpdate uses) +
  // event handler that streams chunks straight to a file on the SD card,
  // (2) flash that file using firmware_flash::flashFromSdPath. Skipping
  // esp_https_ota_* avoids the running ESP-IDF's bogus esp_image_verify
  // efuse-blk-rev rejection on X4 silicon. The cached SD file also lets
  // the user retry the SD update flow if anything dies mid-flash.
  phase = Phase::Downloading;
  totalSize = otaSize;  // GitHub release asset size — pre-populated from checkForUpdate
  processedSize = 0;
  render = true;
  if (onProgress) onProgress(ctx);

  Storage.mkdir("/.crosspoint", true);

  HalFile sdFile;
  if (!Storage.openFileForWrite("OTA", kOtaSdPath, sdFile) || !sdFile) {
    LOG_ERR("OTA", "open SD cache for write failed: %s", kOtaSdPath);
    lastError = "sd_open";
    return INTERNAL_UPDATE_ERROR;
  }

  DownloadCtx dctx{this, &sdFile, 0, false, onProgress, ctx};

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 30000,
      .event_handler = download_event_handler,
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .user_data = &dctx,
      /* Enforce CN/SAN hostname verification — crt_bundle_attach validates the CA chain but
       * hostname matching is a separate step. Leaving this true would let any cert signed by a
       * trusted CA serve a tampered firmware over HTTPS. */
      .skip_cert_common_name_check = false,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_http_client_handle_t client = esp_http_client_init(&client_config);
  if (!client) {
    LOG_ERR("OTA", "esp_http_client_init failed");
    lastError = "http_init";
    return INTERNAL_UPDATE_ERROR;
  }
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  // Explicit close required before flashFromSdPath re-opens the same path for read.
  sdFile.close();

  if (err != ESP_OK) {
    LOG_ERR("OTA", "http perform failed: %s (status=%d)", esp_err_to_name(err), status);
    char buf[48];
    snprintf(buf, sizeof(buf), "http_perform:%s", esp_err_to_name(err));
    lastError = buf;
    return HTTP_ERROR;
  }
  if (status / 100 != 2) {
    LOG_ERR("OTA", "http status %d", status);
    char buf[24];
    snprintf(buf, sizeof(buf), "http_status:%d", status);
    lastError = buf;
    return HTTP_ERROR;
  }
  if (dctx.writeFailed) {
    return INTERNAL_UPDATE_ERROR;  // lastError already set
  }
  if (dctx.written == 0) {
    LOG_ERR("OTA", "no body bytes received");
    lastError = "empty_body";
    return HTTP_ERROR;
  }
  // Reject truncated downloads before flashing. The firmware-flasher only does a magic-byte /
  // min-size check on the SD file, so a short body (network drop after Content-Length is known)
  // would otherwise still go through and brick on reboot.
  const size_t expectedSize = totalSize > 0 ? totalSize : otaSize;
  if (expectedSize > 0 && dctx.written != expectedSize) {
    LOG_ERR("OTA", "short body: got=%u want=%u", static_cast<unsigned>(dctx.written),
            static_cast<unsigned>(expectedSize));
    char buf[48];
    snprintf(buf, sizeof(buf), "short_body:%u/%u", static_cast<unsigned>(dctx.written),
             static_cast<unsigned>(expectedSize));
    lastError = buf;
    return HTTP_ERROR;
  }
  LOG_INF("OTA", "download complete: %u bytes -> %s", static_cast<unsigned>(dctx.written), kOtaSdPath);

  // Phase 2: flash from SD using the shared firmware flasher.
  phase = Phase::Flashing;
  totalSize = dctx.written;
  processedSize = 0;
  render = true;
  if (onProgress) onProgress(ctx);

  FlashCtx flashCtx{this, onProgress, ctx};
  auto progressCb = +[](size_t written, size_t total, void* fctx) {
    auto* fc = static_cast<FlashCtx*>(fctx);
    fc->updater->setProcessed(written);
    fc->updater->setExpectedSize(total);
    if (fc->onProgress) fc->onProgress(fc->userCtx);
  };

  const auto fr = firmware_flash::flashFromSdPath(kOtaSdPath, progressCb, &flashCtx);
  if (fr != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "flash failed: %s", firmware_flash::resultName(fr));
    char buf[32];
    snprintf(buf, sizeof(buf), "flash:%s", firmware_flash::resultName(fr));
    lastError = buf;
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
