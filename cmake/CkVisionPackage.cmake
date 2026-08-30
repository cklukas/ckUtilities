include_guard(GLOBAL)

# This is the clean ckVision commit selected by WP-0. It is a compatibility
# record for development and CI, not a source-tree dependency: consumers must
# find an installed package through CMAKE_PREFIX_PATH or the normal CMake
# package search paths.
set(CKTOOLS_CKVISION_BASELINE_COMMIT
    "bf4c1c6404d58f33693d84e7654789cf60413839"
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
  find_package(ckvision CONFIG REQUIRED)

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
      "-DCKTOOLS_CKVISION_CUTOVER=${CKTOOLS_CKVISION_CUTOVER}"
      -P "${PROJECT_SOURCE_DIR}/cmake/VerifyCkVisionInstall.cmake"
    USES_TERMINAL
    COMMENT "Install and smoke-test every native ckVision executable")
endfunction()

function(cktools_add_ckvision_cutover_verification)
  if(NOT CKTOOLS_CKVISION_CUTOVER OR NOT CKTOOLS_VERIFY_CKVISION_INSTALL)
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
  if(NOT CKTOOLS_CKVISION_CUTOVER OR NOT CKTOOLS_VERIFY_CKVISION_ARCHIVE)
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
    COMMAND "${CMAKE_COMMAND}"
      "-DCKTOOLS_PACKAGE_ARCHIVE=${PROJECT_BINARY_DIR}/${CKTOOLS_CPACK_BINARY_PACKAGE_FILE_NAME}.tar.gz"
      "-DCKTOOLS_PACKAGE_EXTRACT_DIR=${PROJECT_BINARY_DIR}/ckvision-package-check"
      -DCKTOOLS_CKVISION_CUTOVER=ON
      -P "${PROJECT_SOURCE_DIR}/cmake/VerifyCkVisionArchive.cmake"
    USES_TERMINAL
    COMMENT "Extract and smoke-test the packaged ckVision cutover archive")
endfunction()

function(cktools_add_ckvision_terminal_verification)
  if(NOT CKTOOLS_CKVISION_CUTOVER OR
     NOT CKTOOLS_VERIFY_CKVISION_INSTALL OR
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

# Keep the complete local cutover rehearsal discoverable as one target without
# making any individual gate implicit for normal development builds. It is
# available only when every opt-in product, package-consumer, and archive gate
# was requested by the caller. Invoke the gates serially: the install and CPack
# paths both build from this tree, so parallel prerequisite scheduling would
# make the composed target needlessly contend with itself.
function(cktools_add_ckvision_rehearsal_verification)
  if(NOT CKTOOLS_CKVISION_CUTOVER OR
     NOT CKTOOLS_VERIFY_CKVISION_PACKAGE OR
     NOT CKTOOLS_VERIFY_CKVISION_INSTALL OR
     NOT CKTOOLS_VERIFY_CKVISION_ARCHIVE)
    return()
  endif()

  add_custom_target(verify_ckvision_rehearsal
    COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target verify_ckvision_package
    COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target verify_ckvision_cutover
    COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --target verify_ckvision_archive
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${PROJECT_BINARY_DIR}" --output-on-failure
    USES_TERMINAL
    COMMENT "Run the complete local ckVision cutover rehearsal")
endfunction()
