include(FindCursesWithFallback)

include(FetchTurboVision)
cktools_ensure_tvision()

if(TARGET tvision)
  target_include_directories(tvision PRIVATE ${CURSES_INCLUDE_DIRS})
endif()

add_library(ck_du_core STATIC
  src/disk_usage_core.cpp
  src/disk_usage_options.cpp
)

target_include_directories(ck_du_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(ck_du_core PUBLIC cxx_std_20)

if(APPLE)
  target_link_libraries(ck_du_core
    PUBLIC
      "-framework CoreFoundation"
      "-framework CoreServices"
  )
endif()

install(TARGETS ck_du_core
  ARCHIVE DESTINATION lib
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

add_ck_tool(ckdu
  OUTPUT_NAME "ck-du"
  SOURCES
    src/disk-usage-app.cpp
  LIBRARIES
    ck_app_info
    ck_hotkeys
    ck_options
    ck_du_core
    tvision::tvision
    ${CURSES_LIBRARIES}
  INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include
    ${CURSES_INCLUDE_DIRS}
  DEFINITIONS
    CK_DU_VERSION="${PROJECT_VERSION}"
)

if(APPLE)
  target_sources(ckdu
    PRIVATE
      src/mac_cloud_operations.mm
  )
  target_link_libraries(ckdu
    PRIVATE
      "-framework Foundation"
      "-framework AppKit"
  )
endif()
