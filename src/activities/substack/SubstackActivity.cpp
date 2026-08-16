#include "SubstackActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>
#include <new>

#include "RecentBooksStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "substack/SubstackSyncEngine.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr int COMMAND_COUNT = 2;

std::string storageLabel(const uint64_t articleBytes) {
  const uint64_t usedMiB = articleBytes / (1024ULL * 1024ULL);
  char value[48];
  snprintf(value, sizeof(value), "%llu / 128 MiB", static_cast<unsigned long long>(usedMiB));
  return value;
}
}  // namespace

void SubstackActivity::onEnter() {
  Activity::onEnter();
  reload();
  const auto& warning = SUBSTACK_STORE.lastSync();
  if (warning.storageBlocked()) {
    std::vector<std::string> lines;
    char totals[72];
    snprintf(totals, sizeof(totals), "Quota: %u · SD reserve: %u", static_cast<unsigned>(warning.skippedQuota),
             static_cast<unsigned>(warning.skippedReserve));
    lines.emplace_back(totals);
    for (const auto& feed : warning.skippedByFeed) {
      if (lines.size() >= 5) break;
      lines.push_back(feed.publication + " — " + std::to_string(feed.count) + " skipped");
    }
    lines.emplace_back("Close");
    const char* choices[6];
    for (size_t i = 0; i < lines.size(); ++i) choices[i] = lines[i].c_str();
    popup.show("Storage limit reached", choices, static_cast<int>(lines.size()), 0, [](int) {});
  }
  requestUpdate();
}

void SubstackActivity::onExit() {
  articles.reset();
  loadedArticleCount = 0;
  Activity::onExit();
}

size_t SubstackActivity::pageCapacity() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return static_cast<size_t>(std::max(1, GUI.getListPageItems(contentHeight, true)));
}

size_t SubstackActivity::pageOffsetForSelection() const {
  const size_t capacity = pageCapacity();
  const size_t listPageStart = static_cast<size_t>(std::max(0, selectedIndex)) / capacity * capacity;
  return listPageStart > static_cast<size_t>(COMMAND_COUNT) ? listPageStart - COMMAND_COUNT : 0;
}

void SubstackActivity::reload() {
  articles.reset();
  loadedArticleCount = 0;
  totalArticleCount = 0;
  visibleArticleBytes = 0;
  visibleUnreadCount = 0;
  const size_t capacity = pageCapacity();
  articles.reset(new (std::nothrow) SubstackArticleListItem[capacity]);
  if (!articles) {
    menuLoadResult = SubstackStore::ArticleListResult::LowMemory;
    selectedIndex = std::min(selectedIndex, COMMAND_COUNT - 1);
    LOG_ERR("SUBSTACK", "Article menu page allocation failed: need=%u max-block=%u",
            static_cast<unsigned>(capacity * sizeof(SubstackArticleListItem)),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return;
  }

  articlePageOffset = pageOffsetForSelection();
  menuLoadResult = SUBSTACK_STORE.listArticlePage(articlePageOffset, articles.get(), capacity,
                                                   &loadedArticleCount, &totalArticleCount,
                                                   &visibleArticleBytes, &visibleUnreadCount);
  if (menuLoadResult != SubstackStore::ArticleListResult::Ok) {
    articles.reset();
    loadedArticleCount = 0;
    totalArticleCount = 0;
    selectedIndex = std::min(selectedIndex, COMMAND_COUNT - 1);
    return;
  }

  const int count = COMMAND_COUNT + static_cast<int>(totalArticleCount);
  selectedIndex = std::clamp(selectedIndex, 0, std::max(0, count - 1));
  const size_t correctedOffset = pageOffsetForSelection();
  if (correctedOffset != articlePageOffset) {
    articlePageOffset = correctedOffset;
    menuLoadResult = SUBSTACK_STORE.listArticlePage(articlePageOffset, articles.get(), capacity,
                                                     &loadedArticleCount, &totalArticleCount,
                                                     &visibleArticleBytes, &visibleUnreadCount);
  }
  LOG_DBG("SUBSTACK", "Reloaded article page: offset=%u loaded=%u total=%u result=%d",
          static_cast<unsigned>(articlePageOffset), static_cast<unsigned>(loadedArticleCount),
          static_cast<unsigned>(totalArticleCount), static_cast<int>(menuLoadResult));
}

void SubstackActivity::ensureSelectedPageLoaded() {
  if (selectedIndex < COMMAND_COUNT) return;
  const size_t articleIndex = static_cast<size_t>(selectedIndex - COMMAND_COUNT);
  if (articles && articleIndex >= articlePageOffset && articleIndex < articlePageOffset + loadedArticleCount) return;
  reload();
}

const SubstackArticleListItem* SubstackActivity::rowForListIndex(const int index) const {
  if (index < COMMAND_COUNT || !articles) return nullptr;
  const size_t articleIndex = static_cast<size_t>(index - COMMAND_COUNT);
  if (articleIndex < articlePageOffset || articleIndex >= articlePageOffset + loadedArticleCount) return nullptr;
  return &articles[articleIndex - articlePageOffset];
}

bool SubstackActivity::loadSelectedArticle(SubstackArticle& article) const {
  const auto* row = rowForListIndex(selectedIndex);
  return row && SUBSTACK_STORE.loadArticle(row->id, article);
}

void SubstackActivity::beginFetch() {
  if (SUBSTACK_STORE.feeds().empty()) {
    const char* options[] = {"Open feed setup", "Cancel"};
    popup.show("No Substack feeds", options, 2, 0, [this](int choice) {
      if (choice == 0) activityManager.goToFileTransfer(true);
    });
    requestUpdate();
    return;
  }

  // This activity stays on the back stack while the Wi-Fi chooser is open.
  // Its article metadata is a second copy of the SD-backed index and can be
  // tens of KiB. The screen has already been painted, and a silent restart
  // reloads it after sync, so release that copy before bringing up Wi-Fi.
  LOG_DBG("SUBSTACK", "Before refresh cleanup: free=%u max-block=%u articles=%u",
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
          static_cast<unsigned>(loadedArticleCount));
  articles.reset();
  loadedArticleCount = 0;
  totalArticleCount = 0;
  visibleArticleBytes = 0;
  visibleUnreadCount = 0;
  popup = OptionPopup{};
  syncStatus.clear();
  syncStatus.shrink_to_fit();
  if (renderer.getFontCacheManager()) renderer.getFontCacheManager()->clearCache();
  LOG_DBG("SUBSTACK", "After refresh cleanup: free=%u max-block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput), [this](const ActivityResult& result) {
    LOG_DBG("SUBSTACK", "Wi-Fi result handler: free=%u max-block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    if (result.isCancelled) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(30);
      syncStatus.clear();
      // Run the menu reload from loop(), after ActivityManager has destroyed
      // this result callback and all of its captured Wi-Fi state.
      syncing = true;
      reloadPending = true;
      return;
    }
    syncing = true;
    syncStatus = "Preparing feed refresh...";
    requestUpdateAndWait();
    // Rendering the status can repopulate decompression/glyph buffers. Keep
    // the pixels on the e-paper panel but release those transient allocations
    // before wolfSSL starts.
    if (renderer.getFontCacheManager()) renderer.getFontCacheManager()->clearCache();
    LOG_DBG("SUBSTACK", "Starting sync: free=%u max-block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    const auto summary = SubstackSyncEngine::sync([this](size_t index, size_t count, const std::string& publication) {
      LOG_DBG("SUBSTACK", "Refreshing feed %u/%u: %s", static_cast<unsigned>(index + 1),
              static_cast<unsigned>(count), publication.c_str());
      char message[112];
      snprintf(message, sizeof(message), "Refreshing %u/%u: %s", static_cast<unsigned>(index + 1),
               static_cast<unsigned>(count), publication.c_str());
      syncStatus = message;
      requestUpdateAndWait();
      // The next operation is a fresh TLS handshake, so release allocations
      // made while painting this progress frame.
      if (renderer.getFontCacheManager()) renderer.getFontCacheManager()->clearCache();
    });
    if (summary.storageBlocked()) {
      char message[96];
      snprintf(message, sizeof(message), "Storage limit reached — %u articles were not downloaded",
               static_cast<unsigned>(summary.skippedStorage()));
      syncStatus = message;
    } else if (summary.lowMemoryFailures > 0) {
      syncStatus = "Refresh stopped: not enough memory for secure connection";
    } else if (summary.feedsFailed > 0) {
      char message[80];
      snprintf(message, sizeof(message), "Refresh failed: %s",
               summary.lastError.empty() ? "secure feed connection failed" : summary.lastError.c_str());
      syncStatus = message;
    } else {
      char message[80];
      snprintf(message, sizeof(message), "%u new · %u updated", static_cast<unsigned>(summary.downloaded),
               static_cast<unsigned>(summary.updated));
      syncStatus = message;
    }
    LOG_INF("SUBSTACK",
            "Refresh complete: attempted=%u failed=%u downloaded=%u updated=%u existing=%u quota=%u reserve=%u low-memory=%u",
            static_cast<unsigned>(summary.feedsAttempted), static_cast<unsigned>(summary.feedsFailed),
            static_cast<unsigned>(summary.downloaded), static_cast<unsigned>(summary.updated),
            static_cast<unsigned>(summary.alreadyDownloaded),
            static_cast<unsigned>(summary.skippedQuota), static_cast<unsigned>(summary.skippedReserve),
            static_cast<unsigned>(summary.lowMemoryFailures));

    // Tear down Wi-Fi before reconstructing the article list. This retains the
    // TLS headroom during sync, then presents the completed list directly.
    // Depending on a reboot here left the e-paper panel on the pre-sync frame
    // on some X3 runs even though the refresh had completed successfully.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(30);
    reloadPending = true;
  });
}

void SubstackActivity::activate() {
  if (selectedIndex == 0) {
    beginFetch();
  } else if (selectedIndex == 1) {
    activityManager.goToFileTransfer(true);
  } else {
    SubstackArticle article;
    if (loadSelectedArticle(article) && Storage.exists(article.epubPath.c_str()))
      activityManager.goToReader(article.epubPath);
    else {
      syncStatus = "Article file missing — refresh to repair";
      requestUpdate();
    }
  }
}

void SubstackActivity::deleteSelected() {
  SubstackArticle article;
  if (!loadSelectedArticle(article)) return;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, "Delete article?", article.title),
      [this, article](const ActivityResult& result) {
        if (result.isCancelled) return;
        clearBookCache(article.epubPath);
        RECENT_BOOKS.removeByPath(article.epubPath);
        SUBSTACK_STORE.deleteArticle(article.id);
        reload();
        requestUpdate(true);
      });
}

void SubstackActivity::showArticleActions() {
  SubstackArticle snapshot;
  if (!loadSelectedArticle(snapshot)) return;
  const char* options[] = {"Open / Resume", snapshot.read ? "Mark unread" : "Mark read", "Show original link",
                           "Delete"};
  popup.show("Article actions", options, 4, 0, [this, snapshot](int choice) {
    if (choice == 0) {
      if (Storage.exists(snapshot.epubPath.c_str())) {
        activityManager.goToReader(snapshot.epubPath);
      } else {
        syncStatus = "Article file missing — refresh to repair";
        SUBSTACK_STORE.saveArticle(snapshot);  // Invalidates the stale menu index.
        reload();
        requestUpdate();
      }
    } else if (choice == 1) {
      SUBSTACK_STORE.markReadByPath(snapshot.epubPath, !snapshot.read);
      if (!snapshot.read) RECENT_BOOKS.removeByPath(snapshot.epubPath);
      reload();
      requestUpdate();
    } else if (choice == 2) {
      activityManager.pushActivity(std::make_unique<QrDisplayActivity>(renderer, mappedInput, snapshot.sourceUrl));
    } else if (choice == 3) {
      deleteSelected();
    }
  });
  requestUpdate();
}

void SubstackActivity::loop() {
  if (reloadPending) {
    // The Wi-Fi result handler has now returned, releasing its std::function,
    // ActivityResult, sync summary, and other callback-lifetime allocations.
    reloadPending = false;
    if (renderer.getFontCacheManager()) renderer.getFontCacheManager()->clearCache();
    LOG_DBG("SUBSTACK", "Deferred menu reload: free=%u max-block=%u",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    reload();
    syncing = false;
    requestUpdateAndWait();
    return;
  }
  if (syncing) return;
  if (popup.isActive()) {
    popup.handleInput(mappedInput, [this] { requestUpdate(); });
    return;
  }
  const int count = COMMAND_COUNT + static_cast<int>(totalArticleCount);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::SUBSTACK);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= COMMAND_COUNT) showArticleActions();
    else activate();
    return;
  }
  if (gpio.deviceIsX3() && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    activate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    ensureSelectedPageLoaded();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    ensureSelectedPageLoaded();
    requestUpdate();
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  int touched = selectedIndex;
  const auto touch = handleListTouch(touched, count, contentTop, contentHeight, true);
  if (touch != ListTouchResult::None) {
    selectedIndex = touched;
    ensureSelectedPageLoaded();
    if (touch == ListTouchResult::Activated) activate();
  }
}

std::string SubstackActivity::subtitleFor(const SubstackArticleListItem& article) const {
  std::string result = article.publication;
  const time_t timestamp = static_cast<time_t>(article.publishedAt);
  if (timestamp > 0) {
    char date[20];
    const std::tm* tm = localtime(&timestamp);
    if (tm && strftime(date, sizeof(date), "%b %d, %Y", tm)) result += " · " + std::string(date);
  }
  if (article.read) result += " · Read";
  if (article.preview) result += " · Preview";
  return result;
}

void SubstackActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  char headerStatus[64];
  snprintf(headerStatus, sizeof(headerStatus), "%u unread · %s", static_cast<unsigned>(visibleUnreadCount),
           storageLabel(visibleArticleBytes).c_str());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, "Substack", headerStatus);

  std::string notice;
  const auto& lastSync = SUBSTACK_STORE.lastSync();
  if (syncing) notice = syncStatus;
  else if (menuLoadResult == SubstackStore::ArticleListResult::LowMemory)
    notice = "Not enough memory to load article menu";
  else if (menuLoadResult == SubstackStore::ArticleListResult::StorageError)
    notice = "Article menu could not be loaded";
  else if (lastSync.storageBlocked()) {
    char value[112];
    snprintf(value, sizeof(value), "Storage limit reached — %u not downloaded",
             static_cast<unsigned>(lastSync.skippedStorage()));
    notice = value;
  } else if (!syncStatus.empty()) notice = syncStatus;
  else if (lastSync.feedsFailed > 0) {
    notice = lastSync.lastError.empty() ? "Last refresh failed" : lastSync.lastError;
  }
  else if (lastSync.finishedAt > 0) notice = "Last refresh completed";
  else notice = "Manual refresh only";
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, width, metrics.tabBarHeight}, notice.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int count = COMMAND_COUNT + static_cast<int>(totalArticleCount);
  GUI.drawList(
      renderer, Rect{0, contentTop, width, contentHeight}, count, selectedIndex,
      [this](int index) {
        if (index == 0) return std::string("Fetch new articles");
        if (index == 1) return std::string("Manage feeds");
        const auto* article = rowForListIndex(index);
        return article ? std::string(article->title) : std::string("Article unavailable");
      },
      [this](int index) {
        if (index == 0) return std::string("Connect to Wi-Fi and refresh now");
        if (index == 1) return std::string("Add, edit, import, or remove feed URLs");
        const auto* article = rowForListIndex(index);
        return article ? subtitleFor(*article) : std::string();
      },
      [](int index) { return index == 0 ? Wifi : index == 1 ? Settings : Book; });

  const auto labels = mappedInput.mapLabels("Back", selectedIndex >= COMMAND_COUNT ? "Actions" : "Select", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (gpio.deviceIsX3()) GUI.drawSideButtonHints(renderer, "", "Open");
  if (!popup.processRender(renderer, mappedInput)) renderer.displayBuffer();
}
