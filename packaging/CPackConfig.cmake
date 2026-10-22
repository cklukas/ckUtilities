set(CPACK_PACKAGE_NAME "ck-utilities")
set(CPACK_PACKAGE_VENDOR "ckUtilities Project")
set(CPACK_PACKAGE_CONTACT "maintainers@ckutilities.dev")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ckVision-native terminal utilities including ck-json-view, ck-du, ck-find, ck-chat, ck-todo, and ck-sysmon")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ck-utilities")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)

if(APPLE)
  set(CPACK_GENERATOR "TGZ")
  set(CPACK_PACKAGE_FILE_NAME "ck-utilities-${CPACK_PACKAGE_VERSION}-macos")
  # Use an empty packaging install prefix so the macOS archive contains the
  # installed tree directly under the top-level directory. Using "." here
  # causes CPack to generate an empty top-level directory (and therefore a
  # near-empty archive) because the staged install ends up under a sibling
  # directory named with a trailing dot.
  set(CPACK_PACKAGING_INSTALL_PREFIX "")
elseif(WIN32)
  # A portable archive: bin\ holds the nine self-contained executables (static
  # C runtime, static llama), so nothing else needs installing.
  set(CPACK_GENERATOR "ZIP")
  set(_cpack_windows_arch "x64")
  if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "[Aa][Rr][Mm]64")
    set(_cpack_windows_arch "arm64")
  endif()
  set(CPACK_PACKAGE_FILE_NAME "ck-utilities-${CPACK_PACKAGE_VERSION}-windows-${_cpack_windows_arch}")
  set(CPACK_PACKAGING_INSTALL_PREFIX "")
else()
  set(CPACK_GENERATOR "TGZ;DEB;RPM")
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _cpack_system)
  set(CPACK_PACKAGE_FILE_NAME "ck-utilities-${CPACK_PACKAGE_VERSION}-${_cpack_system}")
endif()

set(CPACK_SOURCE_GENERATOR "TGZ")

# Packages hold the product only: the executables, documentation, license and
# sample configuration (component Runtime). Domain-library headers and
# archives (Development) and fetched dependencies' install rules (llama.cpp,
# ggml) are never packaged.
set(CPACK_COMPONENTS_ALL Runtime)
# Component selection applies only when generators install by component;
# grouping keeps it one package per format, named as before.
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
# Archives get a top-level folder, DEB and RPM do not: decided per generator.
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/CPackGeneratorOptions.cmake")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ckUtilities Maintainers")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libstdc++6")

set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Text")
set(CPACK_RPM_PACKAGE_REQUIRES "libstdc++")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")

# CPack rewrites CPACK_PACKAGE_FILE_NAME while configuring its source-package
# metadata. Keep the binary archive identity available to the cutover archive
# verifier that is added after this file is included.
set(CKTOOLS_CPACK_BINARY_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")

include(CPack)
