#include "SubstackSyncEngine.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <ctime>
#include <memory>

#include "network/HttpDownloader.h"
#include "substack/SubstackEpubWriter.h"
#include "substack/SubstackRssParser.h"

namespace {
void addStorageSkip(SubstackSyncSummary& summary, const std::string& publication, SubstackStore::Capacity capacity) {
  if (capacity == SubstackStore::Capacity::Quota) ++summary.skippedQuota;
  else if (capacity == SubstackStore::Capacity::SdReserve) ++summary.skippedReserve;
  auto found = std::find_if(summary.skippedByFeed.begin(), summary.skippedByFeed.end(),
                            [&](const auto& item) { return item.publication == publication; });
  if (found == summary.skippedByFeed.end()) summary.skippedByFeed.push_back({publication, 1});
  else ++found->count;
}
}  // namespace

SubstackSyncSummary SubstackSyncEngine::sync(const ProgressCallback& progress) {
  SubstackSyncSummary summary;
  const uint64_t now = static_cast<uint64_t>(time(nullptr));
  SUBSTACK_STORE.purgeExpiredRead(now);
  uint64_t currentArticleBytes = SUBSTACK_STORE.articleBytes();

  size_t enabledCount = 0;
  for (const auto& feed : SUBSTACK_STORE.feeds())
    if (feed.enabled) ++enabledCount;

  size_t enabledIndex = 0;
  for (const auto& sourceFeed : SUBSTACK_STORE.feeds()) {
    if (!sourceFeed.enabled) continue;
    const std::string feedId = sourceFeed.id;
    const std::string feedUrl = sourceFeed.url;
    std::string publication = sourceFeed.name.empty() ? sourceFeed.url : sourceFeed.name;
    if (progress) progress(enabledIndex++, enabledCount, publication);
    ++summary.feedsAttempted;
    bool itemFailure = false;
    uint32_t feedDownloaded = 0;
    uint32_t feedUpdated = 0;
    uint32_t feedExisting = 0;
    uint32_t feedStorageSkipped = 0;
    bool retryPass = false;

    LOG_DBG("SUBSYNC", "Feed %u/%u before TLS: free=%u max-block=%u", static_cast<unsigned>(enabledIndex),
            static_cast<unsigned>(enabledCount), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));

    auto handleItem = [&](SubstackRssItem&& item) {
      const std::string articlePublication = item.publication.empty() ? publication : item.publication;
      const std::string identity = item.guid.empty() ? item.link + item.title : item.guid;
      const std::string articleId = SubstackStore::stableId(feedId + "\n" + identity);
      SubstackArticle existingArticle;
      const bool hasExisting = SUBSTACK_STORE.loadArticle(articleId, existingArticle);
      const bool missingEpub = hasExisting && !Storage.exists(existingArticle.epubPath.c_str());
      const bool needsContentUpdate =
          hasExisting && (existingArticle.contentVersion < SubstackEpubWriter::CONTENT_VERSION || missingEpub);
      if (missingEpub)
        LOG_ERR("SUBSYNC", "Repairing article with missing EPUB: %s", existingArticle.epubPath.c_str());
      if (hasExisting && !needsContentUpdate) {
        if (!retryPass) {
          ++summary.alreadyDownloaded;
          ++feedExisting;
        }
        return true;
      }
      const uint64_t estimatedBytes = item.htmlBytes + 16ULL * 1024ULL;
      // Missing files are not part of articleBytes(), so they cannot receive
      // quota credit as though their old byte count were still on disk.
      const uint64_t replacedBytes = needsContentUpdate && !missingEpub ? existingArticle.byteSize : 0;
      const auto capacity = SUBSTACK_STORE.capacityFor(estimatedBytes, replacedBytes, currentArticleBytes);
      if (capacity != SubstackStore::Capacity::Available) {
        addStorageSkip(summary, articlePublication, capacity);
        ++feedStorageSkipped;
        return true;  // Deliberately leave it unseen so the next manual sync retries it.
      }

      std::string path;
      uint64_t byteSize = 0;
      if (!SubstackEpubWriter::write(item, articlePublication, articleId, path, byteSize)) {
        LOG_ERR("SUBSYNC", "Could not create EPUB: %s — %s", articlePublication.c_str(), item.title.c_str());
        itemFailure = true;
        return true;
      }
      if (!Storage.exists(path.c_str())) {
        LOG_ERR("SUBSYNC", "EPUB writer returned without a readable file: %s", path.c_str());
        itemFailure = true;
        return true;
      }
      SubstackArticle article;
      article.id = articleId;
      article.feedId = feedId;
      article.publication = articlePublication;
      article.title = std::move(item.title);
      article.author = std::move(item.author);
      article.sourceUrl = std::move(item.link);
      article.epubPath = path;
      article.publishedAt = item.publishedAt;
      article.downloadedAt = now;
      article.byteSize = byteSize;
      article.contentVersion = SubstackEpubWriter::CONTENT_VERSION;
      article.preview = item.preview;
      if (needsContentUpdate) {
        article.downloadedAt = existingArticle.downloadedAt;
        article.readAt = existingArticle.readAt;
        article.read = existingArticle.read;
      }
      if (!SUBSTACK_STORE.saveArticle(article)) {
        LOG_ERR("SUBSYNC", "Could not save article metadata after EPUB write: %s", path.c_str());
        Storage.remove(path.c_str());
        itemFailure = true;
        return true;
      }
      if (needsContentUpdate) {
        ++summary.updated;
        ++feedUpdated;
        if (existingArticle.epubPath != path) Storage.remove(existingArticle.epubPath.c_str());
        if (byteSize > existingArticle.byteSize) summary.bytesAdded += byteSize - existingArticle.byteSize;
        currentArticleBytes = currentArticleBytes - std::min(currentArticleBytes, replacedBytes) + byteSize;
      } else {
        ++summary.downloaded;
        ++feedDownloaded;
        summary.bytesAdded += byteSize;
        currentArticleBytes += byteSize;
      }
      return true;
    };

    // Expat has a meaningful fixed allocation. Constructing it before the
    // socket made that allocation compete with wolfSSL's handshake. Delay it
    // until the first response body bytes, when TLS is already established.
    std::unique_ptr<SubstackRssParser> parser;
    HttpDownloader::DownloadError fetchResult = HttpDownloader::HTTP_ERROR;
    bool emptyFeed = false;
    bool parserFailed = true;
    for (int attempt = 0; attempt < 2; ++attempt) {
      retryPass = attempt > 0;
      parser.reset();
      fetchResult = HttpDownloader::fetchUrlWithResult(
          feedUrl, [&](const uint8_t* data, size_t length) {
            if (!parser) {
              LOG_DBG("SUBSYNC", "TLS complete; creating RSS parser: free=%u max-block=%u",
                      static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
              parser = std::make_unique<SubstackRssParser>(handleItem);
            }
            return parser->write(data, length) == length;
          });
      if (parser) {
        parser->flush();
        if (!parser->publication().empty()) publication = parser->publication();
        LOG_INF("SUBSYNC", "Feed parsed: %s attempt=%u items=%u emitted=%u malformed=%u new=%u updated=%u existing=%u storage-skipped=%u",
                publication.c_str(), static_cast<unsigned>(attempt + 1),
                static_cast<unsigned>(parser->itemsSeen()), static_cast<unsigned>(parser->itemsEmitted()),
                static_cast<unsigned>(parser->itemsSkipped()), static_cast<unsigned>(feedDownloaded),
                static_cast<unsigned>(feedUpdated), static_cast<unsigned>(feedExisting),
                static_cast<unsigned>(feedStorageSkipped));
      }
      emptyFeed = parser && (parser->itemsSeen() == 0 || parser->itemsEmitted() == 0);
      parserFailed = !parser || parser->error() || parser->aborted() || emptyFeed;
      const bool retryIncomplete = attempt == 0 && fetchResult == HttpDownloader::UNKNOWN_LENGTH_RESPONSE &&
                                   parserFailed && !itemFailure && feedStorageSkipped == 0;
      if (!retryIncomplete) break;
      LOG_ERR("SUBSYNC", "Retrying truncated unknown-length feed once: %s", feedUrl.c_str());
      parser.reset();
      delay(50);
    }
    retryPass = false;

    SubstackFeed* feed = SUBSTACK_STORE.mutableFeedById(feedId);
    if (feed) {
      if (parser && feed->name.empty() && !parser->publication().empty()) feed->name = parser->publication();
      feed->lastFetchAt = now;
    }
    const bool lowMemory = fetchResult == HttpDownloader::TLS_MEMORY_ERROR;
    // Some Substack/CDN responses have no Content-Length and close the TLS
    // stream without enough transport-level framing for wolfSSL to mark the
    // response complete. Expat's final parse is a stronger integrity check for
    // this structured payload: accept only a complete, non-empty XML document.
    const bool parserValidatedUnknownLength =
        fetchResult == HttpDownloader::UNKNOWN_LENGTH_RESPONSE && !parserFailed;
    if ((fetchResult != HttpDownloader::OK && !parserValidatedUnknownLength) || parserFailed || itemFailure) {
      ++summary.feedsFailed;
      if (lowMemory) {
        ++summary.lowMemoryFailures;
        if (feed) feed->lastError = "Not enough memory for a secure connection";
      } else if (feed) {
        feed->lastError = fetchResult != HttpDownloader::OK
                              ? "Secure feed connection failed"
                              : emptyFeed ? "Feed contained no downloadable articles"
                              : parserFailed ? "Feed response could not be parsed"
                                             : "One or more articles could not be saved";
      }
      if (summary.lastError.empty()) summary.lastError = feed ? feed->lastError : "Feed failed";
      LOG_ERR("SUBSYNC", "Feed failed: %s (result=%d parser=%s item-save=%s free=%u max-block=%u)", feedUrl.c_str(),
              static_cast<int>(fetchResult), parserFailed ? "failed" : "ok", itemFailure ? "failed" : "ok",
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    } else if (feed) {
      if (parserValidatedUnknownLength)
        LOG_INF("SUBSYNC", "Accepted complete XML from unknown-length response: %s", feedUrl.c_str());
      feed->lastError.clear();
    }
    SUBSTACK_STORE.save();
    parser.reset();
    LOG_DBG("SUBSYNC", "Feed cleanup complete: free=%u max-block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    if (lowMemory) break;  // Remaining feeds would hit the same preflight result.
  }

  summary.finishedAt = static_cast<uint64_t>(time(nullptr));
  const bool allFeedsSuccessful = summary.feedsFailed == 0 && summary.feedsAttempted == enabledCount;
  SUBSTACK_STORE.commitSync(summary, allFeedsSuccessful);
  return summary;
}
