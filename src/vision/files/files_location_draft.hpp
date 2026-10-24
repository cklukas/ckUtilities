// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "files_location_text.hpp"
namespace ck::vision {
// A text projection is not the underlying route. Accepting an unchanged seed,
// including after a format switch, preserves its literal component boundaries.
// Editing Path text deliberately requests a new absolute address. No I/O here.
class FilesLocationDraft final {
  public:
    static ck::files::Observation<FilesLocationDraft> start(FilesPaneLocation location,
                                                            FilesLocationTextLimits limits = {});
    // Empty input is deliberately invalid until the reader supplies an
    // absolute address; no root/cwd/home is guessed during recovery.
    static ck::files::Observation<FilesLocationDraft> new_location(ck::files::NameEncoding encoding,
                                                                   FilesLocationTextLimits limits = {});
    const std::string &text() const noexcept { return text_; }
    FilesLocationTextSyntax syntax() const noexcept { return syntax_; }
    ck::files::Observation<FilesPaneLocation> resolve(std::string_view text) const;
    ck::files::Observation<FilesLocationDraft> reformat(std::string_view text, FilesLocationTextSyntax syntax) const;

  private:
    FilesLocationDraft(std::optional<FilesPaneLocation> location, ck::files::NameEncoding encoding, std::string text,
                       FilesLocationTextSyntax syntax, FilesLocationTextLimits limits);
    static ck::files::Observation<FilesLocationDraft> make(FilesPaneLocation location, FilesLocationTextSyntax syntax,
                                                           FilesLocationTextLimits limits);
    std::optional<FilesPaneLocation> original_;
    ck::files::NameEncoding encoding_;
    std::string text_;
    FilesLocationTextSyntax syntax_;
    FilesLocationTextLimits limits_;
};
} // namespace ck::vision
