include(FetchContent)

function(cktools_ensure_tvision)
  if(TARGET tvision::tvision)
    return()
  endif()

  set(TV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(TV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(HAVE_NCURSESW_NCURSES_H OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    tvision
    GIT_REPOSITORY https://github.com/magiblot/tvision.git
    GIT_TAG 5bc08238a010344fe4dc02f2c9d05648be988f6d
    # GIT_TAG 7ecc590ac59b163a876da50867c69bba605cebfc
    CMAKE_ARGS -DCMAKE_DISABLE_FIND_PACKAGE_Python=ON
  )

  FetchContent_MakeAvailable(tvision)
endfunction()
