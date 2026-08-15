#include "HtmlEntityDecoder.h"

#include <cstdint>

namespace {
void appendUtf8(const uint32_t codepoint, std::string& output) {
  if (codepoint <= 0x7f) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

bool parseNumericEntity(const std::string_view entity, uint32_t& codepoint) {
  if (entity.size() < 2 || entity.front() != '#') return false;
  size_t position = 1;
  uint32_t base = 10;
  if (position < entity.size() && (entity[position] == 'x' || entity[position] == 'X')) {
    base = 16;
    ++position;
  }
  if (position == entity.size()) return false;

  uint32_t value = 0;
  for (; position < entity.size(); ++position) {
    const char c = entity[position];
    uint32_t digit = 0;
    if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
    else if (base == 16 && c >= 'a' && c <= 'f') digit = 10U + static_cast<uint32_t>(c - 'a');
    else if (base == 16 && c >= 'A' && c <= 'F') digit = 10U + static_cast<uint32_t>(c - 'A');
    else return false;
    if (digit >= base || value > (0x10ffffU - digit) / base) return false;
    value = value * base + digit;
  }
  if (value == 0 || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) return false;
  codepoint = value;
  return true;
}
}  // namespace

bool decodeHtmlEntity(const std::string_view entity, std::string& decoded) {
  decoded.clear();
  uint32_t codepoint = 0;
  if (parseNumericEntity(entity, codepoint)) {
    appendUtf8(codepoint, decoded);
    return true;
  }

  struct NamedEntity {
    std::string_view name;
    uint32_t codepoint;
  };
  static constexpr NamedEntity entities[] = {
      {"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},
      {"apos", '\''},   {"nbsp", 0x00a0}, {"copy", 0x00a9}, {"reg", 0x00ae},
      {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
      {"ldquo", 0x201c}, {"rdquo", 0x201d}, {"bull", 0x2022},  {"hellip", 0x2026},
      {"trade", 0x2122},
  };
  for (const auto& named : entities) {
    if (entity == named.name) {
      appendUtf8(named.codepoint, decoded);
      return true;
    }
  }
  return false;
}
