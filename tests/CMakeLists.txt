add_executable(lake-tests test_web_ui.cpp)
target_link_libraries(lake-tests PRIVATE lake-core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(lake-tests)
