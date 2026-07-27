#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, bool paragraphIndent,
                              uint8_t paragraphAlignment, bool characterWrap, uint16_t viewportWidth,
                              uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                              uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  // The pagination parameters recorded in a section cache header, plus the page
  // count they produced. Mirrors the header written by writeSectionFileHeader().
  struct CachedPagination {
    int fontId = 0;
    float lineCompression = 0.0f;
    bool extraParagraphSpacing = false;
    bool paragraphIndent = false;
    uint8_t paragraphAlignment = 0;
    bool characterWrap = false;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
    bool hyphenationEnabled = false;
    bool embeddedStyle = false;
    uint8_t imageRendering = 0;
    uint16_t pageCount = 0;
  };

  uint16_t pageCount = 0;
  int currentPage = 0;

  // Cache file location for a spine item. Shared by the constructor and by
  // callers that want to inspect a cache without constructing a Section.
  static std::string cacheFilePath(const std::string& cachePath, int spineIndex);

  // Read a section cache header WITHOUT loading or invalidating it.
  //
  // Unlike loadSectionFile(), this never calls clearCache() on a mismatch — it
  // reports what the cache says and lets the caller decide. That distinction is
  // load-bearing: callers use this to ask "was this chapter paginated under the
  // layout I care about?", and deleting the cache as a side effect would force
  // a multi-second repagination of every chapter inspected.
  //
  // Returns false (leaving `out` untouched) if the file is missing, shorter than
  // the header, or written by a different SECTION_FILE_VERSION.
  static bool readCachedPagination(const std::string& sectionFilePath, CachedPagination& out);

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(cacheFilePath(epub->getCachePath(), spineIndex)) {}
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, bool paragraphIndent,
                       uint8_t paragraphAlignment, bool characterWrap, uint16_t viewportWidth, uint16_t viewportHeight,
                       bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, bool paragraphIndent,
                         uint8_t paragraphAlignment, bool characterWrap, uint16_t viewportWidth,
                         uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering,
                         const std::function<void()>& popupFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
};
