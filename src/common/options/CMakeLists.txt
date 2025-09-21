add_library(ck_options STATIC
  options.cpp
)

target_include_directories(ck_options
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/json-view/include
)

target_compile_features(ck_options PUBLIC cxx_std_20)

install(TARGETS ck_options
  ARCHIVE DESTINATION lib
)

install(FILES ${PROJECT_SOURCE_DIR}/include/ck/options.hpp
  DESTINATION include/ck
)
