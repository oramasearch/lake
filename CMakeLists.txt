cmake_minimum_required(VERSION 3.28)

# lake is C++26 on the same coroutine stack as Zetta (Capy/Corosio/Beast2), which
# needs Clang 17+ — the same pre-project() auto-selection Zetta and liquid use.
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT LAKE_USE_SYSTEM_COMPILER)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"
     AND EXISTS "/opt/homebrew/opt/llvm/bin/clang++")
    set(CMAKE_C_COMPILER   "/opt/homebrew/opt/llvm/bin/clang"   CACHE FILEPATH "")
    set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++" CACHE FILEPATH "")
    if(NOT CMAKE_OSX_SYSROOT)
      execute_process(COMMAND xcrun --show-sdk-path
                      OUTPUT_VARIABLE LAKE_SDKROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
      set(CMAKE_OSX_SYSROOT "${LAKE_SDKROOT}" CACHE PATH "")
    endif()
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    foreach(LAKE_VER 22 21 20 19 18 17)
      find_program(LAKE_CLANGXX_${LAKE_VER} "clang++-${LAKE_VER}")
      if(LAKE_CLANGXX_${LAKE_VER})
        set(CMAKE_CXX_COMPILER "${LAKE_CLANGXX_${LAKE_VER}}" CACHE FILEPATH "")
        find_program(LAKE_CLANGC_${LAKE_VER} "clang-${LAKE_VER}")
        if(LAKE_CLANGC_${LAKE_VER})
          set(CMAKE_C_COMPILER "${LAKE_CLANGC_${LAKE_VER}}" CACHE FILEPATH "")
        endif()
        break()
      endif()
    endforeach()
  endif()
endif()

project(lake VERSION 0.7.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(LAKE_BUILD_TESTS "Build the test suite" ON)
set(LAKE_BUILD_REVISION "unknown" CACHE STRING "Source revision embedded in the lake binary")

include(FetchContent)

set(LAKE_FONT_FILES
  assets/fonts/ibm-plex-sans-400.woff2
  assets/fonts/ibm-plex-sans-500.woff2
  assets/fonts/ibm-plex-sans-600.woff2
  assets/fonts/ibm-plex-mono-400.woff2
  assets/fonts/ibm-plex-mono-500.woff2)
set(LAKE_EMBEDDED_FONTS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/web_ui_fonts.cpp")
add_custom_command(
  OUTPUT "${LAKE_EMBEDDED_FONTS_CPP}"
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts"
    "-DOUTPUT_FILE=${LAKE_EMBEDDED_FONTS_CPP}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_fonts.cmake"
  DEPENDS ${LAKE_FONT_FILES} cmake/embed_fonts.cmake
  VERBATIM)

# The Docs section's guides (docs/guide/*.md, lake#252), embedded so /docs needs no files at run time.
# CONFIGURE_DEPENDS: a new page (an optional slug) is picked up without a reconfigure.
file(GLOB LAKE_GUIDE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docs/guide/*.md")
set(LAKE_EMBEDDED_GUIDE_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/web_ui_guide.cpp")
add_custom_command(
  OUTPUT "${LAKE_EMBEDDED_GUIDE_CPP}"
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/docs/guide"
    "-DOUTPUT_FILE=${LAKE_EMBEDDED_GUIDE_CPP}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_guide.cmake"
  DEPENDS ${LAKE_GUIDE_FILES} cmake/embed_guide.cmake
  VERBATIM)

# The workspace engine's input schemas for the capabilities lake allows (lake#650), vendored by
# tools/api/vendor-engine-openapi.py and embedded so each named engine capability publishes them.
set(LAKE_EMBEDDED_ENGINE_OPENAPI_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/engine_openapi.cpp")
add_custom_command(
  OUTPUT "${LAKE_EMBEDDED_ENGINE_OPENAPI_CPP}"
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/src/api/engine_openapi.json"
    "-DOUTPUT_FILE=${LAKE_EMBEDDED_ENGINE_OPENAPI_CPP}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_engine_openapi.cmake"
  DEPENDS src/api/engine_openapi.json cmake/embed_engine_openapi.cmake
  VERBATIM)

# lake's control-plane schema (db/schema.sql), embedded so `lake migrate` applies the schema this
# binary was built with and the operator vendors no copy (lake#1119, orama-operator#936).
set(LAKE_EMBEDDED_SCHEMA_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/schema_sql.cpp")
add_custom_command(
  OUTPUT "${LAKE_EMBEDDED_SCHEMA_CPP}"
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/db/schema.sql"
    "-DOUTPUT_FILE=${LAKE_EMBEDDED_SCHEMA_CPP}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake"
  DEPENDS db/schema.sql cmake/embed_schema.cmake
  VERBATIM)

# The web app (frontend/, React + Vite): built with npm and embedded into the binary, so lake
# serves its whole UI with no files at run time (docs/ui/README.md). LAKE_FRONTEND_DIST points
# at an already-built dist/ instead (the image's Node stage); empty builds it here.
set(LAKE_FRONTEND_DIST "" CACHE PATH "A built frontend/dist to embed; empty builds it with npm")
find_program(LAKE_NODE_EXECUTABLE node REQUIRED)
set(LAKE_EMBEDDED_WEB_APP_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/web_app_assets.cpp")
if(LAKE_FRONTEND_DIST)
  set(LAKE_WEB_APP_DIST "${LAKE_FRONTEND_DIST}")
  file(GLOB_RECURSE LAKE_WEB_APP_DEPENDS CONFIGURE_DEPENDS "${LAKE_FRONTEND_DIST}/*")
else()
  find_program(LAKE_NPM_EXECUTABLE npm REQUIRED)
  set(LAKE_WEB_APP_DIST "${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist")
  file(GLOB_RECURSE LAKE_FRONTEND_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/src/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/scripts/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/tests/*")
  set(LAKE_WEB_APP_STAMP "${CMAKE_CURRENT_BINARY_DIR}/frontend-dist.stamp")
  add_custom_command(
    OUTPUT "${LAKE_WEB_APP_STAMP}"
    COMMAND "${LAKE_NPM_EXECUTABLE}" ci --no-audit --no-fund
    COMMAND "${LAKE_NPM_EXECUTABLE}" run build
    COMMAND "${CMAKE_COMMAND}" -E touch "${LAKE_WEB_APP_STAMP}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/frontend"
    DEPENDS ${LAKE_FRONTEND_SOURCES}
      frontend/package.json frontend/package-lock.json frontend/index.html
      frontend/vite.config.ts frontend/tsconfig.json
    COMMENT "Building the web app (frontend/)"
    VERBATIM)
  set(LAKE_WEB_APP_DEPENDS "${LAKE_WEB_APP_STAMP}")
endif()
add_custom_command(
  OUTPUT "${LAKE_EMBEDDED_WEB_APP_CPP}"
  COMMAND "${LAKE_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/ui/embed-frontend.mjs"
    "${LAKE_WEB_APP_DIST}" "${LAKE_EMBEDDED_WEB_APP_CPP}"
  DEPENDS ${LAKE_WEB_APP_DEPENDS} tools/ui/embed-frontend.mjs
  VERBATIM)

# -----------------------------------------------------------------------------
# Sibling and vendored modules — libliquid is library-only and provides Capy, Corosio
# and HTTP to its consumers. Beast2 remains lake's Zetta-shaped serving stack.
# Order matters: OpenSSL → Boost core libs → liquid → beast2. liquid owns the
# one HTTP target shared by its sync client and lake's server.
# -----------------------------------------------------------------------------
if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED)
# http/beast2 expect the Boost:: targets to already exist (they do not self-resolve
# Boost as subdirectories), so the official boost-cmake release comes first.
set(BOOST_INCLUDE_LIBRARIES json url assert config core describe endian mp11 static_assert system throw_exception type_traits variant2 winapi)
set(BOOST_ENABLE_CMAKE ON)
FetchContent_Declare(
  boost_core_libs
  URL https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(boost_core_libs)
# liquid is not vendored here. One pin, in liquid-bridge, is the single source of truth;
# a second submodule pointer in this repo could disagree with it. The default is the
# sibling checkout in that bridge (../liquid); CI and standalone builds pass the path.
set(LAKE_LIQUID_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../liquid"
    CACHE PATH "Path to the liquid source tree providing libliquid")
if(NOT EXISTS "${LAKE_LIQUID_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "LAKE_LIQUID_SOURCE_DIR does not contain a liquid source tree: ${LAKE_LIQUID_SOURCE_DIR}\n"
    "liquid is a sibling module of liquid-bridge, not a submodule of lake. Build lake from the "
    "bridge checkout, or pass -DLAKE_LIQUID_SOURCE_DIR=<path to oramasearch/liquid>.")
endif()
set(LIQUID_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LIQUID_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${LAKE_LIQUID_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_liquid" EXCLUDE_FROM_ALL)
if(BUILD_SHARED_LIBS AND LAKE_BUILD_TESTS)
  # Existing Lake tests exercise libliquid's internal conformance helpers. A nested dependency
  # selects hidden C++ visibility globally, so expose those helpers only in shared test builds.
  set_target_properties(libliquid PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF)
endif()
# Search uses oramacore-cpp's in-memory BM25F engine. Lake owns persistence
# and the positional trigram postings in src/search/index.cpp. Liquid supplies
# the shared oramacore-cpp target at its pinned revision.
set(BOOST_BEAST2_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_BEAST2_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/beast2 EXCLUDE_FROM_ALL)
# Beast2's HTTPS implementation uses Corosio's TLS stream. Declare the dependency here because
# the vendored target omits it and shared builds cannot rely on static archive resolution.
target_link_libraries(boost_beast2 PUBLIC Boost::corosio_openssl)

# Zetta's client-only API transport is compiled from the one authorized bridge checkout and
# linked against lake's existing Boost.HTTP, Beast2, Capy, and Corosio targets. This avoids a
# second copy of the networking stack while keeping Zetta source out of this repository.
set(LAKE_ZETTA_SOURCE_DIR "" CACHE PATH "Path to the zetta source tree used for zetta-api")
if(NOT LAKE_ZETTA_SOURCE_DIR)
  message(FATAL_ERROR
    "LAKE_ZETTA_SOURCE_DIR is required; point it at the zetta-bridge modules/zetta checkout")
endif()
if(NOT EXISTS "${LAKE_ZETTA_SOURCE_DIR}/include/zetta/connect/http_transport.hpp")
  message(FATAL_ERROR
    "LAKE_ZETTA_SOURCE_DIR does not contain the required zetta-api client headers")
endif()
add_library(zetta-api STATIC
  "${LAKE_ZETTA_SOURCE_DIR}/src/core/connect/http_transport.cpp"
  "${LAKE_ZETTA_SOURCE_DIR}/src/core/connect/egress_policy.cpp"
  "${LAKE_ZETTA_SOURCE_DIR}/src/core/connect/connector_destinations.cpp"
  "${LAKE_ZETTA_SOURCE_DIR}/src/core/connect/destination_policy.cpp"
  "${LAKE_ZETTA_SOURCE_DIR}/src/core/api/native_http_transport.cpp"
  "${LAKE_ZETTA_SOURCE_DIR}/src/core/api/system_trust_store.cpp"
  "${LAKE_ZETTA_SOURCE_DIR}/src/core/runtime.cpp")
target_compile_features(zetta-api PUBLIC cxx_std_26)
target_include_directories(zetta-api PUBLIC "${LAKE_ZETTA_SOURCE_DIR}/include")
target_link_libraries(zetta-api PRIVATE
  Boost::http Boost::beast2 Boost::corosio Boost::corosio_openssl OpenSSL::SSL)

# -----------------------------------------------------------------------------
# nlohmann/json — the document type of the API layer (registry, REST envelope,
# OpenAPI, MCP). libliquid's automerge dependency provides the same CMake target.

# libpqxx — the Postgres client for the control plane, a submodule at Zetta's pin (jtv/libpqxx
# 7.10.7) on libpq. Only lake-control-postgres names it, so lake-core links no libpq.
if(APPLE)
  foreach(pg_prefix /opt/homebrew/opt/libpq /usr/local/opt/libpq)
    if(EXISTS "${pg_prefix}")
      list(APPEND CMAKE_PREFIX_PATH "${pg_prefix}")
    endif()
  endforeach()
endif()
set(SKIP_BUILD_TEST ON CACHE BOOL "" FORCE)
add_subdirectory(modules/libpqxx EXCLUDE_FROM_ALL)

# lake-core: HTTP-free. Version, the hosted sign-in page renderers, the capability registry and its
# REST / OpenAPI / MCP renderings, the control-plane store interface and the in-memory store.
add_library(lake-core STATIC
  src/control/apps.cpp
  src/control/order_entitlements.cpp
  src/control/builds.cpp
  ${LAKE_EMBEDDED_FONTS_CPP}
  ${LAKE_EMBEDDED_GUIDE_CPP}
  ${LAKE_EMBEDDED_ENGINE_OPENAPI_CPP}
  ${LAKE_EMBEDDED_SCHEMA_CPP}
  src/control/schema.cpp
  src/api/manifest.cpp
  ${LAKE_EMBEDDED_WEB_APP_CPP}
  src/api/web_app.cpp
  src/control/lake_identity.cpp
  src/control/saml.cpp
  src/control/browser_keys.cpp
  src/control/callers.cpp
  src/control/agents.cpp
  src/control/pki.cpp
  src/control/platform.cpp
  src/control/platform_records.cpp
  src/control/zetta_control.cpp
  src/control/agent_library.cpp
  src/control/agent_bus.cpp
  src/control/agent_runs.cpp
  src/control/time_zones.cpp
  src/control/agent_skills.cpp
  src/control/web_apps.cpp
  src/control/zetta_workspace.cpp
  src/control/workspace_records.cpp
  src/control/workspace_store.cpp
  src/control/ask_messages.cpp
  src/control/opportunities.cpp
  src/control/solutions.cpp
  src/control/methodologies.cpp
  src/control/methodology_templates.cpp
  src/control/transcripts.cpp
  src/control/mcp_connections.cpp
  src/control/agent_revisions.cpp
  src/control/agent_stream_triggers.cpp
  src/control/project_decisions.cpp
  src/control/drives.cpp
  src/control/drive_files.cpp
  src/control/store_contents.cpp
  src/control/store_buckets.cpp
  src/control/store_kept.cpp
  src/control/stream_owners.cpp
  src/control/drive_share_links.cpp
  src/control/forge_records.cpp
  src/control/application_user_records.cpp
  src/control/secret_records.cpp
  src/control/application_webhook_records.cpp
  src/control/application_authorization_codes.cpp
  src/control/application_credential_records.cpp
  src/control/application_settings_records.cpp
  src/control/repository_records.cpp
  src/control/oci_records.cpp
  src/control/agent_task_records.cpp
  src/control/metering.cpp
  src/control/workspace_audit.cpp
  "${LAKE_LIQUID_SOURCE_DIR}/src/scan/detectors.cpp"
  src/version.cpp
  src/api/web_ui.cpp
  src/api/markdown.cpp
  src/api/oauth.cpp
  src/api/saml.cpp
  src/api/oidc_jwks.cpp
  src/api/clerk.cpp
  src/api/workspace_capabilities.cpp
  src/api/repository_records_api.cpp
  src/api/workspace_deletion.cpp
  src/api/workspace_record_capabilities.cpp
  src/api/solution_capabilities.cpp
  src/api/quickstart_capabilities.cpp
  src/api/quickstart_columns.cpp
  src/api/browser_key_capabilities.cpp
  src/api/methodology_capabilities.cpp
  src/api/transcript_capabilities.cpp
  src/api/deletion_capabilities.cpp
  src/api/drives_capabilities.cpp
  src/api/store_repository.cpp
  src/api/model_gateway.cpp
  src/api/transcription.cpp
  src/api/mcp_client.cpp
  src/api/mcp_connection_capabilities.cpp
  src/api/mcp_connection_streams.cpp
  src/api/agent_stream_triggers.cpp
  src/api/audio_duration.cpp
  src/api/workspace_store_capabilities.cpp
  src/api/datasets.cpp
  src/api/stream_capabilities.cpp
  src/api/engine_capabilities.cpp
  src/api/experiment_capabilities.cpp
  src/api/pipeline.cpp
  src/api/registry.cpp
  src/api/store_owner.cpp
  src/api/rest.cpp
  src/api/mcp.cpp
  src/api/agent_docs.cpp
  src/api/capabilities.cpp
  src/api/clerk_webhooks.cpp
  src/api/console_capabilities.cpp
  src/api/forge_capabilities.cpp
  src/api/repository_people.cpp
  src/api/export_capabilities.cpp
  src/api/github_import_capabilities.cpp
  src/api/pki_capabilities.cpp
  src/api/metering_capabilities.cpp
  src/api/metering_recorder.cpp
  src/api/audit_recorder.cpp
  src/api/request_recorder.cpp
  src/api/usage_recorder.cpp
  src/api/usage_capabilities.cpp
  src/api/billing_capabilities.cpp
  src/api/balance.cpp
  src/api/client_address.cpp
  src/api/coupons.cpp
  src/api/stripe.cpp
  src/api/audit_routing.cpp
  src/api/audit_logs.cpp
  src/api/drive.cpp
  src/api/drive_capabilities.cpp
  src/api/lake_attestation.cpp
  src/api/model_capabilities.cpp
  src/api/transparency_capabilities.cpp
  src/api/timestamp_capabilities.cpp
  src/api/replication_capabilities.cpp
  src/api/app_capabilities.cpp
  src/api/agent_capabilities.cpp
  src/api/platform_capabilities.cpp
  src/api/web_app_capabilities.cpp
  src/api/store_mounts.cpp
  src/api/build_capabilities.cpp
  src/api/image_capabilities.cpp
  src/api/volume_capabilities.cpp
  src/api/network_capabilities.cpp
  # lake#889: beside its one user. In lake-control-postgres, which lake-core precedes on the link
  # line, GNU ld (one pass over static archives) left PendingNetworkRecords' vtable unresolved.
  src/control/network_records.cpp
  src/api/tunnel_clients.cpp
  src/api/regions.cpp
  src/api/white_label.cpp
  src/api/orders.cpp
  src/api/resend.cpp
  src/api/brand.cpp
  src/api/tunnel_client_capabilities.cpp
  src/audit/audit.cpp
  src/api/secrets_capabilities.cpp
  src/api/sealed_delivery.cpp
  src/api/secret_records_api.cpp
  src/api/agent_task_records_api.cpp
  src/api/offline_recovery_capabilities.cpp
  src/control/handles.cpp
  src/control/own_identity.cpp
  src/control/policy.cpp
  src/search/index.cpp
  src/transparency/checkpoint.cpp
  src/transparency/log.cpp
  src/transparency/tuf.cpp
  src/transparency/timestamps.cpp
  src/telemetry/metrics.cpp
)
target_include_directories(lake-core PUBLIC include
  PRIVATE "${LAKE_LIQUID_SOURCE_DIR}" "${LAKE_LIQUID_SOURCE_DIR}/src")
target_compile_features(lake-core PUBLIC cxx_std_26)
# Only version.cpp reads the revision; defining it for every lake-core source would change every
# compile command per revision and defeat the image build's compiler cache.
set_source_files_properties(src/version.cpp PROPERTIES
  COMPILE_DEFINITIONS "LAKE_BUILD_REVISION=\"${LAKE_BUILD_REVISION}\"")
target_link_libraries(lake-core
  PUBLIC nlohmann_json::nlohmann_json liquid::liquid orama::oramacore-cpp OpenSSL::Crypto
  PRIVATE liquid_frontend_syntax)
target_link_libraries(lake-core PRIVATE zetta-api lake-object-http)

# lake-object-http: the workspace store's keep-alive HTTP client for a workspace Zetta's object
# routes (lake#544). Like zetta-api it names Boost.HTTP and Corosio inside its one translation unit
# and links them PRIVATE, so no HTTP type reaches lake-core's surface.
add_library(lake-object-http STATIC src/control/object_http.cpp)
target_compile_features(lake-object-http PRIVATE cxx_std_26)
target_include_directories(lake-object-http PRIVATE include)
target_link_libraries(lake-object-http PRIVATE
  liquid::liquid zetta-api Boost::http Boost::corosio)
target_compile_options(lake-object-http PRIVATE -Werror=return-type)

# lake-control-postgres: the one library that names libpqxx and OpenSSL for persistent audit keys.
add_library(lake-control-postgres STATIC
  src/control/apps_memory.cpp
  src/control/memory_store.cpp
  src/control/network.cpp
  src/control/network_memory.cpp
  src/control/postgres_store.cpp
)
target_link_libraries(lake-control-postgres PUBLIC lake-core PRIVATE pqxx OpenSSL::Crypto)

# lake-api: the Beast2 adapter — the ONE translation unit that knows beast2 / http / Boost.JSON.
# No beast2 / http / Boost.JSON type reaches lake-core's surface — they link PRIVATE.
add_library(lake-api STATIC
  src/api/http_server.cpp
)
target_link_libraries(lake-api PUBLIC lake-core)
target_link_libraries(lake-api PRIVATE Boost::beast2 Boost::http Boost::json Boost::url Boost::corosio Boost::corosio_openssl)

add_executable(lake src/main.cpp)
target_link_libraries(lake PRIVATE lake-api lake-core lake-control-postgres)

# A coroutine returning liquid::runtime::task<T> (a result<T>) that flows off its end leaves the
# result unset, and the awaiting coroutine then reads uninitialized memory (lake#476). The compiler
# sees every such path, so it is an error rather than a warning.
foreach(lake_target lake-core lake-control-postgres lake-api lake)
  target_compile_options(${lake_target} PRIVATE -Werror=return-type)
endforeach()

if(LAKE_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
add_subdirectory(tests)
endif()
