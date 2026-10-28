// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "files_location.hpp"
#include "pane_model.hpp"
#include "cvision/core/filesystem.hpp"
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace ck::vision {

struct FilesPersistentSettings {
    std::array<ck::files::PaneView, 2> views{};
    std::array<std::vector<FilesPaneLocation>, 2> bookmarks;
};

std::string files_settings_text(const FilesPersistentSettings &settings);
std::optional<FilesPersistentSettings> parse_files_settings(std::string_view text);

struct FilesSettingsLoad {
    FilesPersistentSettings value;
    std::string diagnostic;
};

class FilesSettingsStore final {
  public:
    FilesSettingsStore(ckv::FileSystem &files, std::string path) : files_(files), path_(std::move(path)) {}
    FilesSettingsLoad load();
    // Empty means success. Failed writes preserve the previous file.
    std::string save(const FilesPersistentSettings &settings);

  private:
    ckv::FileSystem &files_;
    std::string path_;
    std::optional<ckv::FileFingerprint> revision_;
    bool writable_ = false;
};

struct FilesSettingsBinding {
    FilesPersistentSettings value;
    std::function<std::string(const FilesPersistentSettings &)> save;
    std::string diagnostic;
};

} // namespace ck::vision
