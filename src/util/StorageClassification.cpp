#include "StorageAnalyzer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {
std::string lowerCopy(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool isAtOrBelow(const std::string& path, const char* root) {
  const size_t rootLength = strlen(root);
  return path.compare(0, rootLength, root) == 0 &&
         (path.size() == rootLength || (path.size() > rootLength && path[rootLength] == '/'));
}

bool hasExtension(const std::string& path, const char* extension) {
  const size_t extensionLength = strlen(extension);
  return path.size() >= extensionLength && path.compare(path.size() - extensionLength, extensionLength, extension) == 0;
}

bool isBookCachePath(const std::string& path) {
  constexpr char ROOT[] = "/.crosspoint/";
  if (path.compare(0, sizeof(ROOT) - 1, ROOT) != 0) return false;
  const size_t componentStart = sizeof(ROOT) - 1;
  const size_t componentEnd = path.find('/', componentStart);
  const std::string component = path.substr(componentStart, componentEnd - componentStart);
  return component.rfind("epub_", 0) == 0 || component.rfind("txt_", 0) == 0 || component.rfind("xtc_", 0) == 0;
}
}  // namespace

StorageCategory StorageAnalyzer::categoryForPath(const std::string& path) {
  const std::string lower = lowerCopy(path);

  if (isBookCachePath(lower)) return StorageCategory::ReadingCache;
  if (isAtOrBelow(lower, "/.fonts") || isAtOrBelow(lower, "/fonts")) return StorageCategory::Fonts;
  if (isAtOrBelow(lower, "/.dictionaries") || isAtOrBelow(lower, "/dictionaries")) {
    return StorageCategory::Dictionaries;
  }
  if (hasExtension(lower, ".epub") || hasExtension(lower, ".txt") || hasExtension(lower, ".md") ||
      hasExtension(lower, ".xtc") || hasExtension(lower, ".xtch")) {
    return StorageCategory::Books;
  }
  if (hasExtension(lower, ".bmp") || hasExtension(lower, ".jpg") || hasExtension(lower, ".jpeg") ||
      hasExtension(lower, ".png")) {
    return StorageCategory::Images;
  }
  return StorageCategory::Other;
}

bool StorageAnalyzer::canDelete(const StorageCategory category, const std::string& path) {
  if (category == StorageCategory::Books) return true;
  if (category != StorageCategory::Images) return false;
  return isAtOrBelow(lowerCopy(path), "/screenshots");
}

bool StorageAnalyzer::isReadingCacheGroupPath(const std::string& path) {
  const std::string lower = lowerCopy(path);
  constexpr char ROOT[] = "/.crosspoint/";
  if (lower.compare(0, sizeof(ROOT) - 1, ROOT) != 0) return false;
  const std::string component = lower.substr(sizeof(ROOT) - 1);
  if (component.empty() || component.find('/') != std::string::npos) return false;
  return component.rfind("epub_", 0) == 0 || component.rfind("txt_", 0) == 0 ||
         component.rfind("xtc_", 0) == 0;
}

std::string StorageAnalyzer::formatBytes(const uint64_t bytes) {
  constexpr uint64_t KIB = 1024;
  constexpr uint64_t MIB = KIB * 1024;
  constexpr uint64_t GIB = MIB * 1024;
  char buffer[32];
  if (bytes >= GIB) {
    snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / static_cast<double>(GIB));
  } else if (bytes >= MIB) {
    snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / static_cast<double>(MIB));
  } else if (bytes >= KIB) {
    snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / static_cast<double>(KIB));
  } else {
    snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buffer;
}

std::string StorageAnalyzer::formatItemCount(const uint32_t count, const char* singular, const char* plural) {
  return std::to_string(count) + " " + (count == 1 ? singular : plural);
}
