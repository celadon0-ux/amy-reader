#pragma once

#include <string>
#include <string_view>

// Decodes the contents between '&' and ';'. Returns false for unknown or
// invalid entities so callers can preserve the source text verbatim.
bool decodeHtmlEntity(std::string_view entity, std::string& decoded);
