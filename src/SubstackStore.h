#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

struct SubstackFeed {
  std::string id;
  std::string name;
  std::string url;
  bool enabled = true;
  uint64_t lastFetchAt = 0;
  std::string lastError;
};

struct SubstackArticle {
  std::string id;
  std::string feedId;
  std::string publication;
  std::string title;
  std::string author;
  std::string sourceUrl;
  std::string epubPath;
  uint64_t publishedAt = 0;
  uint64_t downloadedAt = 0;
  uint64_t readAt = 0;
  uint64_t byteSize = 0;
  uint32_t contentVersion = 0;
  bool preview = false;
  bool read = false;
};

// The article menu only needs display fields and a stable ID. Keep these rows
// fixed-size so loading one screen never allocates a vector of full metadata
// objects (or a collection of heap-backed strings).
struct SubstackArticleListItem {
  char id[33] = {};
  char title[193] = {};
  char publication[97] = {};
  uint64_t publishedAt = 0;
  bool preview = false;
  bool read = false;
};

struct SubstackFeedSkip {
  std::string publication;
  uint32_t count = 0;
};

struct SubstackSyncSummary {
  uint64_t finishedAt = 0;
  uint32_t feedsAttempted = 0;
  uint32_t feedsFailed = 0;
  uint32_t lowMemoryFailures = 0;
  uint32_t downloaded = 0;
  uint32_t updated = 0;
  uint32_t alreadyDownloaded = 0;
  uint32_t skippedQuota = 0;
  uint32_t skippedReserve = 0;
  uint64_t bytesAdded = 0;
  std::string lastError;
  std::vector<SubstackFeedSkip> skippedByFeed;

  uint32_t skippedStorage() const { return skippedQuota + skippedReserve; }
  bool storageBlocked() const { return skippedStorage() > 0; }
};

class SubstackStore {
 public:
  static constexpr uint64_t ARTICLE_QUOTA_BYTES = 128ULL * 1024ULL * 1024ULL;
  static constexpr uint64_t MIN_FREE_RESERVE_BYTES = 64ULL * 1024ULL * 1024ULL;
  static constexpr uint32_t READ_RETENTION_DAYS = 30;
  static constexpr size_t MAX_FEEDS = 64;
  static constexpr size_t MAX_VISIBLE_ARTICLES = 256;

  enum class Capacity { Available, Quota, SdReserve };
  enum class ArticleListResult { Ok, LowMemory, StorageError };

  static SubstackStore& getInstance();

  bool load();
  bool save() const;

  const std::vector<SubstackFeed>& feeds() const { return feedList; }
  const SubstackSyncSummary& lastSync() const { return syncSummary; }
  bool addFeed(std::string url, std::string name = {});
  size_t addFeedsFromText(const std::string& text);
  bool updateFeed(size_t index, const std::string& url, const std::string& name, bool enabled);
  bool removeFeed(size_t index);
  SubstackFeed* mutableFeedById(const std::string& id);

  bool articleExists(const std::string& id) const;
  bool saveArticle(const SubstackArticle& article) const;
  bool loadArticle(const std::string& id, SubstackArticle& article) const;
  ArticleListResult listArticlePage(size_t offset, SubstackArticleListItem* items, size_t capacity,
                                    size_t* loadedCount, size_t* totalCount, uint64_t* totalBytes = nullptr,
                                    uint32_t* unreadCount = nullptr) const;
  bool findArticleByPath(const std::string& epubPath, SubstackArticle& article) const;
  bool markReadByPath(const std::string& epubPath, bool read);
  bool deleteArticle(const std::string& id, std::string* deletedPath = nullptr);

  uint64_t articleBytes() const;
  uint32_t unreadCount() const;
  uint32_t purgeExpiredRead(uint64_t now);
  Capacity capacityFor(uint64_t estimatedBytes, uint64_t quotaCreditBytes = 0,
                       uint64_t knownArticleBytes = std::numeric_limits<uint64_t>::max()) const;
  uint64_t sdReserveBytes() const;

  void commitSync(const SubstackSyncSummary& summary, bool allFeedsSuccessful);

  static std::string normalizeFeedUrl(const std::string& input);
  static std::string stableId(const std::string& value);

 private:
  std::vector<SubstackFeed> feedList;
  SubstackSyncSummary syncSummary;

  SubstackStore() = default;
  static const char* statePath();
  static std::string articleMetadataPath(const std::string& id);
  static bool writeJsonAtomic(const std::string& path, const JsonDocument& doc);
  static bool readJsonRecovering(const std::string& path, JsonDocument& doc);
  static void invalidateArticleIndex();
  static bool readArticleIndexPage(size_t offset, SubstackArticleListItem* items, size_t capacity,
                                   size_t* loadedCount, size_t* totalCount, uint64_t* totalBytes,
                                   uint32_t* unreadCount);
  ArticleListResult rebuildArticleIndex() const;
  static void articleToJson(const SubstackArticle& article, JsonDocument& doc);
  static bool articleFromJson(JsonVariantConst doc, SubstackArticle& article);
};

#define SUBSTACK_STORE SubstackStore::getInstance()
