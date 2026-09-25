# gitfs — read-only git-to-FUSE filesystem (RFC 0000 §5).
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

project(gitfs
  VERSION 0.0.1
  DESCRIPTION "Mount a git repository as a read-only FUSE filesystem"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3>=3.10)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2>=1.4)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warnings (RFC 0000 §5: -Wall -Wextra -Wpedantic; -Werror on CI)
# ---------------------------------------------------------------------------
option(GITFS_WERROR "Treat warnings as errors" OFF)

add_library(gitfs_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(gitfs_warnings INTERFACE
    -Wall -Wextra -Wpedantic)
  if(GITFS_WERROR)
    target_compile_options(gitfs_warnings INTERFACE -Werror)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Catch2's TEST_CASE macros use __COUNTER__, which recent clang flags
    # as a C2y extension under -Wpedantic (third-party macro, not our
    # code). Listed last so it survives the group enables above.
    target_compile_options(gitfs_warnings INTERFACE -Wno-c2y-extensions)
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library + the mount(8) helper executable
# ---------------------------------------------------------------------------
add_library(gitfs_core STATIC
  src/errmap.cpp
  src/gitfs.cpp
  src/gitrepo.cpp
  src/ino_registry.cpp
  src/log.cpp
  src/options.cpp
  src/path_map.cpp)
target_include_directories(gitfs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gitfs_core
  PUBLIC PkgConfig::FUSE3 PkgConfig::LIBGIT2 Threads::Threads
  PRIVATE gitfs_warnings)

add_executable(mount.gitfs src/main.cpp)
target_link_libraries(mount.gitfs PRIVATE gitfs_core gitfs_warnings)

# ---------------------------------------------------------------------------
# Tests (RFC 0000 §5: Catch2 v3 via FetchContent; integration needs /dev/fuse)
# ---------------------------------------------------------------------------
option(GITFS_BUILD_TESTS "Build unit tests" ON)

if(GITFS_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1)
  FetchContent_MakeAvailable(Catch2)

  add_executable(gitfs_tests
    tests/unit/test_path_map.cpp
    tests/unit/test_object_cache.cpp
    tests/unit/test_errmap.cpp
    tests/unit/test_ino_registry.cpp
    tests/unit/test_options.cpp
    tests/unit/test_gitmodules.cpp
    tests/unit/test_gitfs_util.cpp)
  target_link_libraries(gitfs_tests
    PRIVATE gitfs_core Catch2::Catch2WithMain gitfs_warnings)
  add_test(NAME unit COMMAND gitfs_tests)

  # Integration tests run real FUSE mounts; the script skips (exit 0) when
  # /dev/fuse or fusermount3 is unavailable (RFC 0000 §5).
  add_test(NAME integration
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_tests.sh
            $<TARGET_FILE:mount.gitfs>)
  set_tests_properties(integration PROPERTIES TIMEOUT 300)
endif()

# ---------------------------------------------------------------------------
# Install (RFC 0000 §3.7/§5: $(sbindir)/mount.gitfs + $(mandir)/man8)
# ---------------------------------------------------------------------------
install(TARGETS mount.gitfs DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES docs/mount.gitfs.8 DESTINATION ${CMAKE_INSTALL_MANDIR}/man8)
