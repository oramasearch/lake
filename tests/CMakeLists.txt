add_executable(lake-tests test_web_ui.cpp
  test_api.cpp
  test_handles.cpp
  test_search.cpp)
target_link_libraries(lake-tests PRIVATE lake-api GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(lake-tests)
