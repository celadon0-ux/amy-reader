#pragma once

#include <cstdint>
#include <string>

struct SubstackRssItem;

class SubstackEpubWriter {
 public:
  // Increment when generated article contents need to be rebuilt in place.
  static constexpr uint32_t CONTENT_VERSION = 1;
  static bool write(const SubstackRssItem& item, const std::string& publication, const std::string& articleId,
                    std::string& outputPath, uint64_t& outputBytes);
  static std::string sanitizeFilename(const std::string& value, size_t maxLength = 72);
};
