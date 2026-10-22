// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the AI core keeps per-user files. On macOS and Linux:
//   configuration  $XDG_CONFIG_HOME/cktools, else ~/.config/cktools
//   data           ~/.local/share/cktools   (models, prompts)
// On Windows:
//   configuration  %APPDATA%\cktools       (roams with the profile)
//   data           %LOCALAPPDATA%\cktools  (models are gigabytes: never roamed)
// Each falls back to the current directory when no root can be resolved.
#pragma once

#include <filesystem>

namespace ck::ai::detail
{

std::filesystem::path config_directory();
std::filesystem::path data_directory();

} // namespace ck::ai::detail
