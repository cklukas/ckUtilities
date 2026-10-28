// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_settings.hpp"
#include <cvision/testing/cktest.hpp>
#include <stdexcept>

namespace {
using namespace ck::files;
using namespace ck::vision;

template <class T> T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("Files settings fixture unavailable");
    return std::move(value).take_value();
}

FilesPaneLocation route(std::string_view root, std::string_view child) {
    auto base = take(NativeAbsolutePath::from_posix(root));
    return take(FilesPaneLocation(std::move(base)).descend(*NativeName::from_posix(std::string(child)), {}, 8));
}
} // namespace

CK_TEST(files_settings_round_trip_preserves_view_and_exact_posix_route) {
    FilesPersistentSettings settings;
    settings.views[0].sort = PaneSortKey::Modified;
    settings.views[0].descending = true;
    settings.views[0].folders_first = false;
    settings.views[0].show_hidden = true;
    settings.views[0].ascii_case_sensitive = true;
    settings.views[0].display_substring = "literal\\xff";
    settings.bookmarks[0].push_back(route("/root", "literal\\name\xff"));

    const auto text = files_settings_text(settings);
    const auto parsed = parse_files_settings(text);
    CK_CHECK(parsed.has_value());
    CK_CHECK(parsed->views[0].sort == settings.views[0].sort &&
             parsed->views[0].descending == settings.views[0].descending &&
             parsed->views[0].folders_first == settings.views[0].folders_first &&
             parsed->views[0].show_hidden == settings.views[0].show_hidden &&
             parsed->views[0].ascii_case_sensitive == settings.views[0].ascii_case_sensitive &&
             parsed->views[0].display_substring == settings.views[0].display_substring &&
             parsed->bookmarks[0].size() == 1 && parsed->bookmarks[0][0] == settings.bookmarks[0][0]);
}

CK_TEST(files_settings_round_trip_preserves_windows_units_and_rejects_corruption) {
    FilesPersistentSettings settings;
    settings.views[1].sort = PaneSortKey::Kind;
    const auto root = take(NativeAbsolutePath::from_windows(u"\\\\host\\share"));
    const auto child = take(FilesPaneLocation(std::move(root)).descend(*NativeName::from_windows(u"literal. "), {}, 8));
    settings.bookmarks[1].push_back(child);

    const auto text = files_settings_text(settings);
    CK_CHECK(parse_files_settings(text).has_value());
    CK_CHECK(!parse_files_settings("CKFILES_SETTINGS 1\nVIEW 0 0 0 1 0 0 00\nEND\n").has_value());
    CK_CHECK(!parse_files_settings(text + "garbage\n").has_value());
}

CK_TEST(files_settings_parser_bounds_bookmarks_and_filters) {
    FilesPersistentSettings settings;
    settings.views[0].display_substring.assign(4096, 'x');
    settings.bookmarks[0].push_back(route("/root", "child"));
    CK_CHECK(parse_files_settings(files_settings_text(settings)).has_value());

    settings.views[0].display_substring.push_back('x');
    CK_CHECK(!parse_files_settings(files_settings_text(settings)).has_value());
}
