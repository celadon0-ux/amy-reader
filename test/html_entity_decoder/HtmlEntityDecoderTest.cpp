#include <gtest/gtest.h>

#include <string>

#include "HtmlEntityDecoder.h"

TEST(HtmlEntityDecoder, DecodesDecimalRightApostrophe) {
  std::string decoded;
  ASSERT_TRUE(decodeHtmlEntity("#8217", decoded));
  EXPECT_EQ(decoded, "\xe2\x80\x99");
}

TEST(HtmlEntityDecoder, DecodesHexadecimalUnicode) {
  std::string decoded;
  ASSERT_TRUE(decodeHtmlEntity("#x2019", decoded));
  EXPECT_EQ(decoded, "\xe2\x80\x99");
}

TEST(HtmlEntityDecoder, DecodesCommonTypographicNames) {
  std::string decoded;
  ASSERT_TRUE(decodeHtmlEntity("mdash", decoded));
  EXPECT_EQ(decoded, "\xe2\x80\x94");
  ASSERT_TRUE(decodeHtmlEntity("hellip", decoded));
  EXPECT_EQ(decoded, "\xe2\x80\xa6");
}

TEST(HtmlEntityDecoder, RejectsInvalidCodepointsAndUnknownNames) {
  std::string decoded;
  EXPECT_FALSE(decodeHtmlEntity("#0", decoded));
  EXPECT_FALSE(decodeHtmlEntity("#xD800", decoded));
  EXPECT_FALSE(decodeHtmlEntity("#1114112", decoded));
  EXPECT_FALSE(decodeHtmlEntity("not-a-real-entity", decoded));
}
