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

CK_TEST(files_settings_store_restores_and_rejects_stale_atomic_writes) {
    ckv::MemoryFileSystem files;
    FilesSettingsStore first(files, "/user/ck-files.settings");
    CK_CHECK(first.load().diagnostic.empty());
    FilesPersistentSettings value;
    value.views[1].show_hidden = true;
    value.bookmarks[1].push_back(route("/root", "child"));
    CK_CHECK(first.save(value).empty());

    FilesSettingsStore relaunched(files, "/user/ck-files.settings");
    const auto restored = relaunched.load();
    CK_CHECK(restored.diagnostic.empty() && restored.value.views[1].show_hidden &&
             restored.value.bookmarks[1].size() == 1 && restored.value.bookmarks[1][0] == value.bookmarks[1][0]);

    FilesSettingsStore stale(files, "/user/ck-files.settings");
    CK_CHECK(stale.load().diagnostic.empty());
    value.views[1].show_hidden = false;
    CK_CHECK(relaunched.save(value).empty());
    CK_CHECK(!stale.save({}).empty());
    CK_CHECK(parse_files_settings(files.read_file("/user/ck-files.settings")->contents)->views[1].show_hidden == false);
}

CK_TEST(files_settings_direct_store_accepts_exact_wire_cap_and_preserves_file_after_one_byte_overflow) {
    constexpr std::size_t cap = 1024 * 1024;
    const auto root = [](std::string text) {
        return FilesPaneLocation(take(NativeAbsolutePath::from_posix(text)));
    };
    FilesPersistentSettings value;
    for (unsigned i = 0; i < 15; ++i) value.bookmarks[0].push_back(root("/" + std::string(32767, 'x')));
    value.bookmarks[0].push_back(root("/"));
    value.bookmarks[0][0] = take(value.bookmarks[0][0].descend(*NativeName::from_posix("x"), {},
                                                            FilesPaneLocation::hard_components));
    const auto extra = (cap - files_settings_text(value).size()) / 2;
    value.bookmarks[0].back() = root("/" + std::string(extra, 'y'));
    const auto exact = files_settings_text(value);
    CK_CHECK(exact.size() == cap && parse_files_settings(exact).has_value());
    ckv::MemoryFileSystem files;
    FilesSettingsStore store(files, "/settings/value");
    CK_CHECK(store.load().diagnostic.empty() && store.save(value).empty());
    const auto before = files.read_file("/settings/value");
    CK_CHECK(before && before->contents == exact);
    value.bookmarks[0].back() = take(root("/" + std::string(extra - 1, 'y')).descend(
        *NativeName::from_posix("z"), {}, FilesPaneLocation::hard_components));
    CK_CHECK(files_settings_text(value).size() == cap + 1 && !store.save(value).empty());
    const auto after = files.read_file("/settings/value");
    CK_CHECK(before && after && after->contents == before->contents && after->fingerprint == before->fingerprint);
    CK_CHECK(store.save({}).empty());
}

CK_TEST(files_settings_direct_store_rejects_invalid_filter_order_and_bookmark_count_without_poisoning_revision) {
    ckv::MemoryFileSystem files;
    FilesSettingsStore store(files, "/settings/value");
    CK_CHECK(store.load().diagnostic.empty() && store.save({}).empty());
    const auto before = files.read_file("/settings/value");
    FilesPersistentSettings invalid;
    invalid.views[0].display_substring.assign(4097, 'x');
    CK_CHECK(!store.save(invalid).empty());
    invalid = {};
    invalid.views[1].sort = static_cast<PaneSortKey>(255);
    CK_CHECK(!store.save(invalid).empty());
    invalid = {};
    for (unsigned i = 0; i < 257; ++i) invalid.bookmarks[0].push_back(route("/root", "child"));
    CK_CHECK(!store.save(invalid).empty());
    const auto after = files.read_file("/settings/value");
    CK_CHECK(before && after && after->contents == before->contents && after->fingerprint == before->fingerprint);
    CK_CHECK(store.save({}).empty());
}
