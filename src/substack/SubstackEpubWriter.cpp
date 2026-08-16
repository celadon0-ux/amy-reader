#include "SubstackEpubWriter.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <utility>
#include <vector>

#include "SubstackRssParser.h"
#include "substack/HtmlEntityDecoder.h"

namespace {
void append16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(value & 0xff);
  out.push_back((value >> 8) & 0xff);
}
void append32(std::vector<uint8_t>& out, uint32_t value) {
  append16(out, value & 0xffff);
  append16(out, value >> 16);
}

uint32_t updateCrc(uint32_t crc, const uint8_t* data, size_t length) {
  crc = ~crc;
  while (length--) {
    crc ^= *data++;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

std::string xmlEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 32);
  for (char c : value) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

class ZipWriter {
 public:
  struct Entry {
    std::string name;
    uint32_t offset = 0;
    uint32_t crc = 0;
    uint32_t size = 0;
    bool descriptor = true;
  };

  explicit ZipWriter(HalFile& output) : file(output) {}

  bool add(const std::string& name, const std::string& value, bool descriptor = true) {
    if (!descriptor) {
      Entry entry;
      entry.name = name;
      entry.offset = static_cast<uint32_t>(file.position());
      entry.descriptor = false;
      entry.size = static_cast<uint32_t>(value.size());
      entry.crc = updateCrc(0, reinterpret_cast<const uint8_t*>(value.data()), value.size());
      std::vector<uint8_t> header;
      append32(header, 0x04034b50);
      append16(header, 20);
      append16(header, 0x0800);
      append16(header, 0);
      append16(header, 0);
      append16(header, 0);
      append32(header, entry.crc);
      append32(header, entry.size);
      append32(header, entry.size);
      append16(header, name.size());
      append16(header, 0);
      header.insert(header.end(), name.begin(), name.end());
      if (file.write(header.data(), header.size()) != header.size() ||
          file.write(value.data(), value.size()) != value.size()) return false;
      entries.push_back(std::move(entry));
      return true;
    }
    if (!begin(name, descriptor)) return false;
    if (!write(reinterpret_cast<const uint8_t*>(value.data()), value.size())) return false;
    return end();
  }

  bool begin(const std::string& name, bool descriptor = true) {
    if (active) return false;
    current = {};
    current.name = name;
    current.offset = static_cast<uint32_t>(file.position());
    current.descriptor = descriptor;
    std::vector<uint8_t> header;
    append32(header, 0x04034b50);
    append16(header, 20);
    append16(header, descriptor ? 0x0808 : 0x0800);  // UTF-8 plus optional data descriptor.
    append16(header, 0);
    append16(header, 0);
    append16(header, 0);
    append32(header, 0);
    append32(header, 0);
    append32(header, 0);
    append16(header, name.size());
    append16(header, 0);
    header.insert(header.end(), name.begin(), name.end());
    active = file.write(header.data(), header.size()) == header.size();
    return active;
  }

  bool write(const uint8_t* data, size_t length) {
    if (!active || file.write(data, length) != length) return false;
    current.crc = updateCrc(current.crc, data, length);
    current.size += static_cast<uint32_t>(length);
    return true;
  }

  bool write(const std::string& value) {
    return write(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }

  bool end() {
    if (!active) return false;
    if (current.descriptor) {
      std::vector<uint8_t> descriptor;
      append32(descriptor, 0x08074b50);
      append32(descriptor, current.crc);
      append32(descriptor, current.size);
      append32(descriptor, current.size);
      if (file.write(descriptor.data(), descriptor.size()) != descriptor.size()) return false;
    }
    entries.push_back(current);
    active = false;
    return true;
  }

  bool finish() {
    if (active) return false;
    const uint32_t centralOffset = static_cast<uint32_t>(file.position());
    for (const auto& entry : entries) {
      std::vector<uint8_t> central;
      append32(central, 0x02014b50);
      append16(central, 20);
      append16(central, 20);
      append16(central, entry.descriptor ? 0x0808 : 0x0800);
      append16(central, 0);
      append16(central, 0);
      append16(central, 0);
      append32(central, entry.crc);
      append32(central, entry.size);
      append32(central, entry.size);
      append16(central, entry.name.size());
      append16(central, 0);
      append16(central, 0);
      append16(central, 0);
      append16(central, 0);
      append32(central, 0);
      append32(central, entry.offset);
      central.insert(central.end(), entry.name.begin(), entry.name.end());
      if (file.write(central.data(), central.size()) != central.size()) return false;
    }
    const uint32_t centralSize = static_cast<uint32_t>(file.position()) - centralOffset;
    std::vector<uint8_t> endRecord;
    append32(endRecord, 0x06054b50);
    append16(endRecord, 0);
    append16(endRecord, 0);
    append16(endRecord, entries.size());
    append16(endRecord, entries.size());
    append32(endRecord, centralSize);
    append32(endRecord, centralOffset);
    append16(endRecord, 0);
    return file.write(endRecord.data(), endRecord.size()) == endRecord.size();
  }

 private:
  HalFile& file;
  std::vector<Entry> entries;
  Entry current;
  bool active = false;
};

bool isBreakTag(const std::string& tag) {
  return tag == "p" || tag == "/p" || tag == "br" || tag == "br/" || tag == "div" || tag == "/div" ||
         tag == "h1" || tag == "/h1" || tag == "h2" || tag == "/h2" || tag == "h3" || tag == "/h3" ||
         tag == "li" || tag == "/li" || tag == "blockquote" || tag == "/blockquote";
}

bool writeArticleBody(ZipWriter& zip, const std::string& htmlPath) {
  HalFile input;
  if (!Storage.openFileForRead("SUBEPUB", htmlPath, input)) return false;
  bool inTag = false;
  bool skip = false;
  bool tagNameDone = false;
  bool inEntity = false;
  bool lastSpace = true;
  std::string tag;
  std::string entity;
  std::string text;
  text.reserve(512);
  auto flush = [&]() {
    if (text.empty()) return true;
    const bool ok = zip.write(xmlEscape(text));
    text.clear();
    return ok;
  };
  auto paragraphBreak = [&]() {
    if (!flush()) return false;
    lastSpace = true;
    return zip.write("</p><p>");
  };

  auto acceptTextChar = [&](char c) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!lastSpace) text.push_back(' ');
      lastSpace = true;
    } else {
      text.push_back(c);
      lastSpace = false;
    }
    return text.size() < 512 || flush();
  };

  uint8_t buffer[1024];
  bool ok = true;
  while (ok) {
    const int count = input.read(buffer, sizeof(buffer));
    if (count < 0) {
      ok = false;
      break;
    }
    if (count == 0) break;
    for (int i = 0; i < count && ok; ++i) {
      const char c = static_cast<char>(buffer[i]);
    if (!inTag && c == '<') {
      if (!flush()) { ok = false; break; }
      inTag = true;
      tag.clear();
      tagNameDone = false;
      continue;
    }
    if (inTag) {
      if (c != '>') {
        if (!tagNameDone && std::isspace(static_cast<unsigned char>(c))) tagNameDone = true;
        else if (!tagNameDone && tag.size() < 32)
          tag.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        continue;
      }
      inTag = false;
      const size_t space = tag.find_first_of(" \t\r\n");
      if (space != std::string::npos) tag.erase(space);
      if (tag == "script" || tag == "style") skip = true;
      else if (tag == "/script" || tag == "/style") skip = false;
      else if (!skip && isBreakTag(tag) && !paragraphBreak()) { ok = false; break; }
      continue;
    }
    if (skip) continue;
    if (inEntity) {
      if (c == ';') {
        std::string decoded;
        if (decodeHtmlEntity(entity, decoded)) {
          for (const char decodedChar : decoded)
            if (ok) ok = acceptTextChar(decodedChar);
        } else {
          ok = acceptTextChar('&');
          for (char entityChar : entity) if (ok) ok = acceptTextChar(entityChar);
          if (ok) ok = acceptTextChar(';');
        }
        inEntity = false;
        entity.clear();
        continue;
      }
      if (entity.size() < 32 &&
          (std::isalnum(static_cast<unsigned char>(c)) || (entity.empty() && c == '#'))) {
        entity.push_back(c);
        continue;
      }
      ok = acceptTextChar('&');
      for (char entityChar : entity) if (ok) ok = acceptTextChar(entityChar);
      inEntity = false;
      entity.clear();
      if (!ok) break;
    }
    if (c == '&') {
      inEntity = true;
      entity.clear();
      continue;
    }
    ok = acceptTextChar(c);
    }
  }
  if (inEntity && ok) {
    ok = acceptTextChar('&');
    for (char entityChar : entity) if (ok) ok = acceptTextChar(entityChar);
  }
  if (ok) ok = flush();
  input.close();
  return ok;
}
}  // namespace

std::string SubstackEpubWriter::sanitizeFilename(const std::string& value, size_t maxLength) {
  std::string result;
  result.reserve(std::min(value.size(), maxLength));
  bool pendingSpace = false;
  for (unsigned char c : value) {
    if (result.size() >= maxLength) break;
    if (std::isalnum(c) || c >= 0x80 || c == '-' || c == '_') {
      if (pendingSpace && !result.empty() && result.size() < maxLength) result.push_back(' ');
      result.push_back(static_cast<char>(c));
      pendingSpace = false;
    } else if (std::isspace(c)) {
      pendingSpace = true;
    }
  }
  while (!result.empty() && result.back() == ' ') result.pop_back();
  return result.empty() ? "Article" : result;
}

bool SubstackEpubWriter::write(const SubstackRssItem& item, const std::string& publication,
                               const std::string& articleId, std::string& outputPath, uint64_t& outputBytes) {
  const std::string publicationDir = "/Substack/" + sanitizeFilename(publication, 48);
  if (!Storage.ensureDirectoryExists("/Substack") || !Storage.ensureDirectoryExists(publicationDir.c_str())) return false;

  char date[16] = "undated";
  const time_t timestamp = static_cast<time_t>(item.publishedAt);
  if (timestamp > 0) {
    const std::tm* tm = localtime(&timestamp);
    if (tm) strftime(date, sizeof(date), "%Y-%m-%d", tm);
  }
  outputPath = publicationDir + "/" + date + " - " + sanitizeFilename(item.title) + " - " + articleId.substr(0, 8) + ".epub";
  const std::string partPath = outputPath + ".part";
  Storage.remove(partPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("SUBEPUB", partPath, file)) return false;

  ZipWriter zip(file);
  const std::string title = xmlEscape(item.title);
  const std::string author = xmlEscape(item.author.empty() ? publication : item.author);
  const std::string identifier = xmlEscape(articleId);
  const std::string source = xmlEscape(item.link);
  bool ok = zip.add("mimetype", "application/epub+zip", false) &&
            zip.add("META-INF/container.xml",
                    "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>") &&
            zip.add("OEBPS/content.opf",
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"bookid\">" +
                        identifier + "</dc:identifier><dc:title>" + title + "</dc:title><dc:creator>" + author +
                        "</dc:creator><dc:language>en</dc:language></metadata><manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/><item id=\"article\" href=\"article.xhtml\" media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"article\"/></spine></package>") &&
            zip.add("OEBPS/nav.xhtml",
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>" + title +
                        "</title></head><body><nav epub:type=\"toc\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><ol><li><a href=\"article.xhtml\">" +
                        title + "</a></li></ol></nav></body></html>");

  if (ok) {
    ok = zip.begin("OEBPS/article.xhtml") &&
         zip.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>" +
                   title + "</title><style>body{line-height:1.4}p{margin:0 0 1em}h1{line-height:1.2}.meta{font-style:italic}</style></head><body><h1>" +
                   title + "</h1><p class=\"meta\">" + author + " — " + xmlEscape(publication) + "</p><p>") &&
         writeArticleBody(zip, item.htmlPath) &&
         zip.write("</p><hr/><p><a href=\"" + source + "\">Read the original on Substack</a></p></body></html>") &&
         zip.end() && zip.finish();
  }
  file.flush();
  outputBytes = file.fileSize64();
  file.close();
  if (!ok || outputBytes == 0) {
    Storage.remove(partPath.c_str());
    return false;
  }
  const std::string backupPath = outputPath + ".bak";
  Storage.remove(backupPath.c_str());
  const bool hadOriginal = Storage.exists(outputPath.c_str());
  if (hadOriginal && !Storage.rename(outputPath.c_str(), backupPath.c_str())) {
    Storage.remove(partPath.c_str());
    return false;
  }
  if (!Storage.rename(partPath.c_str(), outputPath.c_str())) {
    if (hadOriginal) Storage.rename(backupPath.c_str(), outputPath.c_str());
    Storage.remove(partPath.c_str());
    return false;
  }
  Storage.remove(backupPath.c_str());
  // A successful rename is not enough of a postcondition for the feed index:
  // verify that the final path is immediately openable and has the size that
  // was just written before allowing metadata to reference it.
  HalFile verified = Storage.open(outputPath.c_str());
  const bool outputValid = verified && !verified.isDirectory() && verified.fileSize64() == outputBytes;
  if (verified) verified.close();
  if (!outputValid) {
    LOG_ERR("SUBEPUB", "Final EPUB verification failed: %s expected-bytes=%llu", outputPath.c_str(),
            static_cast<unsigned long long>(outputBytes));
    Storage.remove(outputPath.c_str());
    return false;
  }
  LOG_INF("SUBEPUB", "EPUB ready: %s bytes=%llu", outputPath.c_str(),
          static_cast<unsigned long long>(outputBytes));
  return true;
}
