# Framework-neutral application cores used by the ckVision-only cutover build.
# Their source locations remain stable during the migration; this composition
# deliberately excludes the legacy presentation targets and their dependencies.

include(${PROJECT_SOURCE_DIR}/cmake/FetchNlohmannJson.cmake)

add_library(ck_json_view_core STATIC
  ${PROJECT_SOURCE_DIR}/src/tools/json-view/src/json_view_core.cpp
)
target_include_directories(ck_json_view_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/tools/json-view/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(ck_json_view_core PUBLIC cxx_std_20)

add_library(ck_du_core STATIC
  ${PROJECT_SOURCE_DIR}/src/tools/ck-du/src/disk_usage_core.cpp
  ${PROJECT_SOURCE_DIR}/src/tools/ck-du/src/disk_usage_options.cpp
)
target_include_directories(ck_du_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/tools/ck-du/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(ck_du_core PUBLIC cxx_std_20)
if(APPLE)
  target_link_libraries(ck_du_core PUBLIC "-framework CoreFoundation" "-framework CoreServices")
endif()

add_library(ck_find_core STATIC
  ${PROJECT_SOURCE_DIR}/src/tools/ck-find/src/search_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/tools/ck-find/src/search_model.cpp
  ${PROJECT_SOURCE_DIR}/src/tools/ck-find/src/guided_search.cpp
)
target_include_directories(ck_find_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/tools/ck-find/include>
)
target_compile_features(ck_find_core PUBLIC cxx_std_20)
target_link_libraries(ck_find_core PUBLIC ck_options nlohmann_json::nlohmann_json)

add_library(ck_markdown_core STATIC
  ${PROJECT_SOURCE_DIR}/src/tools/ck-edit/src/markdown_parser.cpp
  ${PROJECT_SOURCE_DIR}/src/tools/ck-edit/src/markdown_transformations.cpp
)
target_include_directories(ck_markdown_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/tools/ck-edit/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(ck_markdown_core PUBLIC cxx_std_20)

install(TARGETS ck_json_view_core ck_du_core ck_find_core ck_markdown_core ARCHIVE DESTINATION lib)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/tools/json-view/include/ DESTINATION include
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/tools/ck-du/include/ DESTINATION include
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
install(FILES
  ${PROJECT_SOURCE_DIR}/src/tools/ck-edit/include/ck/edit/markdown_parser.hpp
  ${PROJECT_SOURCE_DIR}/src/tools/ck-edit/include/ck/edit/markdown_transformations.hpp
  DESTINATION include/ck/edit)
