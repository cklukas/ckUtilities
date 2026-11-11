// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "files_location.hpp"
#include "pane_model.hpp"
#include "cvision/core/filesystem.hpp"
#include <array>
#include <functional>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace ck::vision {

class FilesSettingsService;

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
    // Native/Memory providers use their bounded, same-version read entry point.
    // Other injected filesystems must supply an explicitly bounded reader; no
    // fallback invokes their potentially unbounded FileSystem::read_file.
    using BoundedRead = std::function<std::optional<ckv::FileReadResult>(std::string_view, std::size_t)>;
    FilesSettingsLoad load();
    // Reader must belong to files_, retain <= max_bytes, and return its opaque
    // fingerprint for exactly those bytes. Invoked once during startup only;
    // never retained by the Store and never called by the persistence worker.
    FilesSettingsLoad load(const BoundedRead &read);
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
    std::string diagnostic;
    // Null deliberately disables persistence for headless/injected controllers.
    // Production supplies a borrowed worker; owner joins outside dispatch.
    FilesSettingsService *worker = nullptr;
};

} // namespace ck::vision
