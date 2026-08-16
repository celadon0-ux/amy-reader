#pragma once

#include <Print.h>
#include <expat.h>
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>

struct SubstackRssItem {
  std::string guid;
  std::string publication;
  std::string title;
  std::string author;
  std::string link;
  std::string htmlPath;
  uint64_t htmlBytes = 0;
  uint64_t publishedAt = 0;
  bool preview = false;
  bool contentTruncated = false;
};

class SubstackRssParser final : public Print {
 public:
  using ItemCallback = std::function<bool(SubstackRssItem&&)>;

  explicit SubstackRssParser(ItemCallback callback);
  ~SubstackRssParser() override;
  SubstackRssParser(const SubstackRssParser&) = delete;
  SubstackRssParser& operator=(const SubstackRssParser&) = delete;

  size_t write(uint8_t value) override;
  size_t write(const uint8_t* data, size_t length) override;
  void flush() override;

  bool error() const { return errorOccurred; }
  bool aborted() const { return callbackAborted; }
  const std::string& publication() const { return feedTitle; }
  uint32_t itemsSeen() const { return seenCount; }
  uint32_t itemsEmitted() const { return emittedCount; }
  uint32_t itemsSkipped() const { return skippedCount; }

  static uint64_t parseDate(const std::string& value);

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** attributes);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* data, int length);
  static std::string localName(const char* name);
  static void appendBounded(std::string& target, const char* value, size_t length, size_t limit, bool* truncated = nullptr);
  bool beginBody(bool replaceExisting);
  void closeBody();
  void updatePreviewProbe(const char* data, size_t length);

  XML_Parser parser = nullptr;
  ItemCallback onItem;
  SubstackRssItem item;
  std::string feedTitle;
  std::string currentField;
  std::string currentText;
  std::string previewProbe;
  HalFile bodyFile;
  int itemDepth = 0;
  int channelDepth = 0;
  bool usingEncodedContent = false;
  bool bodyOpen = false;
  bool errorOccurred = false;
  bool callbackAborted = false;
  uint32_t seenCount = 0;
  uint32_t emittedCount = 0;
  uint32_t skippedCount = 0;
};
