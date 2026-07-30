#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <optional>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "Epub/Section.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
ReaderRenderSpec getReaderRenderSpecForCurrentView(const GfxRenderer& renderer) {
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += std::max<int>(SETTINGS.screenMargin, UITheme::getInstance().getStatusBarHeight());

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  return SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);
}
}  // namespace

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderChapterSelectionActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);
  const int totalItems = getTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  auto selectChapter = [this] {
    const auto tocItem = epub->getTocItem(selectorIndex);
    if (tocItem.spineIndex == -1) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    } else {
      setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
      finish();
    }
  };

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;
  switch (handleListTouch(selectorIndex, totalItems, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      selectChapter();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selectChapter();
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  const int totalItems = getTotalItems();
  const ReaderRenderSpec renderSpec = getReaderRenderSpecForCurrentView(renderer);
  auto getTocStartPage = [this, &renderSpec](const auto& item) -> std::optional<uint16_t> {
    if (item.spineIndex < 0) {
      return std::nullopt;
    }
    if (item.anchor.empty()) {
      return 0;
    }

    Section section(epub, item.spineIndex, renderer);
    return section.getCachedPageForAnchor(item.anchor, renderSpec);
  };
  auto getTocPageCount = [this, totalItems, &renderSpec, &getTocStartPage](int index) -> std::string {
    const auto item = epub->getTocItem(index);
    if (item.spineIndex < 0) {
      return "";
    }

    Section section(epub, item.spineIndex, renderer);
    const auto totalPages = section.getCachedPageCount(renderSpec);
    if (!totalPages || *totalPages == 0) {
      return "";
    }

    const auto startPage = getTocStartPage(item);
    if (!startPage || *startPage >= *totalPages) {
      return "";
    }

    uint16_t endPageExclusive = *totalPages;
    for (int i = index + 1; i < totalItems; i++) {
      const auto nextItem = epub->getTocItem(i);
      if (nextItem.level > item.level) {
        continue;
      }
      if (nextItem.spineIndex < 0) {
        return "";
      }

      if (nextItem.spineIndex == item.spineIndex) {
        const auto nextStartPage = getTocStartPage(nextItem);
        if (!nextStartPage || *nextStartPage > *totalPages) {
          return "";
        }
        endPageExclusive = *nextStartPage;
      }
      break;
    }

    const uint16_t pageCount = (endPageExclusive <= *startPage) ? 1 : endPageExclusive - *startPage;
    return std::to_string(pageCount);
  };

  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, totalItems, selectorIndex,
               [this](int index) {
                 auto item = epub->getTocItem(index);
                 std::string indent(std::max(0, static_cast<int>(item.level) - 1) * 2, ' ');
                 return indent + item.title;
               },
               nullptr, nullptr, getTocPageCount, false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
