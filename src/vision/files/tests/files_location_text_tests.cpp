// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_location_text.hpp"
#include <cvision/testing/cktest.hpp>
#include <stdexcept>
namespace {
using namespace ck::files;
using namespace ck::vision;
template <class T> T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("location text fixture: " + value.unavailable_detail()->error.operation);
    return std::move(value).take_value();
}
FilesPaneLocation posix(std::string_view text) { return FilesPaneLocation(take(NativeAbsolutePath::from_posix(text))); }
FilesPaneLocation windows(std::u16string_view text) {
    return FilesPaneLocation(take(NativeAbsolutePath::from_windows(text)));
}
FilesPaneLocation decode(std::string_view text, NameEncoding encoding = NameEncoding::PosixBytes,
                         FilesLocationTextLimits limits = {}) {
    return take(parse_files_location_text(text, FilesLocationTextSyntax::ExactNames, encoding, limits));
}
void roundtrip(const FilesPaneLocation &location) {
    const auto text = take(format_files_exact_location(location));
    for (auto c : text)
        CK_CHECK(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) <= 0x7E);
    const auto decoded = decode(text, location.root_address().encoding());
    CK_CHECK(decoded == location && decoded.native_bytes() == location.native_bytes());
    CK_CHECK(take(format_files_exact_location(decoded)) == text);
}
template <class T> void limited(const Observation<T> &result) {
    CK_CHECK(!result.value() && result.unavailable_detail()->error.domain == ErrorDomain::Adapter &&
             result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
}
} // namespace
CK_TEST(files_location_exact_codec_preserves_anchor_spelling_and_separate_literal_components) {
    const auto root = posix("//root/../base/");
    const auto route = take(root.descend(*NativeName::from_posix("back\\slash\n\xff\""), {}, 10));
    const auto encoded = take(format_files_exact_location(route));
    const std::string expected = R"(["//root/../base/", "back\\slash\u{000A}\xFF\""])";
    CK_CHECK(encoded == expected);
    roundtrip(route);
    const auto win = take(windows(u"V:\\base\\..").descend(*NativeName::from_windows(u"NUL. "), {}, 10));
    CK_CHECK(take(format_files_exact_location(win)) == R"(["V:\\base\\..", "NUL. "])");
    roundtrip(win);
    CK_CHECK(decode(R"(["/base/child"])") != decode(R"(["/base", "child"])"));
}
CK_TEST(files_location_exact_codec_roundtrips_all_posix_name_bytes_without_encoding_replacement) {
    for (unsigned value = 1; value <= 255; ++value) {
        if (value == '/')
            continue;
        const auto name = *NativeName::from_posix(std::string("name") + static_cast<char>(value));
        roundtrip(take(posix("/root").descend(name, {}, 10)));
    }
    for (const auto text : {"/e\xcc\x81", "/\xc3\xa9", "/\xf0\x9f\x98\x80", "/\xed\xa0\x80", "/\xc0\xaf"})
        roundtrip(posix(text));
}
CK_TEST(files_location_exact_codec_roundtrips_all_windows_name_units_including_unpaired_surrogates) {
    for (unsigned first = 1; first <= 65535; first += 128) {
        std::u16string units = u"name";
        for (unsigned value = first; value <= 65535 && value < first + 128; ++value)
            if (value != '/' && value != '\\')
                units.push_back(static_cast<char16_t>(value));
        roundtrip(take(windows(u"\\\\?\\V:\\root").descend(*NativeName::from_windows(units), {}, 10)));
    }
    for (const auto text : {u"v:/root/../", u"\\\\server\\share", u"\\\\?\\UNC\\server\\share\\",
                            u"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\root"})
        roundtrip(windows(text));
}
CK_TEST(files_location_exact_codec_accepts_explicit_escapes_but_never_parses_display_text_as_authority) {
    const auto route = decode(R"( [ "/base", "quote\"\\\n\r\t\b\f", "\u{1F600}" ] )");
    CK_CHECK(*route.components()[0].posix_bytes() == "quote\"\\\n\r\t\b\f");
    CK_CHECK(*route.components()[1].posix_bytes() == "\xf0\x9f\x98\x80");
    const auto raw = decode(R"(["/", "\xFF"])");
    const auto escaped = decode(R"(["/", "\\xFF"])");
    CK_CHECK(raw != escaped && raw.components()[0].display_name() != escaped.components()[0].display_name());
    CK_CHECK(
        !parse_files_location_text(raw.display_text(), FilesLocationTextSyntax::ExactNames, NameEncoding::PosixBytes)
             .value());
    const auto win = decode(R"(["V:\\", "\u{D800}\u{0061}\u{DC00}"])", NameEncoding::WindowsUtf16);
    CK_CHECK(*win.components()[0].windows_units() == std::u16string({char16_t{0xD800}, u'a', char16_t{0xDC00}}));
}
CK_TEST(files_location_exact_codec_refuses_malformed_incomplete_ambiguous_and_invalid_native_components) {
    for (const auto text :
         {"", "[]", "[\"/\",]", "[\"/\"]tail", "[\"/\" \"name\"]", "[\"/\", \"\"]", "[\"/\", \".\"]", "[\"/\", \"..\"]",
          "[\"/\", \"a/b\"]", "[\"/\", \"\\x00\"]", "[\"/\", \"\\q\"]", "[\"/\", \"\\xF\"]", "[\"/\", \"\\u{}\"]",
          "[\"/\", \"\\u{110000}\"]", "[\"/\", \"\\u{0000001}\"]", "[\"/\", \"\\u{D800}\"]", "[\"relative\"]",
          "[\"/\", \"a\n\"]"}) {
        CK_CHECK(
            !parse_files_location_text(text, FilesLocationTextSyntax::ExactNames, NameEncoding::PosixBytes).value());
    }
    for (const auto text : {R"(["V:\\", "a\\b"])", R"(["V:\\", "\x41"])", R"(["V:\\", "\u{0000}"])",
                            R"(["V:relative"])", R"(["\\.\PhysicalDrive0"])"})
        CK_CHECK(
            !parse_files_location_text(text, FilesLocationTextSyntax::ExactNames, NameEncoding::WindowsUtf16).value());
    for (const auto &bytes : {std::string("[\"/\", \"raw\xff\"]"), std::string("[\"/\", \"\xe2\x80\xae\"]")})
        CK_CHECK(
            !parse_files_location_text(bytes, FilesLocationTextSyntax::ExactNames, NameEncoding::PosixBytes).value());
}
CK_TEST(files_location_path_codec_treats_literal_text_as_a_new_absolute_address_without_unescaping) {
    const auto plain =
        take(parse_files_location_text("/raw\\xFF", FilesLocationTextSyntax::Path, NameEncoding::PosixBytes));
    CK_CHECK(*plain.root_address().posix_bytes() == "/raw\\xFF" && plain.components().empty());
    CK_CHECK(plain != decode(R"(["/raw\xFF"])"));
    const auto unicode =
        take(parse_files_location_text("/e\xcc\x81/\xc3\xa9", FilesLocationTextSyntax::Path, NameEncoding::PosixBytes));
    CK_CHECK(*unicode.root_address().posix_bytes() == "/e\xcc\x81/\xc3\xa9");
    const auto win = take(
        parse_files_location_text("v:/e\xcc\x81/\xc3\xa9", FilesLocationTextSyntax::Path, NameEncoding::WindowsUtf16));
    CK_CHECK(*win.root_address().windows_units() == u"v:/e\u0301/\u00e9");
    for (const auto text : {"relative", "~/folder", "/raw\n", "/raw\xff", "/\xe2\x80\xae", "/\xf4\x90\x80\x80"})
        CK_CHECK(!parse_files_location_text(text, FilesLocationTextSyntax::Path, NameEncoding::PosixBytes).value());
}
CK_TEST(files_location_path_codec_refuses_win32_interpretation_and_keeps_extended_addresses) {
    for (const auto text : {"V:\\parent.\\leaf", "V:\\parent \\leaf", "V:\\NUL", "V:\\file:stream",
                            "\\\\srv.\\share\\leaf", "\\\\server\\share. \\leaf", "\\/server/share.", "V:\\wild*name"})
        CK_CHECK(!parse_files_location_text(text, FilesLocationTextSyntax::Path, NameEncoding::WindowsUtf16).value());
    for (const auto text : {R"(V:\parent\..\leaf)", R"(\\server\share\leaf)", R"(\\?\V:\NUL. \leaf)",
                            R"(\\?\UNC\server\share\literal. )"}) {
        const auto value =
            take(parse_files_location_text(text, FilesLocationTextSyntax::Path, NameEncoding::WindowsUtf16));
        roundtrip(value);
    }
    const auto literal = decode(R"(["V:\\", "NUL. "])", NameEncoding::WindowsUtf16);
    CK_CHECK(*literal.components()[0].windows_units() == u"NUL. ");
}
CK_TEST(files_location_codec_resource_limits_bound_every_input_and_publication_without_partial_routes) {
    const auto route = decode(R"(["/", "first", "second"])");
    const auto text = take(format_files_exact_location(route));
    FilesLocationTextLimits limits;
    limits.path.max_native_bytes = route.native_bytes();
    limits.components = 2;
    limits.text_bytes = text.size();
    CK_CHECK(take(format_files_exact_location(route, limits)) == text &&
             decode(text, NameEncoding::PosixBytes, limits) == route);
    --limits.path.max_native_bytes;
    limited(format_files_exact_location(route, limits));
    limited(parse_files_location_text(text, FilesLocationTextSyntax::ExactNames, NameEncoding::PosixBytes, limits));
    ++limits.path.max_native_bytes;
    limits.components = 1;
    limited(format_files_exact_location(route, limits));
    limited(parse_files_location_text(text, FilesLocationTextSyntax::ExactNames, NameEncoding::PosixBytes, limits));
    limits.components = 2;
    --limits.text_bytes;
    limited(format_files_exact_location(route, limits));
    limited(parse_files_location_text(text, FilesLocationTextSyntax::ExactNames, NameEncoding::PosixBytes, limits));
    limits = {};
    limits.path.max_native_bytes = 3;
    limited(parse_files_location_text("/abc", FilesLocationTextSyntax::Path, NameEncoding::PosixBytes, limits));
}
CK_TEST(files_location_codec_rejects_invalid_limits_kinds_encodings_and_moved_routes) {
    const auto root = posix("/");
    for (int which = 0; which < 6; ++which) {
        FilesLocationTextLimits limits;
        if (which == 0)
            limits.path.max_native_bytes = 0;
        if (which == 1)
            limits.path.max_native_bytes = NativePathLimits::hard_native_bytes + 1;
        if (which == 2)
            limits.components = 0;
        if (which == 3)
            limits.components = FilesPaneLocation::hard_components + 1;
        if (which == 4)
            limits.text_bytes = 0;
        if (which == 5)
            limits.text_bytes = FilesLocationTextLimits::hard_text_bytes + 1;
        const auto encoded = format_files_exact_location(root, limits);
        CK_CHECK(!encoded.value() &&
                 encoded.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidLimit));
        CK_CHECK(!parse_files_location_text(R"(["/"])", FilesLocationTextSyntax::ExactNames, NameEncoding::PosixBytes,
                                            limits)
                      .value());
    }
    CK_CHECK(
        !parse_files_location_text("/", static_cast<FilesLocationTextSyntax>(255), NameEncoding::PosixBytes).value());
    for (auto encoding : {NameEncoding::WindowsEaBytes, static_cast<NameEncoding>(255)})
        CK_CHECK(!parse_files_location_text("/", FilesLocationTextSyntax::Path, encoding).value());
    auto moved = root;
    const auto retained = std::move(moved);
    CK_CHECK(!format_files_exact_location(moved).value() && format_files_exact_location(retained).value());
    auto address = take(NativeAbsolutePath::from_posix("/"));
    const auto valid = std::move(address);
    (void)valid;
    bool refused = false;
    try {
        const FilesPaneLocation invalid(std::move(address));
        (void)invalid;
    } catch (const std::invalid_argument &) {
        refused = true;
    }
    CK_CHECK(refused);
}
CK_TEST(files_location_route_factory_matches_incremental_bytes_and_bounds_deep_components) {
    for (bool windows_encoding : {false, true}) {
        auto root = windows_encoding ? windows(u"V:/base/") : posix("//base/");
        const auto name = windows_encoding ? *NativeName::from_windows(u"x ") : *NativeName::from_posix("x\\");
        std::vector<NativeName> names(64, name);
        auto incremental = root;
        for (const auto &part : names)
            incremental = take(incremental.descend(part, {}, 64));
        const auto complete = take(FilesPaneLocation::from_components(root.root_address(), names, {}, 64));
        CK_CHECK(complete == incremental && complete.native_bytes() == incremental.native_bytes());
        roundtrip(complete);
        limited(FilesPaneLocation::from_components(root.root_address(), names, {complete.native_bytes() - 1}, 64));
    }
    std::vector<NativeName> deep(4096, *NativeName::from_posix("x"));
    const auto route =
        take(FilesPaneLocation::from_components(take(NativeAbsolutePath::from_posix("/")), std::move(deep), {}, 4096));
    FilesLocationTextLimits limits;
    limits.components = 4096;
    const auto text = take(format_files_exact_location(route, limits));
    CK_CHECK(decode(text, NameEncoding::PosixBytes, limits) == route);
    CK_CHECK(!FilesPaneLocation::from_components(take(NativeAbsolutePath::from_posix("/")),
                                                 {*NativeName::from_windows(u"foreign")}, {}, 1)
                  .value());
    auto name = *NativeName::from_posix("moved");
    const auto retained = std::move(name);
    (void)retained;
    CK_CHECK(!FilesPaneLocation::from_components(take(NativeAbsolutePath::from_posix("/")), {std::move(name)}, {}, 1)
                  .value());
}
