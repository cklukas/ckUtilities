// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_location_draft.hpp"
#include <cvision/testing/cktest.hpp>
#include <stdexcept>
namespace {
using namespace ck::files;
using namespace ck::vision;
template <class T> T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("draft fixture: " + value.unavailable_detail()->error.operation);
    return std::move(value).take_value();
}
FilesPaneLocation root(bool windows = false) {
    return FilesPaneLocation(windows ? take(NativeAbsolutePath::from_windows(u"V:\\base"))
                                     : take(NativeAbsolutePath::from_posix("/base")));
}
FilesPaneLocation child(bool windows = false) {
    return take(
        root(windows).descend(windows ? *NativeName::from_windows(u"child") : *NativeName::from_posix("child"), {}, 8));
}
template <class T> void limited(const Observation<T> &result) {
    CK_CHECK(!result.value() &&
             result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
}
} // namespace
CK_TEST(files_location_draft_keeps_unchanged_structured_routes_through_both_formats) {
    for (bool windows : {false, true}) {
        const auto location = child(windows);
        const auto draft = take(FilesLocationDraft::start(location));
        CK_CHECK(draft.syntax() == FilesLocationTextSyntax::Path);
        CK_CHECK(draft.text() == (windows ? "V:\\base\\child" : "/base/child"));
        CK_CHECK(take(draft.resolve(draft.text())) == location);
        const auto exact = take(draft.reformat(draft.text(), FilesLocationTextSyntax::ExactNames));
        CK_CHECK(exact.text() == take(format_files_exact_location(location)) &&
                 take(exact.resolve(exact.text())) == location);
        const auto plain = take(exact.reformat(exact.text(), FilesLocationTextSyntax::Path));
        CK_CHECK(plain.text() == draft.text() && take(plain.resolve(plain.text())) == location);
    }
}
CK_TEST(files_location_draft_uses_exact_names_for_raw_units_and_refuses_lossy_format_changes) {
    const auto posix = take(root().descend(*NativeName::from_posix("raw\xff\\name\n"), {}, 8));
    const auto win =
        take(root(true).descend(*NativeName::from_windows(std::u16string(u"literal. ") + char16_t{0xD800}), {}, 8));
    for (const auto &location : {posix, win}) {
        const auto draft = take(FilesLocationDraft::start(location));
        CK_CHECK(draft.syntax() == FilesLocationTextSyntax::ExactNames &&
                 take(draft.resolve(draft.text())) == location);
        CK_CHECK(!draft.reformat(draft.text(), FilesLocationTextSyntax::Path).value());
        CK_CHECK(take(draft.resolve(draft.text())) == location);
    }
    const auto trailing = take(root(true).descend(*NativeName::from_windows(u"literal. "), {}, 8));
    CK_CHECK(take(FilesLocationDraft::start(trailing)).syntax() == FilesLocationTextSyntax::ExactNames);
}
CK_TEST(files_location_draft_edited_paths_are_new_addresses_and_exact_edits_keep_component_boundaries) {
    const auto draft = take(FilesLocationDraft::start(child()));
    const auto changed = take(draft.resolve("/new/target"));
    CK_CHECK(*changed.root_address().posix_bytes() == "/new/target" && changed.components().empty());
    const auto exact = take(draft.reformat("/new/target", FilesLocationTextSyntax::ExactNames));
    CK_CHECK(take(exact.resolve(exact.text())) == changed);
    const std::string edited = R"(["/new/target", "literal\\child"])";
    const auto child_route = take(exact.resolve(edited));
    CK_CHECK(child_route.components().size() == 1 && *child_route.components()[0].posix_bytes() == "literal\\child");
    CK_CHECK(take(draft.resolve(draft.text())) == child());
    CK_CHECK(!exact.reformat("invalid", FilesLocationTextSyntax::Path).value());
}
CK_TEST(files_location_draft_limits_apply_to_projection_conversion_and_new_addresses_without_truncation) {
    FilesLocationTextLimits limits;
    limits.text_bytes = 4;
    const auto location = FilesPaneLocation(take(NativeAbsolutePath::from_posix("/abc")));
    const auto draft = take(FilesLocationDraft::start(location, limits));
    CK_CHECK(draft.text() == "/abc");
    limited(draft.reformat(draft.text(), FilesLocationTextSyntax::ExactNames));
    limited(draft.resolve("/abcd"));
    limits.text_bytes = 5;
    limited(
        FilesLocationDraft::start(FilesPaneLocation(take(NativeAbsolutePath::from_windows(u"V:\\\u2603"))), limits));
    limits = {};
    limits.path.max_native_bytes = child().native_bytes() - 1;
    limited(FilesLocationDraft::start(child(), limits));
    limits = {};
    limits.components = 1;
    const auto deep = take(child().descend(*NativeName::from_posix("next"), {}, 8));
    limited(FilesLocationDraft::start(deep, limits));
    CK_CHECK(take(draft.resolve(draft.text())) == location);
}
CK_TEST(files_location_draft_invalid_configuration_moved_values_and_unknown_formats_refuse) {
    FilesLocationTextLimits limits;
    limits.text_bytes = 0;
    CK_CHECK(!FilesLocationDraft::start(root(), limits).value());
    CK_CHECK(!FilesLocationDraft::new_location(NameEncoding::PosixBytes, limits).value());
    auto draft = take(FilesLocationDraft::start(root()));
    const auto retained = std::move(draft);
    CK_CHECK(!draft.resolve(draft.text()).value());
    CK_CHECK(take(retained.resolve(retained.text())) == root());
    CK_CHECK(!retained.reformat(retained.text(), static_cast<FilesLocationTextSyntax>(255)).value());
}
CK_TEST(files_location_draft_empty_recovery_requires_an_explicit_absolute_address_and_never_guesses) {
    for (bool windows : {false, true}) {
        const auto fresh =
            take(FilesLocationDraft::new_location(windows ? NameEncoding::WindowsUtf16 : NameEncoding::PosixBytes));
        CK_CHECK(fresh.text().empty() && fresh.syntax() == FilesLocationTextSyntax::Path && !fresh.resolve("").value());
        CK_CHECK(!fresh.resolve("relative").value() && !fresh.resolve("~/home").value());
        const auto parsed = take(fresh.resolve(windows ? "V:\\chosen" : "/chosen"));
        CK_CHECK(parsed.root_address().is_valid() && parsed.components().empty());
    }
    CK_CHECK(!FilesLocationDraft::new_location(NameEncoding::WindowsEaBytes).value());
}
