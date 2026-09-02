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
# Vendored modules — the SAME stack, at the SAME pins, as Zetta (modules/).
# Capy: coroutine foundation. Corosio: networking (private oramasearch carrier).
# http + beast2: sans-I/O HTTP + router, and the coroutine-native HTTP/1 server.
# Order matters: capy → OpenSSL → corosio → Boost core libs → http → beast2.
# -----------------------------------------------------------------------------
set(BOOST_CAPY_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_BENCH    OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/capy EXCLUDE_FROM_ALL)

if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED)
set(BOOST_COROSIO_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_PERF     OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/corosio EXCLUDE_FROM_ALL)

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
set(BOOST_HTTP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_HTTP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/http EXCLUDE_FROM_ALL)
set(BOOST_BEAST2_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_BEAST2_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/beast2 EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# lake-core: HTTP-free. Version, and the pure web_ui renderers (data in, HTML out).
# -----------------------------------------------------------------------------
add_library(lake-core STATIC
  src/version.cpp
  src/api/web_ui.cpp
)
target_include_directories(lake-core PUBLIC include)
target_compile_features(lake-core PUBLIC cxx_std_26)

# -----------------------------------------------------------------------------
# lake-api: the ONE Beast2 adapter TU, exactly as zetta-api is laid out. No
# beast2 / http / Boost.JSON type reaches lake-core's surface — they link PRIVATE.
# -----------------------------------------------------------------------------
add_library(lake-api STATIC
  src/api/http_server.cpp
)
target_compile_features(lake-api PUBLIC cxx_std_26)
target_link_libraries(lake-api PUBLIC lake-core)
target_link_libraries(lake-api PRIVATE
  Boost::beast2
  Boost::http
  Boost::json
  Boost::corosio
  Boost::corosio_openssl)

add_executable(lake src/main.cpp)
target_link_libraries(lake PRIVATE lake-api)

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
