add_library(ck_app_info STATIC
  tool_info.cpp
)

target_include_directories(ck_app_info
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(ck_app_info PUBLIC cxx_std_20)

install(TARGETS ck_app_info
  ARCHIVE DESTINATION lib
)

install(FILES ${PROJECT_SOURCE_DIR}/include/ck/app_info.hpp
  DESTINATION include/ck
)

