find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import jsonschema"
  RESULT_VARIABLE LAKE_JSONSCHEMA_RESULT)
if(NOT LAKE_JSONSCHEMA_RESULT EQUAL 0)
  message(FATAL_ERROR "Lake schema conformance tests require Python jsonschema")
endif()

add_executable(lake-tests test_main.cpp test_web_ui.cpp test_web_app.cpp test_markdown.cpp
  test_examples.cpp
  test_audit_isolation.cpp
  test_instance_origin.cpp
  test_schema_conformance.cpp
  scenarios/drive_test.cpp
  test_apps.cpp
  test_api.cpp
  test_http_server.cpp
  test_http_connections.cpp
  test_forge_scenario.cpp
  test_handles.cpp
  test_policy.cpp
  test_oauth.cpp
  test_clerk.cpp
  test_workspaces.cpp
  test_workspace_records.cpp
  test_transcripts.cpp
  test_solutions.cpp
  test_drives.cpp
  test_request_series.cpp
  test_usage.cpp
  test_usage_contract.cpp
  test_audio_duration.cpp
  test_billing.cpp
  test_stripe.cpp
  test_sealed_delivery.cpp
  test_workspace_deletion.cpp
  test_deletions.cpp
  test_workspace_store.cpp
  test_object_http.cpp
  test_registry_store.cpp
  test_agent_leases.cpp
  test_web_apps.cpp
  test_builds.cpp
  test_streams.cpp
  test_experiments.cpp
  test_pipeline.cpp
  test_sso.cpp
  test_webauthn.cpp
  test_search.cpp
  test_shared_store.cpp
  test_thread.cpp
  test_secrets.cpp
  test_offline_recovery.cpp
  test_pki.cpp
  test_agents.cpp
  test_callers.cpp
  test_transparency.cpp
  test_transparency_fixture.cpp
  test_platform.cpp
  test_identity_exit.cpp
  test_postgres_identity.cpp
  test_token_attenuation.cpp
  test_docs_status.cpp
  test_volumes.cpp)
target_link_libraries(lake-tests PRIVATE lake-api lake-control-postgres pqxx OpenSSL::Crypto
  Boost::beast2 Boost::http Boost::corosio GTest::gtest
  # zetta-api for a fake Zetta HTTP transport under ZettaWorkspaceCaller (test_workspaces.cpp).
  zetta-api)
# src/ for the Beast2 adapter's private headers (api/route_guard.hpp).
target_include_directories(lake-tests PRIVATE "${LAKE_LIQUID_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(lake-tests PRIVATE
  LAKE_TEST_PYTHON="${Python3_EXECUTABLE}"
  LAKE_TEST_NODE="${LAKE_NODE_EXECUTABLE}"
  LAKE_TEST_CMAKE_COMMAND="${CMAKE_COMMAND}"
  LAKE_TEST_SERVER_PATH="$<TARGET_FILE:lake>"
  LAKE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_dependencies(lake-tests lake)
include(GoogleTest)
gtest_discover_tests(lake-tests)

add_executable(lake-postgres-tests test_main.cpp test_postgres.cpp test_operator_contract.cpp)
target_link_libraries(lake-postgres-tests PRIVATE lake-control-postgres pqxx OpenSSL::Crypto GTest::gtest)
target_compile_definitions(lake-postgres-tests PRIVATE
  LAKE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
gtest_discover_tests(lake-postgres-tests)

add_executable(lake-debug-fixture debug_fixture.cpp)
target_compile_options(lake-debug-fixture PRIVATE -g)
target_compile_definitions(lake-tests PRIVATE
  LAKE_TEST_DEBUG_FIXTURE_PATH="$<TARGET_FILE:lake-debug-fixture>")
add_dependencies(lake-tests lake-debug-fixture)

add_test(
  NAME lake-ui-token-gates
  COMMAND ${CMAKE_SOURCE_DIR}/tools/ui/test-token-gates.sh)

add_test(
  NAME lake-ui-contrast
  COMMAND ${LAKE_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ui/check-contrast.mjs)

# The fakes are coroutines too: one that flows off its end is an error, as in lake's own targets.
foreach(lake_test_target lake-tests lake-postgres-tests)
  target_compile_options(${lake_test_target} PRIVATE -Werror=return-type)
endforeach()
