add_ck_tool(ck_win_installer
  OUTPUT_NAME "cku-win-installer"
  SOURCES
    src/main.cpp
  DEFINITIONS
    CK_WIN_INSTALLER_VERSION="${PROJECT_VERSION}"
  NO_INSTALL
)
