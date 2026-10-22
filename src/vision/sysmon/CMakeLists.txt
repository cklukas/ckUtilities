# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ck-sysmon: a desktop system monitor. sysmon_app is the portable model and
# interface; sysmon_native samples the local host (and SSH hosts) on macOS,
# Linux and Windows.
find_package(Threads REQUIRED)

add_library(ck_vision_sysmon STATIC
  monitor.cpp
  ollama_json.cpp
  monitor_service.cpp
  settings.cpp
  sysmon_app.cpp
)
target_include_directories(ck_vision_sysmon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ck_vision_sysmon PUBLIC ck_vision_host ckvision::cvision PRIVATE Threads::Threads)
target_compile_features(ck_vision_sysmon PUBLIC cxx_std_20)
cktools_strict(ck_vision_sysmon)

add_library(ck_vision_sysmon_native STATIC
  native_sample.cpp
  native_gpus.cpp
  linux_gpus.cpp
  ollama_native.cpp
  native_monitor.cpp
  ssh_config.cpp
)
target_include_directories(ck_vision_sysmon_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ck_vision_sysmon_native PUBLIC ckvision::cvision PRIVATE ${CMAKE_DL_LIBS})
target_compile_features(ck_vision_sysmon_native PUBLIC cxx_std_20)
cktools_strict(ck_vision_sysmon_native)
if(APPLE)
  target_sources(ck_vision_sysmon_native PRIVATE apple_gpus.mm)
  target_link_libraries(ck_vision_sysmon_native PRIVATE
    "-framework Metal" "-framework Foundation" "-framework IOKit" "-framework CoreFoundation")
  set_target_properties(ck_vision_sysmon_native PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
elseif(WIN32)
  target_link_libraries(ck_vision_sysmon_native PRIVATE dxgi pdh gdi32 cfgmgr32 winhttp)
endif()

add_executable(cksysmon_ckvision main.cpp)
target_link_libraries(cksysmon_ckvision PRIVATE ck_vision_sysmon_native ck_vision_sysmon ck_vision_host)
set_target_properties(cksysmon_ckvision PROPERTIES
  OUTPUT_NAME "ck-sysmon"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
)
cktools_strict(cksysmon_ckvision)
install(TARGETS cksysmon_ckvision RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  set(_ck_sysmon_suites test_sysmon.cpp test_sysmon_native.cpp)
  if(APPLE)
    list(APPEND _ck_sysmon_suites test_sysmon_apple.cpp)
  endif()
  list(TRANSFORM _ck_sysmon_suites PREPEND tests/ OUTPUT_VARIABLE _ck_sysmon_suite_sources)
  add_executable(ck_vision_sysmon_tests ${_ck_sysmon_suite_sources})
  target_include_directories(ck_vision_sysmon_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(ck_vision_sysmon_tests PRIVATE
    ck_vision_sysmon ck_vision_sysmon_native ck_vision_cktest_main)
  cktools_strict(ck_vision_sysmon_tests)
  foreach(suite IN LISTS _ck_sysmon_suites)
    get_filename_component(name "${suite}" NAME_WE)
    add_test(NAME ${name} COMMAND ck_vision_sysmon_tests --suite ${suite})
    set_tests_properties(${name} PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests
      LABELS "sysmon;unit")
  endforeach()

  add_test(NAME ck_sysmon_sample COMMAND cksysmon_ckvision --sample)
  set_tests_properties(ck_sysmon_sample PROPERTIES
    PASS_REGULAR_EXPRESSION "CKSYSMON 2" LABELS "sysmon;native" TIMEOUT 10)
  add_test(NAME ck_sysmon_version COMMAND cksysmon_ckvision --version)
  set_tests_properties(ck_sysmon_version PROPERTIES
    PASS_REGULAR_EXPRESSION "ck-sysmon ${PROJECT_VERSION}" LABELS "sysmon")
endif()
