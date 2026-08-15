#include "SubstackStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <ctime>

namespace {
constexpr const char* ROOT = "/.crosspoint/substack";
constexpr const char* ARTICLE_META_DIR = "/.crosspoint/substack/articles";
constexpr const char* ARTICLE_INDEX_PATH = "/.crosspoint/substack/article-index.jsonl";
constexpr const char* ARTICLE_INDEX_HEADER = "# amy-substack-index-v1";

std::string trim(std::string value) {
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
  return value;
}

bool hasJsonSuffix(const std::string& value) {
  return value.size() > 5 && value.compare(value.size() - 5, 5, ".json") == 0;
}

uint64_t jsonU64(JsonVariantConst value) {
  if (value.is<uint64_t>()) return value.as<uint64_t>();
  if (value.is<const char*>()) return strtoull(value.as<const char*>(), nullptr, 10);
  return 0;
}
}  // namespace

SubstackStore& SubstackStore::getInstance() {
  static SubstackStore instance;
  return instance;
}

const char* SubstackStore::statePath() { return "/.crosspoint/substack/state.json"; }

std::string SubstackStore::articleMetadataPath(const std::string& id) {
  return std::string(ARTICLE_META_DIR) + "/" + id + ".json";
}

bool SubstackStore::writeJsonAtomic(const std::string& path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(ROOT);
  Storage.mkdir(ARTICLE_META_DIR);

  String body;
  serializeJson(doc, body);
  const std::string temp = path + ".tmp";
  const std::string backup = path + ".bak";
  Storage.remove(temp.c_str());
  if (!Storage.writeFile(temp.c_str(), body)) return false;

  Storage.remove(backup.c_str());
  const bool hadOriginal = Storage.exists(path.c_str());
  if (hadOriginal && !Storage.rename(path.c_str(), backup.c_str())) {
    Storage.remove(temp.c_str());
    return false;
  }
  if (!Storage.rename(temp.c_str(), path.c_str())) {
    if (hadOriginal) Storage.rename(backup.c_str(), path.c_str());
    Storage.remove(temp.c_str());
    return false;
  }
  Storage.remove(backup.c_str());
  return true;
}

bool SubstackStore::readJsonRecovering(const std::string& path, JsonDocument& doc) {
  const std::string candidates[] = {path, path + ".bak", path + ".tmp"};
  for (const auto& candidate : candidates) {
    if (!Storage.exists(candidate.c_str())) continue;
    const String body = Storage.readFile(candidate.c_str());
    if (body.isEmpty()) continue;
    doc.clear();
    if (!deserializeJson(doc, body)) return true;
  }
  return false;
}

bool SubstackStore::load() {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(ROOT);
  Storage.mkdir(ARTICLE_META_DIR);

  JsonDocument doc;
  if (!readJsonRecovering(statePath(), doc)) {
    LOG_INF("SUBSTORE", "No saved Substack state; starting empty");
    return false;
  }

  feedList.clear();
  const JsonArrayConst feedsJson = doc["feeds"].as<JsonArrayConst>();
  feedList.reserve(std::min(feedsJson.size(), MAX_FEEDS));
  for (const JsonObjectConst item : feedsJson) {
    if (feedList.size() >= MAX_FEEDS) break;
    SubstackFeed feed;
    feed.id = item["id"] | "";
    feed.name = item["name"] | "";
    feed.url = item["url"] | "";
    feed.enabled = item["enabled"] | true;
    feed.lastFetchAt = jsonU64(item["lastFetchAt"]);
    feed.lastError = item["lastError"] | "";
    if (!feed.url.empty()) feedList.push_back(std::move(feed));
  }

  const JsonObjectConst sync = doc["lastSync"].as<JsonObjectConst>();
  syncSummary = {};
  syncSummary.finishedAt = jsonU64(sync["finishedAt"]);
  syncSummary.feedsAttempted = sync["feedsAttempted"] | 0;
  syncSummary.feedsFailed = sync["feedsFailed"] | 0;
  syncSummary.lowMemoryFailures = sync["lowMemoryFailures"] | 0;
  syncSummary.downloaded = sync["downloaded"] | 0;
  syncSummary.updated = sync["updated"] | 0;
  syncSummary.alreadyDownloaded = sync["alreadyDownloaded"] | 0;
  syncSummary.skippedQuota = sync["skippedQuota"] | 0;
  syncSummary.skippedReserve = sync["skippedReserve"] | 0;
  syncSummary.bytesAdded = jsonU64(sync["bytesAdded"]);
  syncSummary.lastError = sync["lastError"] | "";
  for (const JsonObjectConst skipped : sync["skippedByFeed"].as<JsonArrayConst>()) {
    SubstackFeedSkip entry;
    entry.publication = skipped["publication"] | "";
    entry.count = skipped["count"] | 0;
    if (entry.count > 0) syncSummary.skippedByFeed.push_back(std::move(entry));
  }
  LOG_INF("SUBSTORE", "Loaded: feeds=%u last-attempted=%u failed=%u downloaded=%u existing=%u quota=%u reserve=%u",
          static_cast<unsigned>(feedList.size()), static_cast<unsigned>(syncSummary.feedsAttempted),
          static_cast<unsigned>(syncSummary.feedsFailed), static_cast<unsigned>(syncSummary.downloaded),
          static_cast<unsigned>(syncSummary.alreadyDownloaded), static_cast<unsigned>(syncSummary.skippedQuota),
          static_cast<unsigned>(syncSummary.skippedReserve));
  return true;
}

bool SubstackStore::save() const {
  JsonDocument doc;
  JsonArray feedsJson = doc["feeds"].to<JsonArray>();
  for (const auto& feed : feedList) {
    JsonObject item = feedsJson.add<JsonObject>();
    item["id"] = feed.id;
    item["name"] = feed.name;
    item["url"] = feed.url;
    item["enabled"] = feed.enabled;
    item["lastFetchAt"] = feed.lastFetchAt;
    item["lastError"] = feed.lastError;
  }

  JsonObject sync = doc["lastSync"].to<JsonObject>();
  sync["finishedAt"] = syncSummary.finishedAt;
  sync["feedsAttempted"] = syncSummary.feedsAttempted;
  sync["feedsFailed"] = syncSummary.feedsFailed;
  sync["lowMemoryFailures"] = syncSummary.lowMemoryFailures;
  sync["downloaded"] = syncSummary.downloaded;
  sync["updated"] = syncSummary.updated;
  sync["alreadyDownloaded"] = syncSummary.alreadyDownloaded;
  sync["skippedQuota"] = syncSummary.skippedQuota;
  sync["skippedReserve"] = syncSummary.skippedReserve;
  sync["bytesAdded"] = syncSummary.bytesAdded;
  sync["lastError"] = syncSummary.lastError;
  JsonArray skipped = sync["skippedByFeed"].to<JsonArray>();
  for (const auto& item : syncSummary.skippedByFeed) {
    JsonObject value = skipped.add<JsonObject>();
    value["publication"] = item.publication;
    value["count"] = item.count;
  }
  return writeJsonAtomic(statePath(), doc);
}

std::string SubstackStore::normalizeFeedUrl(const std::string& input) {
  std::string url = trim(input);
  if (url.empty()) return {};
  if (url.find("://") == std::string::npos) url = "https://" + url;
  if (url.rfind("https://", 0) != 0) return {};
  const size_t fragment = url.find_first_of("?#");
  if (fragment != std::string::npos) url.erase(fragment);
  while (url.size() > 8 && url.back() == '/') url.pop_back();
  if (url.size() < 5 || url.compare(url.size() - 5, 5, "/feed") != 0) url += "/feed";
  return url;
}

std::string SubstackStore::stableId(const std::string& value) {
  unsigned char digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest, 0);
  char output[33];
  for (size_t i = 0; i < 16; ++i) snprintf(output + i * 2, 3, "%02x", digest[i]);
  output[32] = '\0';
  return output;
}

bool SubstackStore::addFeed(std::string url, std::string name) {
  if (feedList.size() >= MAX_FEEDS) return false;
  url = normalizeFeedUrl(url);
  if (url.empty()) return false;
  if (std::any_of(feedList.begin(), feedList.end(), [&](const auto& feed) { return feed.url == url; })) return false;
  SubstackFeed feed;
  feed.id = stableId(url);
  feed.url = std::move(url);
  feed.name = trim(std::move(name));
  feedList.push_back(std::move(feed));
  return save();
}

size_t SubstackStore::addFeedsFromText(const std::string& text) {
  size_t added = 0;
  // OPML exports put each feed URL in an xmlUrl attribute. Accept those directly
  // so a user can move a subscription list without editing the XML by hand.
  size_t opmlAt = 0;
  while ((opmlAt = text.find("xmlUrl=", opmlAt)) != std::string::npos) {
    opmlAt += 7;
    if (opmlAt >= text.size() || (text[opmlAt] != '\"' && text[opmlAt] != '\'')) continue;
    const char quote = text[opmlAt++];
    const size_t end = text.find(quote, opmlAt);
    if (end == std::string::npos) break;
    if (addFeed(text.substr(opmlAt, end - opmlAt))) ++added;
    opmlAt = end + 1;
  }
  if (added > 0 || text.find("<opml") != std::string::npos) return added;

  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find_first_of("\r\n,", start);
    if (end == std::string::npos) end = text.size();
    const std::string candidate = trim(text.substr(start, end - start));
    if (!candidate.empty() && addFeed(candidate)) ++added;
    start = end + 1;
  }
  return added;
}

bool SubstackStore::updateFeed(size_t index, const std::string& url, const std::string& name, bool enabled) {
  if (index >= feedList.size()) return false;
  const std::string normalized = normalizeFeedUrl(url);
  if (normalized.empty()) return false;
  for (size_t i = 0; i < feedList.size(); ++i)
    if (i != index && feedList[i].url == normalized) return false;
  if (feedList[index].url != normalized) {
    feedList[index].url = normalized;
    feedList[index].id = stableId(normalized);
    feedList[index].lastFetchAt = 0;
    feedList[index].lastError.clear();
  }
  feedList[index].name = trim(name);
  feedList[index].enabled = enabled;
  return save();
}

bool SubstackStore::removeFeed(size_t index) {
  if (index >= feedList.size()) return false;
  feedList.erase(feedList.begin() + static_cast<ptrdiff_t>(index));
  return save();
}

SubstackFeed* SubstackStore::mutableFeedById(const std::string& id) {
  const auto it = std::find_if(feedList.begin(), feedList.end(), [&](const auto& feed) { return feed.id == id; });
  return it == feedList.end() ? nullptr : &*it;
}

bool SubstackStore::articleExists(const std::string& id) const {
  return Storage.exists(articleMetadataPath(id).c_str());
}

void SubstackStore::articleToJson(const SubstackArticle& article, JsonDocument& doc) {
  doc["id"] = article.id;
  doc["feedId"] = article.feedId;
  doc["publication"] = article.publication;
  doc["title"] = article.title;
  doc["author"] = article.author;
  doc["sourceUrl"] = article.sourceUrl;
  doc["epubPath"] = article.epubPath;
  doc["publishedAt"] = article.publishedAt;
  doc["downloadedAt"] = article.downloadedAt;
  doc["readAt"] = article.readAt;
  doc["byteSize"] = article.byteSize;
  doc["contentVersion"] = article.contentVersion;
  doc["preview"] = article.preview;
  doc["read"] = article.read;
}

bool SubstackStore::articleFromJson(JsonVariantConst doc, SubstackArticle& article) {
  article = {};
  article.id = doc["id"] | "";
  article.feedId = doc["feedId"] | "";
  article.publication = doc["publication"] | "";
  article.title = doc["title"] | "";
  article.author = doc["author"] | "";
  article.sourceUrl = doc["sourceUrl"] | "";
  article.epubPath = doc["epubPath"] | "";
  article.publishedAt = jsonU64(doc["publishedAt"]);
  article.downloadedAt = jsonU64(doc["downloadedAt"]);
  article.readAt = jsonU64(doc["readAt"]);
  article.byteSize = jsonU64(doc["byteSize"]);
  article.contentVersion = doc["contentVersion"] | 0;
  article.preview = doc["preview"] | false;
  article.read = doc["read"] | false;
  return !article.id.empty() && !article.epubPath.empty();
}

bool SubstackStore::saveArticle(const SubstackArticle& article) const {
  // Invalidate before the authoritative metadata write so a reset at any
  // point can never leave a stale fast index claiming to be current.
  invalidateArticleIndex();
  JsonDocument doc;
  articleToJson(article, doc);
  return writeJsonAtomic(articleMetadataPath(article.id), doc);
}

bool SubstackStore::loadArticle(const std::string& id, SubstackArticle& article) const {
  JsonDocument doc;
  return readJsonRecovering(articleMetadataPath(id), doc) && articleFromJson(doc.as<JsonVariantConst>(), article);
}

std::vector<SubstackArticle> SubstackStore::listArticles(size_t limit, uint64_t* totalBytes,
                                                         uint32_t* unreadCount) const {
  limit = std::min(limit, MAX_VISIBLE_ARTICLES);
  uint64_t indexedBytes = 0;
  uint32_t indexedUnread = 0;
  std::vector<SubstackArticle> result;
  result.reserve(limit);
  if (readArticleIndex(limit, result, &indexedBytes, &indexedUnread)) {
    if (totalBytes) *totalBytes = indexedBytes;
    if (unreadCount) *unreadCount = indexedUnread;
    LOG_INF("SUBSTORE", "Article index cache: returned=%u limit=%u", static_cast<unsigned>(result.size()),
            static_cast<unsigned>(limit));
    return result;
  }
  const auto before = [](const auto& a, const auto& b) {
    if (a.read != b.read) return !a.read;
    if (a.publishedAt != b.publishedAt) return a.publishedAt > b.publishedAt;
    return a.downloadedAt > b.downloadedAt;
  };
  HalFile dir = Storage.open(ARTICLE_META_DIR);
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("SUBSTORE", "Article metadata directory is unavailable: %s", ARTICLE_META_DIR);
    return result;
  }
  char name[96];
  uint32_t metadataFiles = 0;
  uint32_t invalidMetadata = 0;
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    file.close();
    const std::string filename = name;
    if (!hasJsonSuffix(filename)) continue;
    ++metadataFiles;
    SubstackArticle article;
    if (!loadArticle(filename.substr(0, filename.size() - 5), article)) {
      ++invalidMetadata;
      continue;
    }
    indexedBytes += article.byteSize;
    if (!article.read) ++indexedUnread;
    if (limit == 0) continue;
    if (result.size() < limit) {
      result.push_back(std::move(article));
    } else {
      const auto worst = std::max_element(result.begin(), result.end(), before);
      if (before(article, *worst)) *worst = std::move(article);
    }
  }
  dir.close();
  std::sort(result.begin(), result.end(), before);
  if (totalBytes) *totalBytes = indexedBytes;
  if (unreadCount) *unreadCount = indexedUnread;
  if (limit == MAX_VISIBLE_ARTICLES && !writeArticleIndex(result, indexedBytes, indexedUnread))
    LOG_ERR("SUBSTORE", "Could not write article index cache");
  LOG_INF("SUBSTORE", "Article index: metadata=%u valid-returned=%u invalid=%u limit=%u",
          static_cast<unsigned>(metadataFiles), static_cast<unsigned>(result.size()),
          static_cast<unsigned>(invalidMetadata), static_cast<unsigned>(limit));
  return result;
}

bool SubstackStore::findArticleByPath(const std::string& epubPath, SubstackArticle& article) const {
  HalFile dir = Storage.open(ARTICLE_META_DIR);
  if (!dir || !dir.isDirectory()) return false;
  char name[96];
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    file.close();
    const std::string filename = name;
    if (!hasJsonSuffix(filename)) continue;
    SubstackArticle candidate;
    if (loadArticle(filename.substr(0, filename.size() - 5), candidate) && candidate.epubPath == epubPath) {
      article = std::move(candidate);
      dir.close();
      return true;
    }
  }
  dir.close();
  return false;
}

bool SubstackStore::markReadByPath(const std::string& epubPath, const bool read) {
  SubstackArticle article;
  if (!findArticleByPath(epubPath, article)) return false;
  if (article.read == read) return true;
  article.read = read;
  article.readAt = read ? static_cast<uint64_t>(time(nullptr)) : 0;
  return saveArticle(article);
}

bool SubstackStore::deleteArticle(const std::string& id, std::string* deletedPath) {
  SubstackArticle article;
  if (!loadArticle(id, article)) return false;
  if (deletedPath) *deletedPath = article.epubPath;
  invalidateArticleIndex();
  if (Storage.exists(article.epubPath.c_str()) && !Storage.remove(article.epubPath.c_str())) return false;
  const std::string meta = articleMetadataPath(id);
  const bool removed = Storage.remove(meta.c_str());
  Storage.remove((meta + ".bak").c_str());
  Storage.remove((meta + ".tmp").c_str());
  return removed;
}

void SubstackStore::invalidateArticleIndex() {
  Storage.remove(ARTICLE_INDEX_PATH);
  Storage.remove((std::string(ARTICLE_INDEX_PATH) + ".tmp").c_str());
  Storage.remove((std::string(ARTICLE_INDEX_PATH) + ".bak").c_str());
}

bool SubstackStore::readArticleIndex(const size_t limit, std::vector<SubstackArticle>& articles,
                                     uint64_t* totalBytes, uint32_t* unreadCount) {
  HalFile file;
  if (!Storage.openFileForRead("SUBSTORE", ARTICLE_INDEX_PATH, file)) return false;
  bool headerSeen = false;
  bool failed = false;
  bool complete = false;
  uint32_t recordCount = 0;
  std::string line;
  line.reserve(1024);

  const auto processLine = [&]() {
    if (!headerSeen) {
      headerSeen = line == ARTICLE_INDEX_HEADER;
      return headerSeen;
    }
    if (line.rfind("# end ", 0) == 0) {
      unsigned expected = 0;
      unsigned unread = 0;
      unsigned long long bytes = 0;
      complete = sscanf(line.c_str(), "# end %u %llu %u", &expected, &bytes, &unread) == 3 &&
                 expected == recordCount;
      if (complete) {
        if (totalBytes) *totalBytes = static_cast<uint64_t>(bytes);
        if (unreadCount) *unreadCount = static_cast<uint32_t>(unread);
      }
      return complete;
    }
    if (line.empty()) return true;
    JsonDocument doc;
    if (deserializeJson(doc, line)) return false;
    SubstackArticle article;
    if (!articleFromJson(doc.as<JsonVariantConst>(), article)) return false;
    ++recordCount;
    if (articles.size() < limit) articles.push_back(std::move(article));
    return true;
  };

  uint8_t buffer[1024];
  while (!failed && !complete) {
    const int count = file.read(buffer, sizeof(buffer));
    if (count <= 0) break;
    for (int i = 0; i < count && !failed && !complete; ++i) {
      const char value = static_cast<char>(buffer[i]);
      if (value == '\r') continue;
      if (value == '\n') {
        failed = !processLine();
        line.clear();
      } else if (line.size() < 4096) {
        line.push_back(value);
      } else {
        failed = true;
      }
    }
  }
  file.close();
  if (complete && !failed) return true;
  articles.clear();
  invalidateArticleIndex();
  return false;
}

bool SubstackStore::writeArticleIndex(const std::vector<SubstackArticle>& articles, const uint64_t totalBytes,
                                      const uint32_t unreadCount) {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(ROOT);
  const std::string temp = std::string(ARTICLE_INDEX_PATH) + ".tmp";
  const std::string backup = std::string(ARTICLE_INDEX_PATH) + ".bak";
  Storage.remove(temp.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("SUBSTORE", temp, file)) return false;
  const std::string header = std::string(ARTICLE_INDEX_HEADER) + "\n";
  bool ok = file.write(header.data(), header.size()) == header.size();
  for (const auto& article : articles) {
    if (!ok) break;
    JsonDocument doc;
    articleToJson(article, doc);
    String body;
    serializeJson(doc, body);
    ok = file.write(body.c_str(), body.length()) == body.length() && file.write(static_cast<uint8_t>('\n')) == 1;
  }
  if (ok) {
    const std::string footer = "# end " + std::to_string(articles.size()) + " " + std::to_string(totalBytes) + " " +
                               std::to_string(unreadCount) + "\n";
    ok = file.write(footer.data(), footer.size()) == footer.size();
  }
  file.flush();
  file.close();
  if (!ok) {
    Storage.remove(temp.c_str());
    return false;
  }

  Storage.remove(backup.c_str());
  const bool hadOriginal = Storage.exists(ARTICLE_INDEX_PATH);
  if (hadOriginal && !Storage.rename(ARTICLE_INDEX_PATH, backup.c_str())) {
    Storage.remove(temp.c_str());
    return false;
  }
  if (!Storage.rename(temp.c_str(), ARTICLE_INDEX_PATH)) {
    if (hadOriginal) Storage.rename(backup.c_str(), ARTICLE_INDEX_PATH);
    Storage.remove(temp.c_str());
    return false;
  }
  Storage.remove(backup.c_str());
  return true;
}

uint64_t SubstackStore::articleBytes() const {
  std::vector<SubstackArticle> unused;
  uint64_t indexedBytes = 0;
  uint32_t indexedUnread = 0;
  if (readArticleIndex(0, unused, &indexedBytes, &indexedUnread)) return indexedBytes;
  uint64_t bytes = 0;
  HalFile dir = Storage.open(ARTICLE_META_DIR);
  if (!dir || !dir.isDirectory()) return 0;
  char name[96];
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    file.close();
    const std::string filename = name;
    if (!hasJsonSuffix(filename)) continue;
    SubstackArticle article;
    if (loadArticle(filename.substr(0, filename.size() - 5), article)) bytes += article.byteSize;
  }
  dir.close();
  return bytes;
}

uint32_t SubstackStore::unreadCount() const {
  uint32_t count = 0;
  HalFile dir = Storage.open(ARTICLE_META_DIR);
  if (!dir || !dir.isDirectory()) return 0;
  char name[96];
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    file.close();
    const std::string filename = name;
    if (!hasJsonSuffix(filename)) continue;
    SubstackArticle article;
    if (loadArticle(filename.substr(0, filename.size() - 5), article) && !article.read) ++count;
  }
  dir.close();
  return count;
}

uint32_t SubstackStore::purgeExpiredRead(const uint64_t now) {
  if (now == 0) return 0;
  constexpr uint64_t retention = static_cast<uint64_t>(READ_RETENTION_DAYS) * 24ULL * 60ULL * 60ULL;
  std::vector<std::string> expiredIds;
  HalFile dir = Storage.open(ARTICLE_META_DIR);
  if (dir && dir.isDirectory()) {
    char name[96];
    for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
      file.getName(name, sizeof(name));
      file.close();
      const std::string filename = name;
      if (!hasJsonSuffix(filename)) continue;
      SubstackArticle article;
      if (loadArticle(filename.substr(0, filename.size() - 5), article) && article.read && article.readAt > 0 &&
          now > article.readAt && now - article.readAt >= retention) {
        expiredIds.push_back(std::move(article.id));
      }
    }
    dir.close();
  }
  uint32_t removed = 0;
  for (const auto& id : expiredIds)
    if (deleteArticle(id)) ++removed;
  return removed;
}

uint64_t SubstackStore::sdReserveBytes() const {
  return std::max<uint64_t>(MIN_FREE_RESERVE_BYTES, Storage.totalBytes() / 20ULL);
}

SubstackStore::Capacity SubstackStore::capacityFor(const uint64_t estimatedBytes,
                                                   const uint64_t quotaCreditBytes,
                                                   const uint64_t knownArticleBytes) const {
  const uint64_t currentBytes = knownArticleBytes == std::numeric_limits<uint64_t>::max()
                                    ? articleBytes()
                                    : knownArticleBytes;
  const uint64_t quotaBytes = quotaCreditBytes >= currentBytes ? 0 : currentBytes - quotaCreditBytes;
  if (quotaBytes + estimatedBytes > ARTICLE_QUOTA_BYTES) return Capacity::Quota;
  const uint64_t free = Storage.freeBytes();
  const uint64_t reserve = sdReserveBytes();
  if (free <= reserve || estimatedBytes > free - reserve) return Capacity::SdReserve;
  return Capacity::Available;
}

void SubstackStore::commitSync(const SubstackSyncSummary& summary, const bool allFeedsSuccessful) {
  // A prior storage warning only clears after a complete, storage-unblocked sync.
  if (!summary.storageBlocked() && !allFeedsSuccessful && syncSummary.storageBlocked()) {
    SubstackSyncSummary merged = summary;
    merged.skippedQuota = syncSummary.skippedQuota;
    merged.skippedReserve = syncSummary.skippedReserve;
    merged.skippedByFeed = syncSummary.skippedByFeed;
    syncSummary = std::move(merged);
  } else {
    syncSummary = summary;
  }
  save();
}
