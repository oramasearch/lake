add_executable(lake-tests test_web_ui.cpp
  test_api.cpp
  test_handles.cpp
  test_policy.cpp
  test_oauth.cpp
  test_webauthn.cpp
  test_search.cpp
  test_postgres_identity.cpp)
target_link_libraries(lake-tests PRIVATE lake-api lake-control-postgres OpenSSL::Crypto GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(lake-tests)

add_executable(lake-postgres-tests test_postgres.cpp)
target_link_libraries(lake-postgres-tests PRIVATE lake-control-postgres GTest::gtest_main)
gtest_discover_tests(lake-postgres-tests)
