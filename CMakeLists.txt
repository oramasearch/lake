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

project(lake VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(LAKE_BUILD_TESTS "Build the test suite" ON)

include(FetchContent)

# -----------------------------------------------------------------------------
# Vendored modules — libliquid is library-only and provides Capy, Corosio and
# HTTP to its consumers. Beast2 remains lake's Zetta-shaped serving stack.
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
set(LIQUID_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LIQUID_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/liquid EXCLUDE_FROM_ALL)
# Search uses oramacore-cpp's in-memory BM25F engine. Lake owns persistence
# and the positional trigram postings in src/search/index.cpp. Liquid supplies
# the shared oramacore-cpp target at its pinned revision.
set(BOOST_BEAST2_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_BEAST2_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/beast2 EXCLUDE_FROM_ALL)

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

# lake-core: HTTP-free. Version, the pure web_ui renderers, the capability registry and its
# REST / OpenAPI / MCP renderings, the control-plane store interface and the in-memory store.
add_library(lake-core STATIC
  src/control/apps.cpp
  src/control/lake_identity.cpp
  src/control/saml.cpp
  src/control/callers.cpp
  src/control/pki.cpp
  src/version.cpp
  src/api/web_ui.cpp
  src/api/oauth.cpp
  src/api/registry.cpp
  src/api/rest.cpp
  src/api/mcp.cpp
  src/api/capabilities.cpp
  src/api/forge_capabilities.cpp
  src/api/export_capabilities.cpp
  src/api/github_import_capabilities.cpp
  src/api/pki_capabilities.cpp
  src/api/metering_capabilities.cpp
  src/api/drive.cpp
  src/api/drive_capabilities.cpp
  src/api/model_capabilities.cpp
  src/api/transparency_capabilities.cpp
  src/api/app_capabilities.cpp
  src/audit/audit.cpp
  src/api/secrets_capabilities.cpp
  src/api/offline_recovery_capabilities.cpp
  src/control/handles.cpp
  src/control/policy.cpp
  src/search/index.cpp
  src/transparency/checkpoint.cpp
  src/transparency/log.cpp
  src/transparency/tuf.cpp
  src/telemetry/metrics.cpp
)
target_include_directories(lake-core PUBLIC include)
target_compile_features(lake-core PUBLIC cxx_std_26)
target_link_libraries(lake-core PUBLIC nlohmann_json::nlohmann_json liquid::liquid orama::oramacore-cpp OpenSSL::Crypto)

# lake-control-postgres: the one library that names libpqxx and OpenSSL for persistent audit keys.
add_library(lake-control-postgres STATIC
  src/control/apps_memory.cpp
  src/control/memory_store.cpp
  src/control/postgres_store.cpp
)
target_link_libraries(lake-control-postgres PUBLIC lake-core PRIVATE pqxx OpenSSL::Crypto)

# lake-api: the Beast2 adapter — the ONE translation unit that knows beast2 / http / Boost.JSON.
# No beast2 / http / Boost.JSON type reaches lake-core's surface — they link PRIVATE.
add_library(lake-api STATIC
  src/api/http_server.cpp
)
target_link_libraries(lake-api PUBLIC lake-core)
target_link_libraries(lake-api PRIVATE Boost::beast2 Boost::http Boost::json Boost::corosio Boost::corosio_openssl)

add_executable(lake src/main.cpp)
target_link_libraries(lake PRIVATE lake-api lake-core lake-control-postgres)

if(LAKE_BUILD_TESTS)
  find_program(LAKE_NODE_EXECUTABLE node REQUIRED)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
add_subdirectory(tests)
endif()
