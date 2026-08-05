#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

namespace FlowMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 0,
                                 .batteryBarHeight = 40,
                                 .headerHeight = 44,
                                 .verticalSpacing = 12,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 40,
                                 .listWithSubtitleRowHeight = 60,
                                 .menuRowHeight = 48,
                                 .menuSpacing = 5,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 50,
                                 .homeCoverHeight = 320,
                                 .homeCoverTileHeight = 396,
                                 .homeRecentBooksCount = 7,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 8,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 48,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.12f,
                                 .popupMarginX = 16,
                                 .popupMarginY = 12,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 6,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .optionPopupItemSpacing = 8,
                                 .optionPopupInnerPadding = 20,
                                 .optionPopupSelectionHPadding = 16,
                                 .optionPopupSelectionVPadding = 12,
                                 .optionPopupTitleGap = 16,
                                 .optionPopupUseSmallFont = true,
                                 .optionPopupOptionFontBold = false,
                                 .optionPopupSelectionRadius = 6,
                                 .optionPopupSelectionLight = true,
                                 .optionPopupDrawAllRows = false,
                                 .optionPopupDialogSideMargin = 20,
                                 .optionPopupTitleSeparator = true,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class FlowTheme : public BaseTheme {
 public:
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  bool showsFileIcons() const override { return true; }
};
