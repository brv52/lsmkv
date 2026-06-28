cmake_minimum_required(VERSION 3.20)
project(lsmkv VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LSMKV_NATIVE "Compile with -march=native" ON)
option(LSMKV_BUILD_TESTS "Build unit tests" ON)
option(LSMKV_BUILD_BENCH "Build kvbench" ON)
option(LSMKV_BUILD_CRASH "Build crash-consistency harness" ON)
option(LSMKV_FETCH_DEPS "Download liburing / googletest when not installed" ON)
set(LSMKV_SANITIZER "" CACHE STRING "Sanitizer: '', address, thread, undefined, address+undefined")

include(GNUInstallDirs)
include(FetchContent)
include(ExternalProject)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# ---------------------------------------------------------------------------
# Compiler flags
add_compile_definitions(_GNU_SOURCE)
set(LSMKV_WARNINGS -Wall -Wextra -Wpedantic -Wshadow=local -Wnon-virtual-dtor)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG")
if(LSMKV_NATIVE)
  add_compile_options(-march=native)
endif()

if(LSMKV_SANITIZER STREQUAL "address")
  set(LSMKV_SAN_FLAGS -fsanitize=address -fno-omit-frame-pointer)
elseif(LSMKV_SANITIZER STREQUAL "thread")
  set(LSMKV_SAN_FLAGS -fsanitize=thread)
elseif(LSMKV_SANITIZER STREQUAL "undefined")
  set(LSMKV_SAN_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined)
elseif(LSMKV_SANITIZER STREQUAL "address+undefined")
  set(LSMKV_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
elseif(NOT LSMKV_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown LSMKV_SANITIZER=${LSMKV_SANITIZER}")
endif()
if(LSMKV_SAN_FLAGS)
  add_compile_options(${LSMKV_SAN_FLAGS} -g)
  add_link_options(${LSMKV_SAN_FLAGS})
endif()

# ---------------------------------------------------------------------------
# liburing: system package if present, else build a pinned release, else
# compile without the io_uring backend (Options::io_backend = kAuto then
# always selects the preadv2/pwritev2 backend).
set(LSMKV_LIBURING_TAG "liburing-2.7" CACHE STRING "liburing release to build when not installed")
option(LSMKV_REQUIRE_URING "Fail configuration if liburing is unavailable" OFF)
add_library(lsmkv_uring INTERFACE)
set(LSMKV_HAVE_URING OFF)
if(PkgConfig_FOUND)
  pkg_check_modules(URING IMPORTED_TARGET liburing>=2.2)
endif()
if(URING_FOUND)
  target_link_libraries(lsmkv_uring INTERFACE PkgConfig::URING)
  set(LSMKV_HAVE_URING ON)
  message(STATUS "liburing: system ${URING_VERSION}")
elseif(LSMKV_FETCH_DEPS)
  set(URING_PREFIX ${CMAKE_BINARY_DIR}/_deps/liburing-install)
  ExternalProject_Add(liburing_ext
    GIT_REPOSITORY https://github.com/axboe/liburing.git
    GIT_TAG ${LSMKV_LIBURING_TAG}
    GIT_SHALLOW TRUE
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND ./configure --prefix=${URING_PREFIX} --cc=${CMAKE_C_COMPILER}
    BUILD_COMMAND make -C src -j
    INSTALL_COMMAND make -C src install
    BUILD_BYPRODUCTS ${URING_PREFIX}/lib/liburing.a
    UPDATE_COMMAND "")
  file(MAKE_DIRECTORY ${URING_PREFIX}/include)
  add_library(lsmkv_uring_static STATIC IMPORTED GLOBAL)
  set_target_properties(lsmkv_uring_static PROPERTIES
    IMPORTED_LOCATION ${URING_PREFIX}/lib/liburing.a
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${URING_PREFIX}/include)
  add_dependencies(lsmkv_uring_static liburing_ext)
  target_link_libraries(lsmkv_uring INTERFACE lsmkv_uring_static)
  set(LSMKV_HAVE_URING ON)
  message(STATUS "liburing: building ${LSMKV_LIBURING_TAG} from source")
elseif(LSMKV_REQUIRE_URING)
  message(FATAL_ERROR "liburing not found (install liburing-dev or enable LSMKV_FETCH_DEPS)")
else()
  message(WARNING "liburing not found: building WITHOUT the io_uring backend")
endif()
if(LSMKV_HAVE_URING)
  target_compile_definitions(lsmkv_uring INTERFACE LSMKV_HAVE_URING=1)
endif()

# ---------------------------------------------------------------------------
# Engine library
set(LSMKV_SOURCES
  src/util/crc32c.cpp
  src/util/file_util.cpp
  src/io/iobackend.cpp
  src/io/pread.cpp
  src/io/uring.cpp
  src/wal/log_writer.cpp
  src/wal/log_reader.cpp
  src/memtable/memtable.cpp
  src/sstable/block_builder.cpp
  src/sstable/table_builder.cpp
  src/sstable/table_reader.cpp
  src/cache/lru_cache.cpp
  src/manifest/version_set.cpp
  src/compaction/compaction.cpp
  src/db_impl.cpp
)

add_library(lsmkv_objects OBJECT ${LSMKV_SOURCES})
target_include_directories(lsmkv_objects
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(lsmkv_objects PRIVATE ${LSMKV_WARNINGS})
target_link_libraries(lsmkv_objects PUBLIC lsmkv_uring Threads::Threads)

add_library(lsmkv_static STATIC $<TARGET_OBJECTS:lsmkv_objects>)
set_target_properties(lsmkv_static PROPERTIES OUTPUT_NAME lsmkv)
add_library(lsmkv_shared SHARED $<TARGET_OBJECTS:lsmkv_objects>)
set_target_properties(lsmkv_shared PROPERTIES OUTPUT_NAME lsmkv VERSION ${PROJECT_VERSION} SOVERSION 0)
foreach(t lsmkv_static lsmkv_shared)
  target_include_directories(${t}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${t} PUBLIC Threads::Threads PRIVATE lsmkv_uring)
endforeach()

install(TARGETS lsmkv_static lsmkv_shared)
install(DIRECTORY include/lsm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Internal headers are needed by tests and tools that poke at sub-layers.
add_library(lsmkv_internal INTERFACE)
target_include_directories(lsmkv_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsmkv_internal INTERFACE lsmkv_static lsmkv_uring)

# ---------------------------------------------------------------------------
# Benchmark
if(LSMKV_BUILD_BENCH)
  add_executable(kvbench bench/kvbench.cpp)
  target_compile_options(kvbench PRIVATE ${LSMKV_WARNINGS})
  target_link_libraries(kvbench PRIVATE lsmkv_internal)
endif()

# ---------------------------------------------------------------------------
# Crash-consistency harness
if(LSMKV_BUILD_CRASH)
  # LD_PRELOAD shims must not be sanitized (they run before the runtime).
  add_library(eio_shim MODULE crash/eio_shim.c)
  target_compile_options(eio_shim PRIVATE -Wall -Wextra -Wpedantic -fno-sanitize=all)
  target_link_options(eio_shim PRIVATE -fno-sanitize=all)
  target_link_libraries(eio_shim PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(eio_shim PROPERTIES PREFIX "lib")

  add_library(lsmkv_tracer MODULE crash/tracer.cpp)
  target_compile_options(lsmkv_tracer PRIVATE ${LSMKV_WARNINGS} -fno-sanitize=all)
  target_link_options(lsmkv_tracer PRIVATE -fno-sanitize=all)
  target_link_libraries(lsmkv_tracer PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  set_target_properties(lsmkv_tracer PROPERTIES PREFIX "lib")

  add_executable(crash_replay crash/replay.cpp)
  target_compile_options(crash_replay PRIVATE ${LSMKV_WARNINGS})
  target_link_libraries(crash_replay PRIVATE lsmkv_internal)
  target_compile_definitions(crash_replay PRIVATE
    LSMKV_EIO_SHIM_PATH="$<TARGET_FILE:eio_shim>"
    LSMKV_TRACER_PATH="$<TARGET_FILE:lsmkv_tracer>")
  add_dependencies(crash_replay eio_shim lsmkv_tracer)
endif()

# ---------------------------------------------------------------------------
# Unit tests
if(LSMKV_BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    # FETCHCONTENT_SOURCE_DIR_GOOGLETEST=<path> uses a local checkout instead
    # of downloading.
    if(NOT LSMKV_FETCH_DEPS AND NOT FETCHCONTENT_SOURCE_DIR_GOOGLETEST)
      message(FATAL_ERROR "googletest not found")
    endif()
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
      URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(googletest)
    if(NOT TARGET GTest::gtest_main)
      add_library(GTest::gtest_main ALIAS gtest_main)
    endif()
  endif()

  enable_testing()
  include(GoogleTest)
  foreach(name test_skiplist test_wal test_sstable test_recovery)
    add_executable(${name} tests/${name}.cpp)
    target_compile_options(${name} PRIVATE ${LSMKV_WARNINGS})
    target_link_libraries(${name} PRIVATE lsmkv_internal GTest::gtest_main)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)
  endforeach()
  if(LSMKV_BUILD_CRASH)
    add_test(NAME crash_eio COMMAND crash_replay eio --dir ${CMAKE_BINARY_DIR}/crash_eio)
    add_test(NAME crash_kill COMMAND crash_replay kill --dir ${CMAKE_BINARY_DIR}/crash_kill)
    add_test(NAME crash_trace COMMAND crash_replay trace --dir ${CMAKE_BINARY_DIR}/crash_trace)
    set_tests_properties(crash_eio crash_kill crash_trace PROPERTIES TIMEOUT 900)
  endif()
endif()
