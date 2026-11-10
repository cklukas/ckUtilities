include_guard(GLOBAL)

# This is the released ckVision commit validated for ckUtilities. It is a compatibility
# record for development and CI, not a source-tree dependency: consumers must
# find an installed package through CMAKE_PREFIX_PATH or the normal CMake
# package search paths.
set(CKTOOLS_CKVISION_BASELINE_COMMIT
    "0d81e2eb81d5ba2c2fd6420189b7498e829ebd9f"
    CACHE STRING
    "ckVision commit required by the current ckUtilities migration targets")

set(CKTOOLS_CKVISION_PREFIX "" CACHE PATH
    "Installation prefix of a clean ckVision SDK used by the package-consumer verification")

option(CKTOOLS_VERIFY_CKVISION_PACKAGE
       "Enable the independent ckVision package-consumer verification target"
       OFF)

set(CKTOOLS_CKVISION_CONSUMER_BUILD_TYPE "Release" CACHE STRING
    "Build type used by the independent ckVision package consumer")
set_property(CACHE CKTOOLS_CKVISION_CONSUMER_BUILD_TYPE PROPERTY STRINGS
             Debug Release RelWithDebInfo MinSizeRel)

option(CKTOOLS_VERIFY_CKVISION_INSTALL
       "Enable the installed native-executable verification target"
       OFF)

option(CKTOOLS_VERIFY_CKVISION_ARCHIVE
       "Enable verification of the packaged ckVision-native release archive"
       OFF)

option(CKTOOLS_VERIFY_CKVISION_TERMINAL
       "Enable real-PTY terminal-profile verification for the staged native product"
       OFF)

function(cktools_require_ckvision)
  if(CKTOOLS_CKVISION_PREFIX)
    # A qualification or release build that names an SDK must not silently
    # resolve a different package from a system prefix.  PATHS plus
    # NO_DEFAULT_PATH keeps the dependency provenance bound to the requested
    # installed SDK while still allowing ordinary developer builds to use the
    # platform's normal package search below.
    get_filename_component(_ckvision_prefix
                           "${CKTOOLS_CKVISION_PREFIX}" ABSOLUTE
                           BASE_DIR "${CMAKE_SOURCE_DIR}")
    find_package(ckvision 1.5.0 CONFIG REQUIRED
                 PATHS "${_ckvision_prefix}" NO_DEFAULT_PATH)
  else()
    find_package(ckvision 1.5.0 CONFIG REQUIRED)
  endif()

  if(NOT TARGET ckvision::cvision)
    message(FATAL_ERROR
            "The ckVision package was found but does not provide the required "
            "ckvision::cvision target.")
  endif()
endfunction()

function(cktools_add_ckvision_package_verification)
  if(NOT CKTOOLS_VERIFY_CKVISION_PACKAGE)
    return()
  endif()

  if(NOT CKTOOLS_CKVISION_PREFIX)
    message(FATAL_ERROR
            "CKTOOLS_VERIFY_CKVISION_PACKAGE requires CKTOOLS_CKVISION_PREFIX "
            "to name an installed ckVision SDK.")
  endif()

  add_custom_target(verify_ckvision_package
    COMMAND "${CMAKE_COMMAND}"
      "-DCKTOOLS_CKVISION_PREFIX=${CKTOOLS_CKVISION_PREFIX}"
      "-DCKTOOLS_CKVISION_CONSUMER_SOURCE_DIR=${PROJECT_SOURCE_DIR}/tests/integration/ckvision_consumer"
      "-DCKTOOLS_CKVISION_CONSUMER_BINARY_DIR=${PROJECT_BINARY_DIR}/ckvision-package-consumer"
      "-DCKTOOLS_CKVISION_CONSUMER_BUILD_TYPE=${CKTOOLS_CKVISION_CONSUMER_BUILD_TYPE}"
      "-DCKTOOLS_CKVISION_CONSUMER_GENERATOR=${CMAKE_GENERATOR}"
      "-DCKTOOLS_CKVISION_CONSUMER_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
      "-DCKTOOLS_CKVISION_CONSUMER_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
      "-DCKTOOLS_CKVISION_CONSUMER_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}"
      -P "${PROJECT_SOURCE_DIR}/cmake/VerifyCkVisionConsumer.cmake"
    USES_TERMINAL
    COMMENT "Configure, build, and run an independent ckVision package consumer")
endfunction()

function(cktools_add_ckvision_install_verification)
  if(NOT CKTOOLS_VERIFY_CKVISION_INSTALL)
    return()
  endif()

  add_custom_target(verify_ckvision_install
    COMMAND "${CMAKE_COMMAND}"
      "-DCKTOOLS_BINARY_DIR=${PROJECT_BINARY_DIR}"
      "-DCKTOOLS_INSTALL_PREFIX=${PROJECT_BINARY_DIR}/ckvision-install-check"
      -P "${PROJECT_SOURCE_DIR}/cmake/VerifyCkVisionInstall.cmake"
    USES_TERMINAL
    COMMENT "Install and smoke-test every native ckVision executable")
endfunction()

function(cktools_add_ckvision_cutover_verification)
  if(NOT CKTOOLS_VERIFY_CKVISION_INSTALL)
    return()
  endif()

  add_custom_target(verify_ckvision_cutover
    COMMAND "${CMAKE_COMMAND}"
      "-DCKTOOLS_INSTALL_PREFIX=${PROJECT_BINARY_DIR}/ckvision-install-check"
      -P "${PROJECT_SOURCE_DIR}/cmake/VerifyCkVisionCutover.cmake"
    DEPENDS verify_ckvision_install
    USES_TERMINAL
    COMMENT "Verify that the staged cutover product has no legacy UI artifacts")
endfunction()

function(cktools_add_ckvision_archive_verification)
  if(NOT CKTOOLS_VERIFY_CKVISION_ARCHIVE)
    return()
  endif()

  if(NOT DEFINED CKTOOLS_CPACK_BINARY_PACKAGE_FILE_NAME OR
     "${CKTOOLS_CPACK_BINARY_PACKAGE_FILE_NAME}" STREQUAL "")
    message(FATAL_ERROR
      "CKTOOLS_VERIFY_CKVISION_ARCHIVE requires CPack package metadata before "
      "the verification target is created.")
  endif()

  add_custom_target(verify_ckvision_archive
    COMMAND "${CMAKE_CPACK_COMMAND}" --config "${PROJECT_BINARY_DIR}/CPackConfig.cmake"
      -D CPACK_THREADS=1 -D CPACK_ARCHIVE_THREADS=1
    COMMAND "${CMAKE_COMMAND}"
      "-DCKTOOLS_PACKAGE_ARCHIVE=${PROJECT_BINARY_DIR}/${CKTOOLS_CPACK_BINARY_PACKAGE_FILE_NAME}.tar.gz"
      "-DCKTOOLS_PACKAGE_EXTRACT_DIR=${PROJECT_BINARY_DIR}/ckvision-package-check"
      -P "${PROJECT_SOURCE_DIR}/cmake/VerifyCkVisionArchive.cmake"
    USES_TERMINAL
    COMMENT "Extract and smoke-test the packaged ckVision cutover archive")
  if(UNIX AND NOT APPLE)
    # The same cpack run produced the DEB and RPM: check their install layout.
    add_custom_command(TARGET verify_ckvision_archive POST_BUILD
      COMMAND "${CMAKE_COMMAND}" "-DCKTOOLS_PACKAGE_DIR=${PROJECT_BINARY_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/VerifyLinuxPackages.cmake"
      VERBATIM)
  endif()
endfunction()

function(cktools_add_ckvision_terminal_verification)
  if(NOT CKTOOLS_VERIFY_CKVISION_INSTALL OR
     NOT CKTOOLS_VERIFY_CKVISION_TERMINAL OR
     NOT UNIX)
    return()
  endif()

  if(NOT TARGET ck_vision_terminal_smoke)
    message(FATAL_ERROR
      "CKTOOLS_VERIFY_CKVISION_TERMINAL requires the POSIX terminal-smoke helper.")
  endif()

  add_custom_target(verify_ckvision_terminal
    COMMAND "${CMAKE_COMMAND}"
      "-DCKTOOLS_INSTALL_PREFIX=${PROJECT_BINARY_DIR}/ckvision-install-check"
      "-DCKTOOLS_TERMINAL_CONFIG_ROOT=${PROJECT_BINARY_DIR}/ckvision-terminal-config"
      "-DCKTOOLS_TERMINAL_SMOKE_EXECUTABLE=$<TARGET_FILE:ck_vision_terminal_smoke>"
      -P "${PROJECT_SOURCE_DIR}/cmake/VerifyCkVisionTerminal.cmake"
    DEPENDS verify_ckvision_install ck_vision_terminal_smoke
    USES_TERMINAL
    COMMENT "Smoke-test staged ckVision executables in real terminal profiles")
endfunction()

# The rehearsal tests the paired Release tree and stages only this production tree.
function(cktools_add_ckvision_rehearsal_verification)
  if(BUILD_TESTING OR NOT CKTOOLS_VERIFY_CKVISION_PACKAGE OR
     NOT CKTOOLS_VERIFY_CKVISION_INSTALL OR
     NOT CKTOOLS_VERIFY_CKVISION_ARCHIVE OR
     NOT CKTOOLS_REHEARSAL_TEST_BINARY_DIR)
    return()
  endif()

  if(NOT IS_ABSOLUTE "${CKTOOLS_REHEARSAL_TEST_BINARY_DIR}" OR
     NOT EXISTS "${CKTOOLS_REHEARSAL_TEST_BINARY_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "Rehearsal requires a configured absolute Release test tree")
  endif()
  load_cache("${CKTOOLS_REHEARSAL_TEST_BINARY_DIR}" READ_WITH_PREFIX _cktools_rehearsal_
    BUILD_TESTING CMAKE_BUILD_TYPE CMAKE_HOME_DIRECTORY CK_CORE_FS_LIB_SOURCE_DIR CKTOOLS_CKVISION_PREFIX)
  if(NOT "${_cktools_rehearsal_BUILD_TESTING}" STREQUAL "ON" OR
     NOT "${_cktools_rehearsal_CMAKE_BUILD_TYPE}" STREQUAL "Release" OR
     NOT "${_cktools_rehearsal_CMAKE_HOME_DIRECTORY}" STREQUAL "${PROJECT_SOURCE_DIR}" OR
     NOT "${_cktools_rehearsal_CK_CORE_FS_LIB_SOURCE_DIR}" STREQUAL "${CK_CORE_FS_LIB_SOURCE_DIR}" OR
     NOT "${_cktools_rehearsal_CKTOOLS_CKVISION_PREFIX}" STREQUAL "${CKTOOLS_CKVISION_PREFIX}")
    message(FATAL_ERROR "Rehearsal requires the same frontend and dependency inputs in its Release test tree")
  endif()

  add_custom_target(verify_ckvision_rehearsal
    COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target verify_ckvision_package --parallel 1
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CKTOOLS_REHEARSAL_TEST_BINARY_DIR}" --output-on-failure --parallel 1 --no-tests=error
    COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target verify_ckvision_cutover --parallel 1
    COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target verify_ckvision_archive --parallel 1
    USES_TERMINAL
    VERBATIM
    COMMENT "Run the complete local ckVision cutover rehearsal")
endfunction()
