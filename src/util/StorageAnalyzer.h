#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class StorageCategory : uint8_t { Books, Images, Fonts, Dictionaries, ReadingCache, Other, Count };

struct StorageBreakdown {
  std::array<uint64_t, static_cast<size_t>(StorageCategory::Count)> bytes{};
  std::array<uint32_t, static_cast<size_t>(StorageCategory::Count)> itemCounts{};
  uint64_t totalBytes = 0;
  uint64_t usedBytes = 0;
  uint64_t freeBytes = 0;
  uint64_t scannedBytes = 0;
  uint64_t unaccountedBytes = 0;
  bool spaceValid = false;
  bool complete = true;
};

struct StorageItem {
  std::string path;
  uint64_t size = 0;
  bool deletable = false;
};

struct StorageSnapshot {
  StorageBreakdown breakdown;
  std::array<uint32_t, static_cast<size_t>(StorageCategory::Count)> logicalItemCounts{};
  std::array<std::vector<StorageItem>, static_cast<size_t>(StorageCategory::Count)> largestItems;
};

class StorageAnalyzer {
 public:
  static constexpr size_t MAX_LARGEST_ITEMS = 25;

  bool analyze(StorageSnapshot& result, bool forceSpaceRefresh = false);

  static StorageCategory categoryForPath(const std::string& path);
  static bool isReadingCacheGroupPath(const std::string& path);
  static bool canDelete(StorageCategory category, const std::string& path);
  static std::string formatBytes(uint64_t bytes);
  static std::string formatItemCount(uint32_t count, const char* singular, const char* plural);

 private:
  using FileVisitor = void (*)(const std::string& path, uint64_t size, void* context);
  using DirectoryVisitor = void (*)(const std::string& path, void* context);

  bool walk(const char* rootPath, FileVisitor fileVisitor, DirectoryVisitor directoryVisitor, void* context);
  bool walkDirectory(const std::string& path, int depth, FileVisitor fileVisitor, DirectoryVisitor directoryVisitor,
                     void* context);

  char nameBuffer[512]{};
  bool walkComplete = true;
};
