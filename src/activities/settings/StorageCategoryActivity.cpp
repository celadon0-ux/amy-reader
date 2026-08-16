#include "StorageCategoryActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <utility>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SubstackStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
std::string fileNameFromPath(const std::string& path) {
  const size_t separator = path.find_last_of('/');
  return separator == std::string::npos ? path : path.substr(separator + 1);
}
}  // namespace

StorageCategoryActivity::StorageCategoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 const StorageCategory category,
                                                 std::vector<StorageItem> cachedItems, const uint32_t totalItems,
                                                 const bool scanComplete)
    : Activity("StorageCategory", renderer, mappedInput),
      category(category),
      items(std::move(cachedItems)),
      totalItems(totalItems),
      scanComplete(scanComplete) {}

void StorageCategoryActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void StorageCategoryActivity::finishWithResult() {
  setResult(StorageMutationResult{storageChanged});
  finish();
}

const char* StorageCategoryActivity::categoryTitle() const {
  switch (category) {
    case StorageCategory::Books:
      return tr(STR_STORAGE_BOOKS);
    case StorageCategory::Substack:
      return tr(STR_STORAGE_SUBSTACK_ARTICLES);
    case StorageCategory::Images:
      return tr(STR_STORAGE_IMAGES);
    case StorageCategory::Fonts:
      return tr(STR_STORAGE_FONTS);
    case StorageCategory::Dictionaries:
      return tr(STR_STORAGE_DICTIONARIES);
    case StorageCategory::ReadingCache:
      return tr(STR_STORAGE_CACHE);
    case StorageCategory::Other:
      return tr(STR_STORAGE_OTHER);
    default:
      return tr(STR_STORAGE);
  }
}

void StorageCategoryActivity::deleteItem(const StorageItem& item) {
  bool cleanupComplete = true;
  bool managedCleanupAttempted = false;
  if (category == StorageCategory::Substack) {
    SubstackArticle article;
    if (SUBSTACK_STORE.findArticleByPath(item.path, article)) {
      managedCleanupAttempted = true;
      cleanupComplete = SUBSTACK_STORE.deleteArticle(article.id);
    } else {
      cleanupComplete = Storage.remove(item.path.c_str());
    }
  } else {
    cleanupComplete = Storage.remove(item.path.c_str());
  }

  const bool fileRemoved = !Storage.exists(item.path.c_str());
  if (!fileRemoved) {
    storageChanged = storageChanged || managedCleanupAttempted;
    deleteFailed = true;
    requestUpdate();
    return;
  }

  if (category == StorageCategory::Books || category == StorageCategory::Substack) {
    clearBookCache(item.path);
  }
  if (category == StorageCategory::Substack) RECENT_BOOKS.removeByPath(item.path);
  storageChanged = true;
  deleteFailed = !cleanupComplete;
  const auto deleted = std::find_if(items.begin(), items.end(), [&item](const StorageItem& candidate) {
    return candidate.path == item.path;
  });
  if (deleted != items.end()) items.erase(deleted);
  if (totalItems > 0) totalItems--;
  selectedIndex = std::min(selectedIndex, std::max(0, static_cast<int>(items.size()) - 1));
  requestUpdate();
}

void StorageCategoryActivity::activateSelected() {
  if (items.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(items.size())) return;
  const StorageItem item = items[selectedIndex];
  if (!item.deletable) return;

  const std::string body = item.path + " (" + StorageAnalyzer::formatBytes(item.size) + ")";
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_STORAGE_DELETE_ITEM), body),
      [this, item](const ActivityResult& result) {
        if (!result.isCancelled) deleteItem(item);
      });
}

void StorageCategoryActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishWithResult();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  int touchIndex = selectedIndex;
  switch (handleListTouch(touchIndex, static_cast<int>(items.size()), listTop, listHeight, true)) {
    case ListTouchResult::Activated:
      selectedIndex = touchIndex;
      activateSelected();
      return;
    case ListTouchResult::Consumed:
      selectedIndex = touchIndex;
      return;
    case ListTouchResult::None:
      break;
  }

  const int itemCount = static_cast<int>(items.size());
  const int pageItems = GUI.getListPageItems(listHeight, true);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, itemCount, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, itemCount, pageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, itemCount, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, itemCount, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, itemCount, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, itemCount, pageItems);
    requestUpdate();
  });
}

void StorageCategoryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  std::string subtitle;
  if (totalItems > items.size()) {
    subtitle = std::to_string(items.size()) + " / " + std::to_string(totalItems);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, categoryTitle(),
                 subtitle.empty() ? nullptr : subtitle.c_str());

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (items.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_STORAGE_NO_ITEMS));
  } else {
    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(items.size()), selectedIndex,
        [this](int index) { return fileNameFromPath(items[index].path); },
        [this](int index) { return items[index].path; }, nullptr,
        [this](int index) { return StorageAnalyzer::formatBytes(items[index].size); }, true,
        [this](int index) { return !items[index].deletable; });
  }

  if (deleteFailed) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 2,
                              tr(STR_STORAGE_DELETE_FAILED), true, EpdFontFamily::BOLD);
  } else if (!scanComplete) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 2,
                              tr(STR_STORAGE_SCAN_INCOMPLETE));
  }

  const char* confirm = (!items.empty() && items[selectedIndex].deletable) ? tr(STR_DELETE) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, items.empty() ? "" : tr(STR_DIR_UP),
                                            items.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
