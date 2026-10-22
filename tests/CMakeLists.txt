include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Test frameworks are build-only dependencies.  Installing them would add
# development libraries and headers to every product archive even though no
# shipped executable links them.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_subdirectory(unit)

add_subdirectory(integration)

# Windows: every tool starts in a real ConPTY pseudo-console, draws, and quits
# through Alt+X - the counterpart of the POSIX terminal-profile verification.
if(WIN32)
  add_executable(ck_windows_tools_contract windows/windows_tools_contract.cpp)
  target_link_libraries(ck_windows_tools_contract PRIVATE ck_vision_cktest_main ckvision::cvision)
  target_compile_definitions(ck_windows_tools_contract PRIVATE
    CKTOOLS_TOOL_BINARY_DIR="${PROJECT_BINARY_DIR}/bin")
  cktools_strict(ck_windows_tools_contract)
  add_dependencies(ck_windows_tools_contract
    ckutilities_ckvision ckjsonview_ckvision ckfind_ckvision ckdu_ckvision ckedit_ckvision
    ckchat_ckvision cktodo_ckvision cksysmon_ckvision ckconfig_ckvision)
  add_test(NAME ck_windows_tools_contract COMMAND ck_windows_tools_contract)
  set_tests_properties(ck_windows_tools_contract PROPERTIES
    LABELS "platform;windows;native" TIMEOUT 300)
endif()
