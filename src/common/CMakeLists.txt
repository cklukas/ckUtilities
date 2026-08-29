add_subdirectory(app-info)
add_subdirectory(options)

if(NOT CKTOOLS_CKVISION_CUTOVER)
  add_subdirectory(layout/grid)
  add_subdirectory(hotkeys)
  add_subdirectory(ui)
endif()
