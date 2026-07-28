#include "ReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace {
constexpr const char* MODULE = "RSTAT";
constexpr const char* FILENAME = "/reading_stats.bin";

void writeU32(uint8_t* buf, uint32_t v) {
  buf[0] = v & 0xFF;
  buf[1] = (v >> 8) & 0xFF;
  buf[2] = (v >> 16) & 0xFF;
  buf[3] = (v >> 24) & 0xFF;
}

uint32_t readU32(const uint8_t* buf) {
  return static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) | (static_cast<uint32_t>(buf[2]) << 16) |
         (static_cast<uint32_t>(buf[3]) << 24);
}
}  // namespace

bool ReadingStats::load(const std::string& cachePath, ReadingStats& out) {
  out = ReadingStats{};

  HalFile f;
  if (!Storage.openFileForRead(MODULE, cachePath + FILENAME, f)) {
    return false;
  }

  uint8_t data[FILE_SIZE];
  const int got = f.read(data, FILE_SIZE);
  if (got != static_cast<int>(FILE_SIZE)) {
    LOG_DBG(MODULE, "Truncated stats file (%d bytes)", got);
    return false;
  }

  const uint32_t magic = readU32(data);
  if (magic != MAGIC) {
    LOG_DBG(MODULE, "Bad magic 0x%08lx", static_cast<unsigned long>(magic));
    return false;
  }
  if (data[4] != FILE_VERSION) {
    LOG_DBG(MODULE, "Version mismatch (%u)", data[4]);
    return false;
  }

  out.totalSeconds = readU32(data + 8);
  out.sessionCount = readU32(data + 12);
  return true;
}

bool ReadingStats::save(const std::string& cachePath, const ReadingStats& stats) {
  HalFile f;
  if (!Storage.openFileForWrite(MODULE, cachePath + FILENAME, f)) {
    LOG_ERR(MODULE, "Failed to open stats for write");
    return false;
  }

  uint8_t data[FILE_SIZE];
  std::memset(data, 0, sizeof(data));
  writeU32(data, MAGIC);
  data[4] = FILE_VERSION;
  writeU32(data + 8, stats.totalSeconds);
  writeU32(data + 12, stats.sessionCount);

  const size_t wrote = f.write(data, FILE_SIZE);
  f.close();
  if (wrote != FILE_SIZE) {
    LOG_ERR(MODULE, "Short write (%u bytes)", static_cast<unsigned>(wrote));
    return false;
  }
  return true;
}

void ReadingStats::format(uint32_t seconds, char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return;

  const uint32_t minutes = seconds / 60;
  const uint32_t hours = minutes / 60;
  const uint32_t days = hours / 24;

  if (days >= 100) {
    snprintf(buf, bufSize, "%lud", static_cast<unsigned long>(days));
  } else if (days >= 1) {
    snprintf(buf, bufSize, "%lud %luh", static_cast<unsigned long>(days), static_cast<unsigned long>(hours % 24));
  } else if (hours >= 1) {
    snprintf(buf, bufSize, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes % 60));
  } else {
    snprintf(buf, bufSize, "%lum", static_cast<unsigned long>(minutes));
  }
}
