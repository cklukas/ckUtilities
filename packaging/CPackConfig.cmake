set(CPACK_PACKAGE_NAME "ck-utilities")
set(CPACK_PACKAGE_VENDOR "ckUtilities Project")
set(CPACK_PACKAGE_CONTACT "maintainers@ckutilities.dev")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Turbo Vision utilities including ck-json-view, ck-du, ck-find, and ck-du")
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
else()
  set(CPACK_GENERATOR "TGZ;DEB;RPM")
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _cpack_system)
  set(CPACK_PACKAGE_FILE_NAME "ck-utilities-${CPACK_PACKAGE_VERSION}-${_cpack_system}")
endif()

set(CPACK_SOURCE_GENERATOR "TGZ")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ckUtilities Maintainers")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libncursesw6 | libncursesw5, libstdc++6")

set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Text")
set(CPACK_RPM_PACKAGE_REQUIRES "ncurses, libstdc++")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")

include(CPack)
