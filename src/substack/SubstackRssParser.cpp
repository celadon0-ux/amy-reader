#include "SubstackRssParser.h"

#include <Arduino.h>
#include <Logging.h>
#include <XmlParserUtils.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

namespace {
constexpr size_t MAX_TITLE = 240;
constexpr size_t MAX_AUTHOR = 160;
constexpr size_t MAX_URL = 1024;
constexpr size_t MAX_GUID = 512;
constexpr const char* ITEM_BODY_PATH = "/.crosspoint/substack/rss-item.tmp";
constexpr size_t MAX_PREVIEW_PROBE = 4096;

std::string trim(std::string value) {
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
  return value;
}

int monthNumber(const std::string& month) {
  static constexpr const char* names[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                          "jul", "aug", "sep", "oct", "nov", "dec"};
  std::string lower;
  for (char c : month) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  for (int i = 0; i < 12; ++i)
    if (lower == names[i]) return i;
  return -1;
}

bool containsPreviewPhrase(const std::string& html) {
  std::string tail;
  const size_t start = html.size() > 4096 ? html.size() - 4096 : 0;
  tail.reserve(html.size() - start);
  for (size_t i = start; i < html.size(); ++i)
    tail.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(html[i]))));
  return tail.find("continue reading") != std::string::npos || tail.find("read more") != std::string::npos ||
         tail.find("paid subscribers") != std::string::npos || tail.find("subscribe to read") != std::string::npos;
}
}  // namespace

SubstackRssParser::SubstackRssParser(ItemCallback callback) : onItem(std::move(callback)) {
  LOG_DBG("SUBRSS", "Creating parser after TLS: free=%u max-block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccurred = true;
    return;
  }
  LOG_DBG("SUBRSS", "Parser ready: free=%u max-block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
}

SubstackRssParser::~SubstackRssParser() {
  closeBody();
  Storage.remove(ITEM_BODY_PATH);
  destroyXmlParser(parser);
  LOG_DBG("SUBRSS", "Parser destroyed: seen=%u emitted=%u skipped=%u free=%u max-block=%u",
          static_cast<unsigned>(seenCount), static_cast<unsigned>(emittedCount), static_cast<unsigned>(skippedCount),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

size_t SubstackRssParser::write(uint8_t value) { return write(&value, 1); }

size_t SubstackRssParser::write(const uint8_t* data, size_t length) {
  if (errorOccurred || callbackAborted || !parser) return 0;
  if (XML_Parse(parser, reinterpret_cast<const char*>(data), static_cast<int>(length), XML_FALSE) != XML_STATUS_OK) {
    LOG_ERR("SUBRSS", "RSS parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
            XML_ErrorString(XML_GetErrorCode(parser)));
    errorOccurred = true;
    destroyXmlParser(parser);
    parser = nullptr;
    return 0;
  }
  return length;
}

void SubstackRssParser::flush() {
  if (errorOccurred || callbackAborted || !parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) errorOccurred = true;
}

std::string SubstackRssParser::localName(const char* name) {
  const char* colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}

bool SubstackRssParser::beginBody(bool replaceExisting) {
  closeBody();
  if (replaceExisting) Storage.remove(ITEM_BODY_PATH);
  if (!Storage.openFileForWrite("SUBRSS", ITEM_BODY_PATH, bodyFile)) {
    errorOccurred = true;
    return false;
  }
  bodyOpen = true;
  item.htmlPath = ITEM_BODY_PATH;
  item.htmlBytes = 0;
  previewProbe.clear();
  return true;
}

void SubstackRssParser::closeBody() {
  if (!bodyOpen) return;
  bodyFile.flush();
  bodyFile.close();
  bodyOpen = false;
}

void SubstackRssParser::updatePreviewProbe(const char* data, size_t length) {
  if (length >= MAX_PREVIEW_PROBE) {
    previewProbe.assign(data + length - MAX_PREVIEW_PROBE, MAX_PREVIEW_PROBE);
    return;
  }
  if (previewProbe.size() + length > MAX_PREVIEW_PROBE)
    previewProbe.erase(0, previewProbe.size() + length - MAX_PREVIEW_PROBE);
  previewProbe.append(data, length);
}

void SubstackRssParser::appendBounded(std::string& target, const char* value, size_t length, size_t limit,
                                      bool* truncated) {
  if (target.size() >= limit) {
    if (truncated) *truncated = true;
    return;
  }
  const size_t accepted = std::min(length, limit - target.size());
  target.append(value, accepted);
  if (accepted != length && truncated) *truncated = true;
}

void XMLCALL SubstackRssParser::startElement(void* userData, const XML_Char* name, const XML_Char**) {
  auto* self = static_cast<SubstackRssParser*>(userData);
  const std::string local = localName(name);
  if (local == "channel") ++self->channelDepth;
  if (local == "item" || local == "entry") {
    ++self->seenCount;
    self->itemDepth = 1;
    self->item = {};
    self->closeBody();
    Storage.remove(ITEM_BODY_PATH);
    self->previewProbe.clear();
    self->usingEncodedContent = false;
    self->currentField.clear();
    self->currentText.clear();
    return;
  }
  if (self->itemDepth > 0) ++self->itemDepth;

  const bool inItem = self->itemDepth > 0;
  if ((local == "title" || local == "link" || local == "guid" || local == "id" || local == "pubDate" ||
       local == "published" || local == "creator" || local == "author" || local == "description" ||
       local == "encoded" || local == "summary") &&
      (inItem || (self->channelDepth > 0 && local == "title"))) {
    self->currentField = local;
    self->currentText.clear();
    if (inItem && local == "encoded") {
      self->usingEncodedContent = true;
      self->item.contentTruncated = false;
      self->beginBody(true);
    } else if (inItem && (local == "description" || local == "summary") && !self->usingEncodedContent &&
               self->item.htmlPath.empty()) {
      self->beginBody(true);
    }
  }
}

void XMLCALL SubstackRssParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<SubstackRssParser*>(userData);
  const std::string local = localName(name);
  const bool inItem = self->itemDepth > 0;

  if (!self->currentField.empty() && local == self->currentField) {
    const std::string value = trim(std::move(self->currentText));
    if (inItem) {
      if (local == "title") self->item.title = value;
      else if (local == "link") self->item.link = value;
      else if (local == "guid" || local == "id") self->item.guid = value;
      else if (local == "creator" || local == "author") self->item.author = value;
      else if (local == "pubDate" || local == "published") self->item.publishedAt = parseDate(value);
    } else if (local == "title" && self->feedTitle.empty()) {
      self->feedTitle = value;
    }
    self->currentField.clear();
    self->currentText.clear();
  }

  if (local == "item" || local == "entry") {
    self->closeBody();
    self->item.publication = self->feedTitle;
    self->item.preview = self->item.contentTruncated || containsPreviewPhrase(self->previewProbe);
    if (self->item.guid.empty()) self->item.guid = self->item.link;
    if (!self->item.title.empty() && self->item.htmlBytes > 0 && self->onItem) {
      if (self->onItem(std::move(self->item))) {
        ++self->emittedCount;
      } else {
        self->callbackAborted = true;
        XML_StopParser(self->parser, XML_FALSE);
      }
    } else {
      ++self->skippedCount;
      LOG_ERR("SUBRSS", "Skipped RSS item %u: title-bytes=%u body-bytes=%llu callback=%s",
              static_cast<unsigned>(self->seenCount), static_cast<unsigned>(self->item.title.size()),
              static_cast<unsigned long long>(self->item.htmlBytes), self->onItem ? "ready" : "missing");
    }
    Storage.remove(ITEM_BODY_PATH);
    self->item = {};
    self->itemDepth = 0;
    return;
  }
  if (self->itemDepth > 0) --self->itemDepth;
  if (local == "channel" && self->channelDepth > 0) --self->channelDepth;
}

void XMLCALL SubstackRssParser::characterData(void* userData, const XML_Char* data, int length) {
  auto* self = static_cast<SubstackRssParser*>(userData);
  if (self->currentField.empty() || length <= 0) return;
  if ((self->currentField == "encoded" || self->currentField == "description" || self->currentField == "summary") &&
      self->bodyOpen) {
    const size_t written = self->bodyFile.write(data, static_cast<size_t>(length));
    self->item.htmlBytes += written;
    self->updatePreviewProbe(data, written);
    if (written != static_cast<size_t>(length)) self->errorOccurred = true;
    return;
  }
  size_t limit = MAX_TITLE;
  if (self->currentField == "link") limit = MAX_URL;
  else if (self->currentField == "guid" || self->currentField == "id") limit = MAX_GUID;
  else if (self->currentField == "creator" || self->currentField == "author") limit = MAX_AUTHOR;
  appendBounded(self->currentText, data, static_cast<size_t>(length), limit);
}

uint64_t SubstackRssParser::parseDate(const std::string& value) {
  // RFC 822 used by Substack: "Wed, 14 Aug 2026 12:34:56 GMT".
  const char* p = value.c_str();
  const char* comma = strchr(p, ',');
  if (comma) p = comma + 1;
  while (*p == ' ') ++p;
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  char month[4] = {};
  if (sscanf(p, "%d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) < 5) {
    // ISO-8601 fallback.
    int monthNumberValue = 0;
    if (sscanf(p, "%d-%d-%dT%d:%d:%d", &year, &monthNumberValue, &day, &hour, &minute, &second) < 5) return 0;
    snprintf(month, sizeof(month), "%s", "jan");
    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = monthNumberValue - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return static_cast<uint64_t>(mktime(&tm));
  }
  std::tm tm = {};
  tm.tm_year = year - 1900;
  tm.tm_mon = monthNumber(month);
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  if (tm.tm_mon < 0) return 0;
  return static_cast<uint64_t>(mktime(&tm));
}
