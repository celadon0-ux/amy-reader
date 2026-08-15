#pragma once

#include <string>
#include <vector>

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
  void activate();
  void beginFetch();
  void showArticleActions();
  void deleteSelected();
  std::string subtitleFor(const SubstackArticle& article) const;

  OptionPopup popup;
  std::vector<SubstackArticle> articles;
  uint64_t visibleArticleBytes = 0;
  uint32_t visibleUnreadCount = 0;
  int selectedIndex = 0;
  bool syncing = false;
  std::string syncStatus;
};
