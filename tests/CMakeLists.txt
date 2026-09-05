add_executable(lake-tests test_web_ui.cpp
  test_apps.cpp
  test_api.cpp
  test_handles.cpp
  test_policy.cpp
  test_oauth.cpp
  test_sso.cpp
  test_webauthn.cpp
  test_search.cpp
  test_secrets.cpp
  test_offline_recovery.cpp
  test_pki.cpp
  test_agents.cpp
  test_callers.cpp
  test_transparency.cpp
  test_transparency_fixture.cpp
  test_postgres_identity.cpp)
target_link_libraries(lake-tests PRIVATE lake-api lake-control-postgres pqxx OpenSSL::Crypto GTest::gtest_main)
target_include_directories(lake-tests PRIVATE ${PROJECT_SOURCE_DIR}/modules/liquid/src)
target_compile_definitions(lake-tests PRIVATE
  LAKE_TEST_SERVER_PATH="$<TARGET_FILE:lake>"
  LAKE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_dependencies(lake-tests lake)
include(GoogleTest)
gtest_discover_tests(lake-tests)

add_executable(lake-postgres-tests test_postgres.cpp)
target_link_libraries(lake-postgres-tests PRIVATE lake-control-postgres GTest::gtest_main)
gtest_discover_tests(lake-postgres-tests)

add_test(
  NAME lake-ui-token-gates
  COMMAND ${CMAKE_SOURCE_DIR}/tools/ui/test-token-gates.sh)

add_test(
  NAME lake-ui-contrast
  COMMAND ${LAKE_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ui/check-contrast.mjs)
