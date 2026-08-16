add_executable(HtmlEntityDecoderTest
  HtmlEntityDecoderTest.cpp
  ${REPO_ROOT}/src/substack/HtmlEntityDecoder.cpp
)

target_include_directories(HtmlEntityDecoderTest PRIVATE
  ${REPO_ROOT}/src/substack
)

target_link_libraries(HtmlEntityDecoderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HtmlEntityDecoderTest)
