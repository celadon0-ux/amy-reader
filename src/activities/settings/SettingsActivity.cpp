#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StorageCategoryActivity.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM,
                                                              StrId::STR_CAT_STORAGE};

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();
  storageSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  // Rescan /dictionaries on every rebuild: cheap (one directory listing) and
  // picks up dictionaries copied to the SD card since the last visit.
  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);
  discoveredDictionaryCount = static_cast<uint32_t>(dictionaries.size());

  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaries)) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into "Text Settings"
      // (they stay in the shared list for the web settings API)
      if (setting.inTextSettings) continue;
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  if (!BoardConfig::hasTouch()) {
    controlsSettings.insert(controlsSettings.begin(),
                            SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  // TODO: Touch devices need their own firmware update path/artifacts before OTA is exposed.
  if (!BoardConfig::hasTouch()) {
    systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  storageSettings.push_back(SettingInfo::Action(StrId::STR_STORAGE_OPTIMIZE, SettingAction::None));
  storageSettings.push_back(SettingInfo::Action(StrId::STR_STORAGE_BOOKS, SettingAction::None));
  storageSettings.push_back(SettingInfo::Action(StrId::STR_STORAGE_IMAGES, SettingAction::None));
  storageSettings.push_back(SettingInfo::Action(StrId::STR_STORAGE_FONTS, SettingAction::None));
  storageSettings.push_back(SettingInfo::Action(StrId::STR_STORAGE_DICTIONARIES, SettingAction::None));
  storageSettings.push_back(SettingInfo::Action(StrId::STR_STORAGE_CACHE, SettingAction::None));
  storageSettings.push_back(SettingInfo::Action(StrId::STR_STORAGE_OTHER, SettingAction::None));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
    case 4:
      currentSettings = &storageSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (storageScanPending) {
    requestUpdateAndWait();
    scanStorage();
    return;
  }

  bool hasChangedCategory = false;

  auto applyCategorySelection = [this] {
    switch (selectedCategoryIndex) {
      case 0:
        currentSettings = &displaySettings;
        break;
      case 1:
        currentSettings = &readerSettings;
        break;
      case 2:
        currentSettings = &controlsSettings;
        break;
      case 3:
        currentSettings = &systemSettings;
        break;
      case 4:
        currentSettings = &storageSettings;
        if (!storageScanned) storageScanPending = true;
        break;
    }
    settingsCount = static_cast<int>(currentSettings->size());
  };

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  int tx = 0;
  int ty = 0;
  const int tabTop = metrics.topPadding + metrics.headerHeight;
  const int listTop = selectedCategoryIndex == 4
                          ? storageListTop()
                          : metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int listHeight = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  auto buildTabs = [&]() {
    std::vector<TabInfo> tabs;
    tabs.reserve(categoryCount);
    for (int i = 0; i < categoryCount; i++) {
      tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
    }
    return tabs;
  };
  auto settingIndexFromPoint = [&](const int x, const int y, int& settingIndex) {
    (void)x;
    const int rowStep = GUI.getListRowStep(false);
    if (rowStep <= 0) return false;
    if (selectedCategoryIndex == 4) {
      const int optimizeTop = storageListTop();
      if (y >= optimizeTop && y < optimizeTop + rowStep) {
        settingIndex = 1;
        return true;
      }

      const int categoryTop = storageCategoryListTop();
      const int categoryHeight = storageCategoryListHeight();
      if (y < categoryTop || y >= categoryTop + categoryHeight) return false;
      const int pageItems = GUI.getListPageItems(categoryHeight, false);
      if (pageItems <= 0) return false;
      const int selectedRow = std::max(0, selectedSettingIndex - 2);
      const int pageStart = selectedRow / pageItems * pageItems;
      const int row = (y - categoryTop) / rowStep;
      const int touched = pageStart + row;
      const int categoryCount = static_cast<int>(StorageCategory::Count);
      if (row < 0 || row >= pageItems || touched < 0 || touched >= categoryCount) return false;
      settingIndex = touched + 2;
      return true;
    }

    if (settingsCount <= 0 || y < listTop || y >= listTop + listHeight) return false;
    const int pageItems = GUI.getListPageItems(listHeight, false);
    const int selectedRow = std::max(0, selectedSettingIndex - 1);
    const int pageStart = selectedRow / pageItems * pageItems;
    const int row = (y - listTop) / rowStep;
    const int touched = pageStart + row;
    if (row < 0 || row >= pageItems || touched < 0 || touched >= settingsCount) return false;
    settingIndex = touched + 1;
    return true;
  };

  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    int touchedCategory = -1;
    const auto tabs = buildTabs();
    if (GUI.tabIndexFromPoint(renderer, Rect{0, tabTop, renderer.getScreenWidth(), metrics.tabBarHeight}, tabs, tx, ty,
                              touchedCategory)) {
      if (selectedCategoryIndex != touchedCategory || selectedSettingIndex != 0) {
        selectedCategoryIndex = touchedCategory;
        selectedSettingIndex = 0;
        applyCategorySelection();
        requestUpdate();
      }
      return;
    }

    int touchedSetting = -1;
    if (settingIndexFromPoint(tx, ty, touchedSetting)) {
      if (selectedSettingIndex != touchedSetting) {
        selectedSettingIndex = touchedSetting;
        requestUpdate();
      }
      return;
    }
  }

  if (mappedInput.wasScreenTapped(tx, ty)) {
    int tappedCategory = -1;
    const auto tabs = buildTabs();
    if (GUI.tabIndexFromPoint(renderer, Rect{0, tabTop, renderer.getScreenWidth(), metrics.tabBarHeight}, tabs, tx, ty,
                              tappedCategory)) {
      selectedCategoryIndex = tappedCategory;
      selectedSettingIndex = 0;
      applyCategorySelection();
      requestUpdate();
      return;
    }

    int tappedSetting = -1;
    if (settingIndexFromPoint(tx, ty, tappedSetting)) {
      selectedSettingIndex = tappedSetting;
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  // Handle navigation
  const auto& navMetrics = UITheme::getInstance().getMetrics();
  const int settingsListHeight =
      renderer.getScreenHeight() -
      (selectedCategoryIndex == 4
           ? storageCategoryListTop() + navMetrics.buttonHintsHeight + navMetrics.verticalSpacing
           : navMetrics.topPadding + navMetrics.headerHeight + navMetrics.tabBarHeight +
                 navMetrics.buttonHintsHeight + navMetrics.verticalSpacing * 2);
  const int settingsPageItems = GUI.getListPageItems(settingsListHeight, false);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    if (selectedCategoryIndex == 4) {
      if (selectedSettingIndex == 0) {
        selectedSettingIndex = 1;
      } else if (selectedSettingIndex == 1) {
        selectedSettingIndex = 2;
      } else if (selectedSettingIndex == settingsCount) {
        selectedSettingIndex = 0;
      } else {
        selectedSettingIndex =
            ButtonNavigator::nextPageIndex(selectedSettingIndex - 2,
                                           static_cast<int>(StorageCategory::Count), settingsPageItems) +
            2;
      }
    } else {
      selectedSettingIndex = selectedSettingIndex == 0 ? 1
                                                       : ButtonNavigator::nextPageIndex(
                                                             selectedSettingIndex, settingsCount + 1,
                                                             settingsPageItems);
    }
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    if (selectedCategoryIndex == 4) {
      if (selectedSettingIndex == 0) {
        selectedSettingIndex = settingsCount;
      } else if (selectedSettingIndex == 1) {
        selectedSettingIndex = 0;
      } else if (selectedSettingIndex == 2) {
        selectedSettingIndex = 1;
      } else {
        selectedSettingIndex =
            ButtonNavigator::previousPageIndex(selectedSettingIndex - 2,
                                               static_cast<int>(StorageCategory::Count), settingsPageItems) +
            2;
      }
    } else {
      selectedSettingIndex =
          ButtonNavigator::previousPageIndex(selectedSettingIndex, settingsCount + 1, settingsPageItems);
    }
    requestUpdate();
    return;
  }

  const auto selectNextSetting = [this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  };

  const auto selectPreviousSetting = [this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  };

  const auto selectNextCategory = [this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  };

  const auto selectPreviousCategory = [this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  };

  // The X3's isolated top-left/top-right side buttons switch tabs. The remappable
  // lower-cluster Left/Right roles (Buttons 3/4 by default) move through settings.
  buttonNavigator.onRelease(MappedInputManager::Button::Right, selectNextSetting);
  buttonNavigator.onRelease(MappedInputManager::Button::Left, selectPreviousSetting);
  buttonNavigator.onContinuous(MappedInputManager::Button::Right, selectNextSetting);
  buttonNavigator.onContinuous(MappedInputManager::Button::Left, selectPreviousSetting);

  buttonNavigator.onRelease(MappedInputManager::Button::Down, selectNextCategory);
  buttonNavigator.onRelease(MappedInputManager::Button::Up, selectPreviousCategory);
  buttonNavigator.onContinuous(MappedInputManager::Button::Down, selectNextCategory);
  buttonNavigator.onContinuous(MappedInputManager::Button::Up, selectPreviousCategory);

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    applyCategorySelection();
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  if (selectedCategoryIndex == 4) {
    activateStorageRow(selectedSetting);
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (setting.enumValues.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                       currentValue, [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
                         SETTINGS.*valuePtr = idx;
                         syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
                         SETTINGS.saveToFile();
                         rebuildSettingsLists();
                       });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues > 2) {
      const auto valueSetter = setting.valueSetter;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
        valueSetter(idx);
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::TextSettings:
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::scanStorage() {
  storageAnalyzer.analyze(storageSnapshot, true);
  storageSnapshot.logicalItemCounts[static_cast<size_t>(StorageCategory::Fonts)] =
      static_cast<uint32_t>(sdFontSystem.registry().getFamilyCount());
  storageSnapshot.logicalItemCounts[static_cast<size_t>(StorageCategory::Dictionaries)] = discoveredDictionaryCount;
  storageScanPending = false;
  storageScanned = true;
  requestUpdate();
}

void SettingsActivity::handleStorageMutationResult(const ActivityResult& result) {
  const auto* mutation = std::get_if<StorageMutationResult>(&result.data);
  if (!mutation || !mutation->changed) return;
  storageScanned = false;
  storageScanPending = true;
}

void SettingsActivity::activateStorageRow(const int row) {
  if (row == 0) {
    const size_t cacheIndex = static_cast<size_t>(StorageCategory::ReadingCache);
    if (storageSnapshot.breakdown.bytes[cacheIndex] == 0) return;
    startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) { handleStorageMutationResult(result); });
    return;
  }

  if (row == 3) {
    startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             sdFontSystem.refreshIfDirty();
                             rebuildSettingsLists();
                             handleStorageMutationResult(result);
                           });
    return;
  }

  static constexpr StorageCategory ROW_CATEGORIES[] = {
      StorageCategory::Books, StorageCategory::Images, StorageCategory::Fonts,
      StorageCategory::Dictionaries, StorageCategory::ReadingCache, StorageCategory::Other};
  const int categoryRow = row - 1;
  if (categoryRow < 0 || categoryRow >= static_cast<int>(std::size(ROW_CATEGORIES))) return;
  const StorageCategory category = ROW_CATEGORIES[categoryRow];
  const size_t categoryIndex = static_cast<size_t>(category);
  startActivityForResult(
      std::make_unique<StorageCategoryActivity>(renderer, mappedInput, category,
                                                storageSnapshot.largestItems[categoryIndex],
                                                storageSnapshot.breakdown.itemCounts[categoryIndex],
                                                storageSnapshot.breakdown.complete),
      [this](const ActivityResult& result) { handleStorageMutationResult(result); });
}

int SettingsActivity::storageListTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int summaryTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  return summaryTop + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing + metrics.progressBarHeight +
         metrics.verticalSpacing;
}

int SettingsActivity::storageCategoryListTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return storageListTop() + GUI.getListRowStep(false) + metrics.verticalSpacing;
}

int SettingsActivity::storageCategoryListHeight() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - storageCategoryListTop() - metrics.buttonHintsHeight - metrics.verticalSpacing;
}

void SettingsActivity::renderStoragePanel() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int summaryTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;

  if (storageScanPending || !storageScanned) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_STORAGE_SCANNING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  std::string summary;
  const auto& breakdown = storageSnapshot.breakdown;
  if (breakdown.spaceValid) {
    char buffer[96];
    const std::string used = StorageAnalyzer::formatBytes(breakdown.usedBytes);
    const std::string total = StorageAnalyzer::formatBytes(breakdown.totalBytes);
    snprintf(buffer, sizeof(buffer), tr(STR_STORAGE_USED_OF), used.c_str(), total.c_str());
    summary = buffer;
  } else {
    summary = tr(STR_STORAGE_UNAVAILABLE);
  }
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, summaryTop, summary.c_str(), true,
                    EpdFontFamily::BOLD);

  const int barX = metrics.contentSidePadding;
  const int barY = summaryTop + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  const int barWidth = pageWidth - metrics.contentSidePadding * 2;
  const int barHeight = metrics.progressBarHeight;
  renderer.drawRect(barX, barY, barWidth, barHeight);
  if (breakdown.spaceValid && breakdown.totalBytes > 0 && barWidth > 2 && barHeight > 2) {
    const int innerX = barX + 1;
    const int innerWidth = barWidth - 2;
    const int innerHeight = barHeight - 2;
    uint64_t cumulative = 0;
    int currentX = innerX;
    static constexpr Color COLORS[] = {Black, DarkGray, LightGray, Black, DarkGray, LightGray, White};
    for (size_t i = 0; i < static_cast<size_t>(StorageCategory::Count) + 1; i++) {
      const uint64_t value = i < static_cast<size_t>(StorageCategory::Count)
                                 ? breakdown.bytes[i]
                                 : breakdown.freeBytes;
      cumulative = std::min(breakdown.totalBytes, cumulative + value);
      const int endX = innerX + static_cast<int>((cumulative * static_cast<uint64_t>(innerWidth)) /
                                                 breakdown.totalBytes);
      const int segmentWidth = std::max(0, endX - currentX);
      if (segmentWidth > 0) {
        renderer.fillRectDither(currentX, barY + 1, segmentWidth, innerHeight, COLORS[i]);
      }
      if (endX > innerX && endX < innerX + innerWidth) renderer.drawLine(endX, barY + 1, endX, barY + barHeight - 2);
      currentX = endX;
    }
  }

  const int optimizeTop = storageListTop();
  const int rowStep = GUI.getListRowStep(false);
  GUI.drawList(
      renderer, Rect{0, optimizeTop, pageWidth, rowStep}, 1, selectedSettingIndex == 1 ? 0 : -1,
      [](int) { return std::string(tr(STR_STORAGE_OPTIMIZE)); }, nullptr, nullptr,
      [&breakdown](int) {
        return StorageAnalyzer::formatBytes(breakdown.bytes[static_cast<size_t>(StorageCategory::ReadingCache)]);
      },
      true, [&breakdown](int) {
        return breakdown.bytes[static_cast<size_t>(StorageCategory::ReadingCache)] == 0;
      });

  static constexpr StrId SINGULAR_IDS[] = {
      StrId::STR_STORAGE_BOOK,       StrId::STR_STORAGE_IMAGE,       StrId::STR_STORAGE_FONT,
      StrId::STR_STORAGE_DICTIONARY, StrId::STR_STORAGE_CACHED_BOOK, StrId::STR_STORAGE_OTHER_ITEM};
  static constexpr StrId PLURAL_IDS[] = {
      StrId::STR_STORAGE_BOOKS,       StrId::STR_STORAGE_IMAGES,       StrId::STR_STORAGE_FONTS,
      StrId::STR_STORAGE_DICTIONARIES, StrId::STR_STORAGE_CACHED_BOOKS, StrId::STR_STORAGE_OTHER_ITEMS};
  const int categoryTop = storageCategoryListTop();
  const int categoryHeight = storageCategoryListHeight();
  GUI.drawList(
      renderer, Rect{0, categoryTop, pageWidth, categoryHeight}, static_cast<int>(StorageCategory::Count),
      selectedSettingIndex >= 2 ? selectedSettingIndex - 2 : -1,
      [this](int index) {
        return StorageAnalyzer::formatItemCount(storageSnapshot.logicalItemCounts[static_cast<size_t>(index)],
                                                I18N.get(SINGULAR_IDS[index]), I18N.get(PLURAL_IDS[index]));
      },
      nullptr, nullptr,
      [&breakdown](int index) { return StorageAnalyzer::formatBytes(breakdown.bytes[static_cast<size_t>(index)]); },
      true);

  const char* confirmLabel = selectedSettingIndex == 0 ? tr(STR_CAT_DISPLAY)
                                                       : (selectedSettingIndex == 1 ? tr(STR_STORAGE_OPTIMIZE_HINT)
                                                                                    : tr(STR_SELECT));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  if (selectedCategoryIndex == 4) {
    renderStoragePanel();
    return;
  }

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        }
        return valueText;
      },
      true);

  // Draw help text
  const auto confirmLabel =
      (selectedSettingIndex == 0)
          ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
          : (selectedSettingIndex > 0 && (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_TIME_TO_SLEEP
                 ? tr(STR_SELECT)
                 : tr(STR_TOGGLE));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
