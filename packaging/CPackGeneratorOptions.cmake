# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Read by CPack once per generator (CPACK_PROJECT_CONFIG_FILE). Archives
# unpack into one named folder; DEB and RPM install at their package paths
# (/usr/bin, /usr/share), never under such a folder.
if(CPACK_GENERATOR MATCHES "^(TGZ|ZIP)$")
  set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)
else()
  set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)
endif()
