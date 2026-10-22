// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_name.hpp"

#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "cvision/testing/cktest.hpp"

namespace {
using ck::files::NameEncoding;
using ck::files::NativeName;

NativeName posix(std::string bytes) {
    auto name = NativeName::from_posix(std::move(bytes));
    if (!name) throw std::runtime_error("invalid test name");
    return std::move(*name);
}

NativeName windows(std::u16string units) {
    auto name = NativeName::from_windows(std::move(units));
    if (!name) throw std::runtime_error("invalid test name");
    return std::move(*name);
}

std::string utf8(std::u8string_view text) {
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
}

CK_TEST(native_names_enforce_component_boundaries_without_normalizing) {
    for (const auto* invalid : {"", ".", "..", "a/b", "/"})
        CK_CHECK(!NativeName::from_posix(invalid));
    CK_CHECK(!NativeName::from_posix(std::string("a\0b", 3)));
    for (const auto* invalid : {u"", u".", u"..", u"a/b", u"a\\b", u"\\"})
        CK_CHECK(!NativeName::from_windows(invalid));
    CK_CHECK(!NativeName::from_windows(std::u16string(u"a\0b", 3)));
    CK_CHECK(posix("a\\b").posix_bytes() && *posix("a\\b").posix_bytes() == "a\\b");
    CK_CHECK(*posix("trailing. ").posix_bytes() == "trailing. ");
    CK_CHECK(*windows(u"CON").windows_units() == u"CON");
    CK_CHECK(*windows(u"trailing. ").windows_units() == u"trailing. ");
    CK_CHECK(posix("Name") != posix("name"));
    CK_CHECK(posix(utf8(u8"é")) != posix(utf8(u8"e\u0301")));
}

CK_TEST(native_name_identity_includes_encoding_and_preserves_all_raw_units) {
    const auto raw = std::string("\xFF\xFE\xC0", 3);
    const auto bytes = posix(raw);
    CK_CHECK(bytes.encoding() == NameEncoding::PosixBytes);
    CK_CHECK(bytes.posix_bytes() && *bytes.posix_bytes() == raw);
    CK_CHECK(bytes.windows_units() == nullptr);
    const std::u16string unusual{u'a', static_cast<char16_t>(0xD800), u'b'};
    const auto units = windows(unusual);
    CK_CHECK(units.encoding() == NameEncoding::WindowsUtf16);
    CK_CHECK(units.windows_units() && *units.windows_units() == unusual);
    CK_CHECK(units.posix_bytes() == nullptr);
    CK_CHECK(posix("same") != windows(u"same"));
}

CK_TEST(native_display_retains_readable_Unicode_spelling) {
    CK_CHECK(posix("notes.txt").display_name() == "notes.txt");
    const auto text = utf8(u8"日本語-Ä-e\u0301-👩‍💻.txt");
    CK_CHECK(posix(text).display_name() == text);
    CK_CHECK(windows(u"日本語-Ä-e\u0301-👩‍💻.txt").display_name() == text);
    CK_CHECK(windows(std::u16string{static_cast<char16_t>(0xD800), static_cast<char16_t>(0xDC00)}).display_name() == "\xF0\x90\x80\x80");
    CK_CHECK(posix("\xF4\x8F\xBF\xBD").display_name() == "\xF4\x8F\xBF\xBD");
    for (const auto* boundary : {"\xC2\xA0", "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80", "\xF0\x90\x80\x80"})
        CK_CHECK(posix(boundary).display_name() == boundary);
}

CK_TEST(native_display_escapes_controls_without_ambiguous_literal_escapes) {
    CK_CHECK(posix("a\n\t\x1B[31m").display_name() == "a\\u{000A}\\u{0009}\\u{001B}[31m");
    CK_CHECK(posix("\\xFF").display_name() == "\\\\xFF");
    CK_CHECK(posix("\xFF").display_name() == "\\xFF");
    CK_CHECK(posix("\\u{202E}").display_name() == "\\\\u{202E}");
    CK_CHECK(posix(utf8(u8"a\u202Eb\u2066c\u200F")).display_name() == "a\\u{202E}b\\u{2066}c\\u{200F}");
    CK_CHECK(windows(u"a\u2028b\u2029c\uFEFF").display_name() == "a\\u{2028}b\\u{2029}c\\u{FEFF}");
    CK_CHECK(posix("\xC2\x80").display_name() == "\\u{0080}");
    CK_CHECK(posix("\xEF\xBF\xBE").display_name() == "\\u{FFFE}");
    CK_CHECK(windows(std::u16string{static_cast<char16_t>(0xDBFF), static_cast<char16_t>(0xDFFF)}).display_name() == "\\u{10FFFF}");
}

CK_TEST(malformed_UTF8_is_escaped_byte_for_byte_rather_than_replaced) {
    for (const auto& [raw, expected] : {
             std::pair{"\xC0\xAF", "\\xC0\\xAF"},
             std::pair{"\xE0\x80\xAF", "\\xE0\\x80\\xAF"},
             std::pair{"\xED\xA0\x80", "\\xED\\xA0\\x80"},
             std::pair{"\xF4\x90\x80\x80", "\\xF4\\x90\\x80\\x80"},
             std::pair{"\xF0\x80\x80\x80", "\\xF0\\x80\\x80\\x80"},
             std::pair{"\xE2\x82", "\\xE2\\x82"},
             std::pair{"\xE2" "A\xAC", "\\xE2A\\xAC"}})
        CK_CHECK(posix(raw).display_name() == expected);
    CK_CHECK(posix("\xEF\xBF\xBD").display_name() != posix("\xFF").display_name());
}

CK_TEST(all_unpaired_surrogate_units_remain_distinguishable) {
    std::set<std::string> displays;
    for (unsigned int unit = 0xD800; unit <= 0xDFFF; ++unit) {
        const auto name = windows(std::u16string{static_cast<char16_t>(unit)});
        CK_CHECK(name.windows_units()->front() == unit);
        const auto display = name.display_name();
        char expected[20];
        std::snprintf(expected, sizeof(expected), "\\u{%04X}", unit);
        CK_CHECK(display == expected);
        CK_CHECK(displays.insert(display).second);
    }
    CK_CHECK(displays.size() == 2048);
    CK_CHECK(windows(std::u16string{static_cast<char16_t>(0xDC00), static_cast<char16_t>(0xD800)}).display_name() == "\\u{DC00}\\u{D800}");
}

CK_TEST(single_POSIX_bytes_and_literal_escape_spellings_never_alias) {
    std::set<std::string> displays;
    for (unsigned int byte = 1; byte <= 255; ++byte) {
        const auto raw = std::string(1, static_cast<char>(byte));
        auto name = NativeName::from_posix(raw);
        if (!name) continue; // Navigation '.' and the separator are not names.
        CK_CHECK(*name->posix_bytes() == raw);
        const auto display = name->display_name();
        CK_CHECK(displays.insert(display).second);
        if (display != raw) CK_CHECK(posix(display).display_name() != display);
        for (unsigned char value : display) CK_CHECK(value >= 0x20U && value != 0x7FU);
    }
    CK_CHECK(displays.size() == 253);
}

CK_TEST(native_names_do_not_assume_the_completion_services_size_limits) {
    const std::string bytes(10000, 'x');
    const std::u16string units(10000, u'x');
    CK_CHECK(*posix(bytes).posix_bytes() == bytes);
    CK_CHECK(*windows(units).windows_units() == units);
    CK_CHECK(posix(bytes).display_name() == bytes);
    CK_CHECK(windows(units).display_name() == bytes);
}
