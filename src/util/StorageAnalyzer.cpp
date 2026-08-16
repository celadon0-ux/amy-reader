#include "StorageAnalyzer.h"

#include <HalStorage.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr int MAX_SCAN_DEPTH = 24;
constexpr size_t MAX_SCAN_PATH = 768;

size_t categoryIndex(const StorageCategory category) { return static_cast<size_t>(category); }

void retainLargest(std::vector<StorageItem>& items, StorageItem item) {
  if (items.size() < StorageAnalyzer::MAX_LARGEST_ITEMS) {
    items.push_back(std::move(item));
    return;
  }

  auto smallest = std::min_element(items.begin(), items.end(),
                                   [](const StorageItem& lhs, const StorageItem& rhs) { return lhs.size < rhs.size; });
  if (smallest != items.end() && item.size > smallest->size) *smallest = std::move(item);
}

void accumulateFile(const std::string& path, const uint64_t size, void* opaque) {
  auto& snapshot = *static_cast<StorageSnapshot*>(opaque);
  auto& result = snapshot.breakdown;
  const StorageCategory category = StorageAnalyzer::categoryForPath(path);
  const size_t index = categoryIndex(category);
  result.bytes[index] += size;
  result.itemCounts[index]++;
  result.scannedBytes += size;

  if (category == StorageCategory::Books || category == StorageCategory::Images ||
      category == StorageCategory::Other) {
    snapshot.logicalItemCounts[index]++;
  }
  retainLargest(snapshot.largestItems[index], {path, size, StorageAnalyzer::canDelete(category, path)});
}

void accumulateDirectory(const std::string& path, void* opaque) {
  if (!StorageAnalyzer::isReadingCacheGroupPath(path)) return;
  auto& snapshot = *static_cast<StorageSnapshot*>(opaque);
  snapshot.logicalItemCounts[categoryIndex(StorageCategory::ReadingCache)]++;
}
}  // namespace

bool StorageAnalyzer::walk(const char* rootPath, const FileVisitor fileVisitor,
                           const DirectoryVisitor directoryVisitor, void* context) {
  walkComplete = true;
  const bool opened = walkDirectory(rootPath, 0, fileVisitor, directoryVisitor, context);
  return opened && walkComplete;
}

bool StorageAnalyzer::walkDirectory(const std::string& path, const int depth, const FileVisitor fileVisitor,
                                    const DirectoryVisitor directoryVisitor, void* context) {
  if (depth > MAX_SCAN_DEPTH || path.size() > MAX_SCAN_PATH) {
    walkComplete = false;
    return false;
  }

  auto directory = Storage.open(path.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    walkComplete = false;
    return false;
  }

  directory.rewindDirectory();
  for (auto entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    entry.getName(nameBuffer, sizeof(nameBuffer));
    if (strcmp(nameBuffer, ".") == 0 || strcmp(nameBuffer, "..") == 0) {
      entry.close();
      continue;
    }

    std::string childPath = path == "/" ? "/" : path + "/";
    childPath += nameBuffer;
    const bool isDirectory = entry.isDirectory();
    const uint64_t size = isDirectory ? 0 : entry.fileSize64();
    entry.close();

    if (childPath.size() > MAX_SCAN_PATH) {
      walkComplete = false;
      continue;
    }
    if (isDirectory) {
      if (directoryVisitor) directoryVisitor(childPath, context);
      walkDirectory(childPath, depth + 1, fileVisitor, directoryVisitor, context);
    } else {
      fileVisitor(childPath, size, context);
    }
  }
  directory.close();
  return true;
}

bool StorageAnalyzer::analyze(StorageSnapshot& snapshot, const bool forceSpaceRefresh) {
  snapshot = {};
  auto& result = snapshot.breakdown;
  result = {};
  result.complete = walk("/", accumulateFile, accumulateDirectory, &snapshot);

  for (auto& items : snapshot.largestItems) {
    std::sort(items.begin(), items.end(), [](const StorageItem& lhs, const StorageItem& rhs) {
      if (lhs.size != rhs.size) return lhs.size > rhs.size;
      return lhs.path < rhs.path;
    });
  }

  StorageSpaceInfo space;
  result.spaceValid = Storage.getSpaceInfo(space, forceSpaceRefresh);
  if (!result.spaceValid) return result.complete;

  result.totalBytes = space.totalBytes;
  result.usedBytes = space.usedBytes;
  result.freeBytes = space.totalBytes - space.usedBytes;

  uint64_t recognizedBytes = 0;
  for (size_t i = 0; i < categoryIndex(StorageCategory::Other); i++) recognizedBytes += result.bytes[i];
  const uint64_t scannedOther = result.bytes[categoryIndex(StorageCategory::Other)];
  if (result.usedBytes >= recognizedBytes) {
    result.bytes[categoryIndex(StorageCategory::Other)] = result.usedBytes - recognizedBytes;
    const uint64_t accountedLogical = recognizedBytes + scannedOther;
    result.unaccountedBytes = result.usedBytes > accountedLogical ? result.usedBytes - accountedLogical : 0;
  } else {
    result.complete = false;
  }
  return result.complete;
}
