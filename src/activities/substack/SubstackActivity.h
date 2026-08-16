#pragma once

#include <memory>
#include <string>

#include "SubstackStore.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class SubstackActivity final : public Activity {
 public:
  SubstackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Substack", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return syncing; }

 private:
  void reload();
  void ensureSelectedPageLoaded();
  void activate();
  void beginFetch();
  void showArticleActions();
  void deleteSelected();
  size_t pageCapacity() const;
  size_t pageOffsetForSelection() const;
  const SubstackArticleListItem* rowForListIndex(int index) const;
  bool loadSelectedArticle(SubstackArticle& article) const;
  std::string subtitleFor(const SubstackArticleListItem& article) const;

  OptionPopup popup;
  std::unique_ptr<SubstackArticleListItem[]> articles;
  size_t loadedArticleCount = 0;
  size_t articlePageOffset = 0;
  size_t totalArticleCount = 0;
  uint64_t visibleArticleBytes = 0;
  uint32_t visibleUnreadCount = 0;
  int selectedIndex = 0;
  bool syncing = false;
  bool reloadPending = false;
  SubstackStore::ArticleListResult menuLoadResult = SubstackStore::ArticleListResult::Ok;
  std::string syncStatus;
};
