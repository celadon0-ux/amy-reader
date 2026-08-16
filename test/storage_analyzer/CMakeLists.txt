add_executable(StorageAnalyzerTest
  StorageAnalyzerTest.cpp
  ${REPO_ROOT}/src/util/StorageClassification.cpp
)

target_include_directories(StorageAnalyzerTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(StorageAnalyzerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StorageAnalyzerTest)
