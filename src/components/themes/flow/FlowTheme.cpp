#include "FlowTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/folder.h"
#include "components/icons/hotspot.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "components/themes/flow/DefaultBookCover.h"
#include "fontIds.h"

namespace {
constexpr int kCornerRadius = 6;
constexpr int kBookCornerRadius = 6;
constexpr int kSideCoverWidth = 66;
constexpr int kCenterCoverWidth = 220;
constexpr int kCenterCoverHeight = 320;
constexpr int kSideInnerHeight = 288;
constexpr int kSideOuterHeight = 256;
constexpr int kCarouselTopOffset = 50;
constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kMenuFontId = NOTOSANS_12_FONT_ID;

const uint8_t* iconForName(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Wifi:
      return WifiIcon;
    case UIIcon::Hotspot:
      return HotspotIcon;
    default:
      return nullptr;
  }
}

void drawIconColor(const GfxRenderer& renderer, const uint8_t* bitmap, int x, int y, int size, bool black) {
  if (!bitmap) return;
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) {
        renderer.drawPixel(x + (size - 1 - row), y + col, black);
      }
    }
  }
}

bool isDefaultCoverInk(const int x, const int y) {
  const uint8_t byte = FlowDefaultCover::BITMAP[y * FlowDefaultCover::ROW_BYTES + (x >> 3)];
  return ((byte >> (7 - (x & 7))) & 1) == 0;
}

void drawDefaultCoverBitmap(const GfxRenderer& renderer, const int x, const int y, const int width,
                            const int height) {
  if (width <= 0 || height <= 0) return;

  for (int dstY = 0; dstY < height; dstY++) {
    const int srcY = dstY * FlowDefaultCover::HEIGHT / height;
    for (int dstX = 0; dstX < width; dstX++) {
      const int srcX = dstX * FlowDefaultCover::WIDTH / width;
      if (isDefaultCoverInk(srcX, srcY)) {
        renderer.drawPixel(x + dstX, y + dstY, true);
      }
    }
  }
}

void drawPlaceholderCover(GfxRenderer& renderer, int x, int y, int w, int h, bool perspective = false, int hLeft = 0,
                           int hRight = 0) {
  if (perspective) {
    const int maxH = std::max(hLeft, hRight);
    const int centerY = y + maxH / 2;
    const int topLeft = centerY - hLeft / 2;
    const int bottomLeft = centerY + hLeft / 2 - 1;
    const int topRight = centerY - hRight / 2;
    const int bottomRight = centerY + hRight / 2 - 1;
    const int xs[4] = {x, x + w - 1, x + w - 1, x};
    const int ys[4] = {topLeft, topRight, bottomRight, bottomLeft};
    renderer.fillPolygon(xs, ys, 4, false);

    const int widthDenominator = std::max(1, w - 1);
    for (int dstX = 0; dstX < w; dstX++) {
      const int columnHeight = hLeft + (hRight - hLeft) * dstX / widthDenominator;
      if (columnHeight <= 0) continue;

      const int columnTop = centerY - columnHeight / 2;
      const int srcX = dstX * FlowDefaultCover::WIDTH / w;
      for (int dstY = 0; dstY < columnHeight; dstY++) {
        const int srcY = dstY * FlowDefaultCover::HEIGHT / columnHeight;
        if (isDefaultCoverInk(srcX, srcY)) {
          renderer.drawPixel(x + dstX, columnTop + dstY, true);
        }
      }
    }

    renderer.drawLine(x, topLeft, x + w - 1, topRight, true);
    renderer.drawLine(x + w - 1, topRight, x + w - 1, bottomRight, true);
    renderer.drawLine(x + w - 1, bottomRight, x, bottomLeft, true);
    renderer.drawLine(x, bottomLeft, x, topLeft, true);
    return;
  }

  renderer.fillRoundedRect(x, y, w, h, kBookCornerRadius, Color::White);
  drawDefaultCoverBitmap(renderer, x, y, w, h);
  renderer.maskRoundedRectOutsideCorners(x, y, w, h, kBookCornerRadius, Color::White);
  renderer.drawRoundedRect(x, y, w, h, 1, kBookCornerRadius, true);
}

bool drawCoverBitmap(GfxRenderer& renderer, const RecentBook& book, int x, int y, int w, int h, bool rounded) {
  if (book.coverBmpPath.empty()) return false;
  const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, kCenterCoverHeight);
  HalFile file;
  if (!Storage.openFileForRead("FLOW", coverPath, file)) return false;

  Bitmap bitmap(file);
  const bool ok = bitmap.parseHeaders() == BmpReaderError::Ok;
  if (ok) {
    renderer.drawBitmap(bitmap, x, y, w, h);
    if (rounded) {
      renderer.maskRoundedRectOutsideCorners(x, y, w, h, kBookCornerRadius, Color::White);
    }
  }
  file.close();
  return ok;
}

bool drawPerspectiveCoverBitmap(GfxRenderer& renderer, const RecentBook& book, int x, int y, int w, int hLeft,
                                int hRight) {
  if (book.coverBmpPath.empty()) return false;
  const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, kCenterCoverHeight);
  HalFile file;
  if (!Storage.openFileForRead("FLOW", coverPath, file)) return false;

  Bitmap bitmap(file);
  const bool ok = bitmap.parseHeaders() == BmpReaderError::Ok;
  if (ok) {
    renderer.drawPerspectiveBitmap(bitmap, x, y, w, hLeft, hRight);
  }
  file.close();
  return ok;
}

std::string bookDisplayTitle(const RecentBook& book) {
  if (!book.title.empty()) return book.title;
  std::string name = book.path;
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  return name;
}
}  // namespace

void FlowTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  (void)title;
  (void)subtitle;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - FlowMetrics::values.contentSidePadding - FlowMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 14, FlowMetrics::values.batteryWidth, FlowMetrics::values.batteryHeight},
                   showBatteryPercentage);
}

void FlowTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  if (recentBooks.empty()) {
    const int coverY = rect.y + kCarouselTopOffset;
    drawPlaceholderCover(renderer, rect.x + (rect.width - kCenterCoverWidth) / 2, coverY, kCenterCoverWidth,
                         kCenterCoverHeight);
    renderer.drawCenteredText(kTitleFontId, coverY + kCenterCoverHeight + 12, tr(STR_NO_OPEN_BOOK), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, coverY + kCenterCoverHeight + 36, tr(STR_START_READING));
    return;
  }

  const int count = static_cast<int>(recentBooks.size());
  const bool hasBookFocus = selectorIndex >= 0 && selectorIndex < count;
  int currentIndex = selectorIndex >= 1000 ? selectorIndex - 1000 : selectorIndex;
  if (currentIndex < 0 || currentIndex >= count) currentIndex = 0;

  if (bufferRestored) {
    coverRendered = true;
    coverBufferStored = true;
  } else if (!coverRendered) {
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  const int centerX = rect.x + rect.width / 2;
  const int centerY = rect.y + kCarouselTopOffset;
  const int centerLeft = centerX - kCenterCoverWidth / 2;
  const int centerRight = centerLeft + kCenterCoverWidth;

  auto drawSideCover = [&](int idx, bool left, bool far) {
    const int hLeft = left ? kSideInnerHeight : kSideOuterHeight;
    const int hRight = left ? kSideOuterHeight : kSideInnerHeight;
    const int drawX = left ? centerLeft - (far ? 100 : 50) : centerRight - 15 + (far ? 50 : 0);
    const int maxH = std::max(hLeft, hRight);
    const int drawY = centerY + kCenterCoverHeight / 2 - maxH / 2;
    if (!drawPerspectiveCoverBitmap(renderer, recentBooks[idx], drawX, drawY, kSideCoverWidth, hLeft, hRight)) {
      drawPlaceholderCover(renderer, drawX, drawY, kSideCoverWidth, maxH, true, hLeft, hRight);
    }
  };

  if (count >= 5) drawSideCover((currentIndex + count - 2) % count, true, true);
  if (count >= 4) drawSideCover((currentIndex + 2) % count, false, true);
  if (count >= 2) drawSideCover((currentIndex + count - 1) % count, true, false);
  if (count >= 3) drawSideCover((currentIndex + 1) % count, false, false);

  renderer.fillRoundedRect(centerLeft, centerY, kCenterCoverWidth, kCenterCoverHeight, kBookCornerRadius, Color::White);
  if (!drawCoverBitmap(renderer, recentBooks[currentIndex], centerLeft, centerY, kCenterCoverWidth, kCenterCoverHeight,
                       true)) {
    drawPlaceholderCover(renderer, centerLeft, centerY, kCenterCoverWidth, kCenterCoverHeight);
  }
  renderer.drawRoundedRect(centerLeft, centerY, kCenterCoverWidth, kCenterCoverHeight, 1, kBookCornerRadius, true);

  if (hasBookFocus) {
    renderer.drawRoundedRect(centerLeft - 3, centerY - 3, kCenterCoverWidth + 6, kCenterCoverHeight + 6, 3,
                             kBookCornerRadius + 3, true);
  }

  const std::string title = bookDisplayTitle(recentBooks[currentIndex]);
  const std::string clipped = renderer.truncatedText(kTitleFontId, title.c_str(), rect.width - 40, EpdFontFamily::BOLD);
  const int titleW = renderer.getTextWidth(kTitleFontId, clipped.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(kTitleFontId, centerX - titleW / 2, rect.y - 4, clipped.c_str(), true, EpdFontFamily::BOLD);

  if (!recentBooks[currentIndex].author.empty()) {
    const std::string author =
        renderer.truncatedText(UI_10_FONT_ID, recentBooks[currentIndex].author.c_str(), rect.width - 80);
    const int authorW = renderer.getTextWidth(UI_10_FONT_ID, author.c_str());
    renderer.drawText(UI_10_FONT_ID, centerX - authorW / 2, centerY + kCenterCoverHeight + 8, author.c_str());
  }
}

void FlowTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  const int rowHeight = FlowMetrics::values.menuRowHeight;
  const int spacing = FlowMetrics::values.menuSpacing;
  const int menuLeft = std::max(FlowMetrics::values.contentSidePadding, rect.x + rect.width / 2 - 190);
  const int menuWidth = std::min(230, rect.x + rect.width - menuLeft - FlowMetrics::values.contentSidePadding);

  for (int i = 0; i < buttonCount; ++i) {
    const bool selected = selectedIndex == i;
    const int y = rect.y + i * (rowHeight + spacing);

    if (selected) {
      renderer.fillRoundedRect(menuLeft, y, menuWidth, rowHeight, kCornerRadius, Color::Black);
    }

    int textX = menuLeft + 12;
    if (rowIcon) {
      const uint8_t* icon = iconForName(rowIcon(i));
      if (icon) {
        drawIconColor(renderer, icon, textX, y + (rowHeight - 32) / 2, 32, !selected);
        textX += 42;
      }
    }

    const std::string label = buttonLabel(i);
    const std::string clipped =
        renderer.truncatedText(kMenuFontId, label.c_str(), std::max(0, menuWidth - (textX - menuLeft) - 10));
    const int textY = y + (rowHeight - renderer.getLineHeight(kMenuFontId)) / 2 - 2;
    renderer.drawText(kMenuFontId, textX, textY, clipped.c_str(), !selected);
  }
}
