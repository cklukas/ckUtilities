// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "files_location.hpp"
#include <string_view>
namespace ck::vision {
enum class FilesLocationTextSyntax { Path, ExactNames };
struct FilesLocationTextLimits {
    ck::files::NativePathLimits path{64 * 1024};
    std::size_t components = 1024;
    std::size_t text_bytes = 1024 * 1024;
    static constexpr std::size_t hard_text_bytes = 16 * 1024 * 1024;
};
// Value-only editor codec. Never parse display_text()/display_name() here.
// Path means one new absolute native address encoded as safe, strict UTF-8;
// no escaping, shell expansion, cwd lookup or Unicode normalization. Ordinary
// Windows components that Win32 may alter/interpret refuse; use ExactNames.
// ExactNames is a bracketed sequence of quoted native strings: the absolute
// anchor, then zero or more separate native child names. Examples:
// ["/home/user", "work", "raw\xFF"]
// ["V:\\Users", "literal. ", "\u{D800}"]
// Escapes: \\, \", \/, \n, \r, \t, \b, \f and \u{1..6 hex digits};
// POSIX additionally accepts \xHH.
// No raw controls/bidi format controls, implicit conversions or partial result.
// Encoding is supplied by the pane, never inferred. Exact text alone is not a
// self-identifying cross-platform transport or an operation-authorization token.
ck::files::Observation<FilesPaneLocation> parse_files_location_text(std::string_view text,
                                                                    FilesLocationTextSyntax syntax,
                                                                    ck::files::NameEncoding encoding,
                                                                    FilesLocationTextLimits limits = {});
// Canonical ASCII-only ExactNames serialization, not a flattened pathname.
// Round-trips exact native units, anchor spelling and component boundaries.
ck::files::Observation<std::string> format_files_exact_location(const FilesPaneLocation &location,
                                                                FilesLocationTextLimits limits = {});
struct FilesLocationProposal {
    std::string text;
    FilesPaneLocation location;
};
// Value-only completion query. Prefixes may be empty or partial navigation
// names (".", ".."); only complete NativeNames can become proposals. Matching
// is exact native-unit prefix matching, with no case folding or normalization.
// A caller must discover/classify directories on a worker before proposing
// them. Neither a query nor a proposal grants object identity or authority.
class FilesLocationCompletionQuery final {
  public:
    const FilesPaneLocation &parent() const noexcept { return parent_; }
    const std::string *posix_prefix() const noexcept { return std::get_if<std::string>(&prefix_); }
    const std::u16string *windows_prefix() const noexcept { return std::get_if<std::u16string>(&prefix_); }
    bool matches(const ck::files::NativeName &name) const noexcept;
    // Produces complete, validated editor text. Path proposals end in a
    // separator for the next segment. ExactNames proposals close the quoted
    // component/list; a comma starts the next literal component. Typed escape
    // spelling is retained, including prefixes ending inside a Unicode scalar
    // in native units. A lossy/unrepresentable Path proposal refuses explicitly.
    ck::files::Observation<FilesLocationProposal> propose_directory(const ck::files::NativeName &name) const;

  private:
    friend class FilesCompletionIndexJob;
    friend class FilesCompletionService;
    friend ck::files::Observation<FilesLocationCompletionQuery>
        parse_files_location_completion(std::string_view, FilesLocationTextSyntax, ck::files::NameEncoding,
                                        FilesLocationTextLimits);
    FilesLocationCompletionQuery(FilesPaneLocation parent, std::variant<std::string, std::u16string> prefix,
                                 std::string leading, FilesLocationTextSyntax syntax, char separator,
                                 FilesLocationTextLimits limits)
        : parent_(std::move(parent)), prefix_(std::move(prefix)), leading_(std::move(leading)), syntax_(syntax),
          separator_(separator), limits_(limits) {}
    FilesPaneLocation parent_;
    std::variant<std::string, std::u16string> prefix_;
    std::string leading_;
    FilesLocationTextSyntax syntax_;
    char separator_;
    FilesLocationTextLimits limits_;
};
// Path requires an absolute parent through the last native separator.
// ExactNames requires a complete absolute anchor; its last quoted component
// may be open, or a trailing comma may start an empty prefix. Incomplete
// escapes/UTF-8, separators/NUL in a literal prefix, foreign encoding and all
// budgets refuse before discovery. A missing anchor never guesses cwd/home.
ck::files::Observation<FilesLocationCompletionQuery>
parse_files_location_completion(std::string_view text, FilesLocationTextSyntax syntax, ck::files::NameEncoding encoding,
                                FilesLocationTextLimits limits = {});
} // namespace ck::vision
