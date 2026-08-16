#include <gtest/gtest.h>

#include "util/StorageAnalyzer.h"

TEST(StorageAnalyzerTest, ClassifiesReaderContentCaseInsensitively) {
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/Books/NOVEL.EPUB"), StorageCategory::Books);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/notes/reading.MD"), StorageCategory::Books);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/screenshots/Page.PNG"), StorageCategory::Images);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/.fonts/Family/Bold.TTF"), StorageCategory::Fonts);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/dictionaries/webster/dict.dz"), StorageCategory::Dictionaries);
}

TEST(StorageAnalyzerTest, GivesReaderManagedFoldersPriorityOverExtensions) {
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/.crosspoint/epub_123/cover.png"), StorageCategory::ReadingCache);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/.crosspoint/txt_notes/page.txt"), StorageCategory::ReadingCache);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/fonts/license.txt"), StorageCategory::Fonts);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/dictionaries/readme.txt"), StorageCategory::Dictionaries);
}

TEST(StorageAnalyzerTest, SeparatesSubstackArticleEpubsFromBooks) {
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/Substack/article.epub"), StorageCategory::Substack);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/Substack/Publication/article.epub"), StorageCategory::Substack);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/SUBSTACK/Publication/ARTICLE.EPUB"), StorageCategory::Substack);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/Substack/Publication/notes.txt"), StorageCategory::Books);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/Substack/Publication/notes.json"), StorageCategory::Other);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/.crosspoint/substack/articles/id.json"), StorageCategory::Other);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/Substackish/Publication/article.epub"), StorageCategory::Books);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/Books/article.epub"), StorageCategory::Books);
}

TEST(StorageAnalyzerTest, LeavesUnknownFilesInOther) {
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/music/album.flac"), StorageCategory::Other);
  EXPECT_EQ(StorageAnalyzer::categoryForPath("/archive/book.epub.bak"), StorageCategory::Other);
}

TEST(StorageAnalyzerTest, OnlyOffersSafeUserContentForDeletion) {
  EXPECT_TRUE(StorageAnalyzer::canDelete(StorageCategory::Books, "/Books/novel.epub"));
  EXPECT_TRUE(StorageAnalyzer::canDelete(StorageCategory::Substack, "/Substack/Publication/article.epub"));
  EXPECT_TRUE(StorageAnalyzer::canDelete(StorageCategory::Substack, "/SUBSTACK/Publication/ARTICLE.EPUB"));
  EXPECT_FALSE(StorageAnalyzer::canDelete(StorageCategory::Substack, "/Books/article.epub"));
  EXPECT_FALSE(StorageAnalyzer::canDelete(StorageCategory::Substack, "/Substack/Publication/notes.txt"));
  EXPECT_TRUE(StorageAnalyzer::canDelete(StorageCategory::Images, "/screenshots/page.bmp"));
  EXPECT_TRUE(StorageAnalyzer::canDelete(StorageCategory::Images, "/SCREENSHOTS/page.PNG"));
  EXPECT_FALSE(StorageAnalyzer::canDelete(StorageCategory::Images, "/covers/page.png"));
  EXPECT_FALSE(StorageAnalyzer::canDelete(StorageCategory::Fonts, "/fonts/reader.ttf"));
  EXPECT_FALSE(StorageAnalyzer::canDelete(StorageCategory::Dictionaries, "/dictionaries/en/dict.dz"));
  EXPECT_FALSE(StorageAnalyzer::canDelete(StorageCategory::ReadingCache, "/.crosspoint/epub_1/page"));
  EXPECT_FALSE(StorageAnalyzer::canDelete(StorageCategory::Other, "/system.dat"));
}

TEST(StorageAnalyzerTest, FormatsStorageAmountsForTheInterface) {
  EXPECT_EQ(StorageAnalyzer::formatBytes(512), "512 B");
  EXPECT_EQ(StorageAnalyzer::formatBytes(1536), "1.5 KB");
  EXPECT_EQ(StorageAnalyzer::formatBytes(2 * 1024 * 1024), "2.0 MB");
  EXPECT_EQ(StorageAnalyzer::formatBytes(3ULL * 1024 * 1024 * 1024), "3.0 GB");
}

TEST(StorageAnalyzerTest, FormatsLogicalCountsWithSingularAndPluralLabels) {
  struct Labels {
    const char* singular;
    const char* plural;
  };
  constexpr Labels labels[] = {{"Book", "Books"},
                               {"Substack Article", "Substack Articles"},
                               {"Image", "Images"},
                               {"Font", "Fonts"},
                               {"Dictionary", "Dictionaries"},
                               {"Cached Book", "Cached Books"},
                               {"Other Item", "Other Items"}};

  for (const auto& label : labels) {
    EXPECT_EQ(StorageAnalyzer::formatItemCount(0, label.singular, label.plural),
              std::string("0 ") + label.plural);
    EXPECT_EQ(StorageAnalyzer::formatItemCount(1, label.singular, label.plural),
              std::string("1 ") + label.singular);
    EXPECT_EQ(StorageAnalyzer::formatItemCount(5, label.singular, label.plural),
              std::string("5 ") + label.plural);
  }
}

TEST(StorageAnalyzerTest, CountsOnlyTopLevelBookCacheDirectoriesAsLogicalGroups) {
  EXPECT_TRUE(StorageAnalyzer::isReadingCacheGroupPath("/.crosspoint/epub_123"));
  EXPECT_TRUE(StorageAnalyzer::isReadingCacheGroupPath("/.CROSSPOINT/TXT_notes"));
  EXPECT_TRUE(StorageAnalyzer::isReadingCacheGroupPath("/.crosspoint/xtc_book"));
  EXPECT_FALSE(StorageAnalyzer::isReadingCacheGroupPath("/.crosspoint/epub_123/pages"));
  EXPECT_FALSE(StorageAnalyzer::isReadingCacheGroupPath("/.crosspoint/metadata"));
  EXPECT_FALSE(StorageAnalyzer::isReadingCacheGroupPath("/backups/epub_123"));
}
