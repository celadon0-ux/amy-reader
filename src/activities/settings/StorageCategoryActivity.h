#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/StorageAnalyzer.h"

class StorageCategoryActivity final : public Activity {
 public:
  StorageCategoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StorageCategory category,
                          std::vector<StorageItem> cachedItems, uint32_t totalItems, bool scanComplete);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }

 private:
  void finishWithResult();
  void activateSelected();
  void deleteItem(const StorageItem& item);
  const char* categoryTitle() const;

  StorageCategory category;
  std::vector<StorageItem> items;
  uint32_t totalItems = 0;
  int selectedIndex = 0;
  bool scanComplete = true;
  bool storageChanged = false;
  bool deleteFailed = false;
  ButtonNavigator buttonNavigator;
};
