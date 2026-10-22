# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Embeds cmake/windows/utf8.manifest in every executable this project defines
# (not in fetched dependencies), so each runs with UTF-8 as its code page.
include_guard(GLOBAL)

function(_cktools_collect_executables directory out)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  set(found)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
      list(APPEND found ${target})
    endif()
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    if(NOT child MATCHES "/_deps/")
      _cktools_collect_executables("${child}" child_found)
      list(APPEND found ${child_found})
    endif()
  endforeach()
  set(${out} ${found} PARENT_SCOPE)
endfunction()

function(cktools_embed_windows_manifests)
  if(NOT WIN32)
    return()
  endif()
  _cktools_collect_executables("${PROJECT_SOURCE_DIR}" executables)
  foreach(target IN LISTS executables)
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/cmake/windows/utf8.manifest")
  endforeach()
endfunction()
