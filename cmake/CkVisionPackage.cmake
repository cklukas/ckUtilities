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

option(CKTOOLS_VERIFY_CKVISION_INSTALL
       "Enable the installed native-executable verification target"
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
