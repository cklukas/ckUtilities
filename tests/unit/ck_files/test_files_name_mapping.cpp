// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "name_mapping.hpp"
#include "unicode_scalar.hpp"
#include "windows_name.hpp"
#include "cvision/testing/cktest.hpp"
#include <stdexcept>
#include <vector>

namespace {
using namespace ck::files;
template<class T> const T& required(const Observation<T>& value) { if(!value.value())throw std::runtime_error("name mapping fixture unavailable");return *value.value(); }
NativeName posix(std::string value) { return *NativeName::from_posix(std::move(value)); }
NativeName windows(std::u16string value) { return *NativeName::from_windows(std::move(value)); }
}
CK_TEST(unicode_scalar_codecs_round_trip_every_unicode_scalar_including_zero_and_noncharacters) {
    bool all=true;std::uint32_t checked=0;
    for(std::uint32_t scalar=0;scalar<=0x10ffffU;++scalar) {
        if(scalar>=0xd800U&&scalar<=0xdfffU)continue;
        const auto utf8=detail::encode_utf8_scalar(scalar);const auto utf16=detail::encode_utf16_scalar(scalar);
        const auto a=detail::decode_utf8_scalar({utf8.data.data(),utf8.units});
        const auto b=detail::decode_utf16_scalar({utf16.data.data(),utf16.units});
        if(utf8.units==0||utf16.units==0||a.value!=scalar||b.value!=scalar||a.units!=utf8.units||b.units!=utf16.units){all=false;break;}
        ++checked;
    }
    CK_CHECK(all&&checked==0x110000U-0x800U);
    for(auto invalid:{0xd800U,0xdbffU,0xdc00U,0xdfffU,0x110000U,0xffffffffU}) {
        CK_CHECK(detail::encode_utf8_scalar(invalid).units==0&&detail::encode_utf16_scalar(invalid).units==0);
    }
}
CK_TEST(unicode_scalar_boundaries_match_independent_utf8_and_utf16_vectors) {
    struct Example {std::uint32_t scalar;std::string bytes;std::u16string units;};
    for(const auto& e:std::vector<Example>{{0x7f,"\x7f",u"\x7f"},{0x80,"\xc2\x80",u"\x80"},{0x7ff,"\xdf\xbf",u"\x7ff"},
        {0x800,"\xe0\xa0\x80",u"\x800"},{0xd7ff,"\xed\x9f\xbf",u"\xd7ff"},{0xe000,"\xee\x80\x80",u"\xe000"},
        {0xffff,"\xef\xbf\xbf",u"\xffff"},{0x10000,"\xf0\x90\x80\x80",std::u16string{0xd800,0xdc00}},
        {0x10ffff,"\xf4\x8f\xbf\xbf",std::u16string{0xdbff,0xdfff}}}) {
        const auto a=detail::encode_utf8_scalar(e.scalar);const auto b=detail::encode_utf16_scalar(e.scalar);
        CK_CHECK(std::string(a.data.data(),a.units)==e.bytes&&std::u16string(b.data.data(),b.units)==e.units);
        CK_CHECK(detail::decode_utf8_scalar(e.bytes).value==e.scalar&&detail::decode_utf16_scalar(e.units).value==e.scalar);
    }
}
CK_TEST(strict_utf8_decoder_rejects_malformed_sequences_without_consuming_a_prefix) {
    for(const auto& bytes:std::vector<std::string>{"","\x80","\xff","\xc0\xaf","\xc1\xbf","\xc2","\xc2X","\xe0\x80\xaf",
        "\xed\xa0\x80","\xed\xbf\xbf","\xf0\x80\x80\x80","\xf4\x90\x80\x80","\xf5\x80\x80\x80","\xf8\x88\x80\x80\x80","\xe2\x82"})
        CK_CHECK(detail::decode_utf8_scalar(bytes).units==0);
    CK_CHECK(detail::decode_utf8_scalar("x\x80").units==1); // a scalar decoder, not a whole-string validator
}
CK_TEST(strict_utf16_decoder_rejects_unpaired_and_reversed_surrogates_without_consumption) {
    for(const auto& units:std::vector<std::u16string>{{},{0xd800},{0xdc00},{0xd800,u'x'},{0xdc00,0xd800},{0xd800,0xdbff}})
        CK_CHECK(detail::decode_utf16_scalar(units).units==0);
    const auto decoded=detail::decode_utf16_scalar(std::u16string{0xd800,0xdc00,u'x'});
    CK_CHECK(decoded.units==2&&decoded.value==0x10000);
}
CK_TEST(native_name_mapping_round_trips_scalars_without_case_normalization_or_bom_changes) {
    for(const auto& text:std::vector<std::u16string>{u"plain",u"\u65e5\u672c-\U0001f642",u"\u00e9",u"e\u0301",u"\ufeff-leading",u"line\ncontrol",u"\ufffe"}) {
        const auto source=windows(text);const auto mapped=required(map_native_name(source,NameEncoding::PosixBytes));
        CK_CHECK(required(map_native_name(mapped,NameEncoding::WindowsUtf16))==source);
    }
    CK_CHECK(required(map_native_name(windows(u"\u00e9"),NameEncoding::PosixBytes))!=required(map_native_name(windows(u"e\u0301"),NameEncoding::PosixBytes)));
    CK_CHECK(*required(map_native_name(windows(u"\ufeff-x"),NameEncoding::PosixBytes)).posix_bytes()=="\xef\xbb\xbf-x");
}
CK_TEST(same_native_encoding_preserves_non_unicode_bytes_and_unpaired_utf16) {
    const auto bytes=posix("raw-\xff\\\n");const auto units=windows(std::u16string{u'x',0xd800,0xdc01,0xdfff});
    CK_CHECK(required(map_native_name(bytes,NameEncoding::PosixBytes))==bytes);
    CK_CHECK(required(map_native_name(units,NameEncoding::WindowsUtf16))==units);
    CK_CHECK(*required(map_native_name(bytes,NameEncoding::PosixBytes)).posix_bytes()==*bytes.posix_bytes());
}
CK_TEST(cross_encoding_mapping_never_escapes_replaces_or_returns_partially_mapped_names) {
    for(const auto& bytes:std::vector<std::string>{"ok-\xff","ok-\xc0\xaf","ok-\xed\xa0\x80","a\\b"}) {
        const auto result=map_native_name(posix(bytes),NameEncoding::WindowsUtf16);
        CK_CHECK(result.state()==ObservationState::Unsupported&&!result.value()&&result.unavailable_detail()->error.code==static_cast<std::int64_t>(AdapterError::UnrepresentableName));
    }
    const auto invalid=map_native_name(windows(std::u16string{u'o',u'k',0xd800}),NameEncoding::PosixBytes);
    CK_CHECK(invalid.state()==ObservationState::Unsupported&&!invalid.value());
    CK_CHECK(required(map_native_name(posix("\\xFF"),NameEncoding::PosixBytes))==posix("\\xFF"));
}
CK_TEST(name_mapping_checks_input_output_and_configuration_budgets_in_native_bytes) {
    auto moving=posix("moving");const auto owned=std::move(moving);CK_CHECK(owned.is_valid());
    // The standard permits a moved-from string to remain nonempty. Whichever
    // state the implementation chooses, it cannot become an empty ready name.
    const auto moved=map_native_name(moving,NameEncoding::PosixBytes);
    CK_CHECK(moving.is_valid() ? moved.state()==ObservationState::Available : moved.state()==ObservationState::Failed);
    const auto a=posix("a"),abc=posix("abc"),cjk=windows(u"\u65e5");
    CK_CHECK(map_native_name(a,NameEncoding::WindowsUtf16,{1}).state()==ObservationState::Unknown);
    CK_CHECK(map_native_name(a,NameEncoding::WindowsUtf16,{2}).state()==ObservationState::Available);
    CK_CHECK(map_native_name(abc,NameEncoding::WindowsUtf16,{5}).state()==ObservationState::Unknown);
    CK_CHECK(map_native_name(abc,NameEncoding::WindowsUtf16,{6}).state()==ObservationState::Available);
    CK_CHECK(map_native_name(cjk,NameEncoding::PosixBytes,{2}).state()==ObservationState::Unknown);
    CK_CHECK(map_native_name(cjk,NameEncoding::PosixBytes,{3}).state()==ObservationState::Available);
    CK_CHECK(map_native_name(cjk,NameEncoding::WindowsUtf16,{1}).state()==ObservationState::Unknown);
    CK_CHECK(map_native_name(abc,NameEncoding::PosixBytes,{2}).state()==ObservationState::Unknown);
    CK_CHECK(map_native_name(a,NameEncoding::PosixBytes,{0}).state()==ObservationState::Failed);
    CK_CHECK(map_native_name(a,NameEncoding::PosixBytes,{NameMappingLimits::hard_native_bytes+1}).state()==ObservationState::Failed);
    for(auto encoding:{NameEncoding::WindowsEaBytes,static_cast<NameEncoding>(999)})
        CK_CHECK(map_native_name(a,encoding).state()==ObservationState::Failed);
}
CK_TEST(win32_component_checks_identify_forbidden_units_including_stream_syntax_and_controls) {
    for(char16_t unit:std::u16string(u"<>:\"/\\|?*")) {
        const auto check=check_win32_component(std::u16string{u'a',unit,u'z'});
        CK_CHECK(check.has(Win32NameIssue::ForbiddenCharacter)&&check.first_forbidden_unit==1&&!check.ordinary_component());
    }
    for(char16_t unit=0;unit<32;++unit)CK_CHECK(check_win32_component(std::u16string{unit,u'*'}).first_forbidden_unit==0);
    const auto stream=check_win32_component(u"file:channel:$DATA");CK_CHECK(stream.has(Win32NameIssue::ForbiddenCharacter)&&stream.first_forbidden_unit==4);
}
CK_TEST(win32_device_alias_risks_include_extensions_superscript_digits_and_console_aliases) {
    for(const auto& value:std::vector<std::u16string>{u"CON",u"con.txt",u"CON .txt",u"PRN",u"aux.",u"NUL",u"CONIN$",u"conout$",u"COM1",u"com9.ext",u"LPT1",u"lpt9",u"COM\u00b9",u"LPT\u00b2.ext",u"COM\u00b3 "})
    {
        CK_CHECK(check_win32_component(value).has(Win32NameIssue::DeviceAliasRisk));
        std::wstring native;for(auto unit:value)native.push_back(static_cast<wchar_t>(unit));
        CK_CHECK(detail::win32_device_component(std::wstring_view(native)));
    }
    for(const auto& value:std::vector<std::u16string>{u"COM0",u"COM10",u"LPT0",u"LPT10",u"xCON",u"NULx",u".CON",u"COM4extra",u"\uff23\uff2f\uff2e"})
        CK_CHECK(check_win32_component(value).ordinary_component());
}
CK_TEST(win32_component_checks_report_navigation_empty_and_trailing_rewrite_risks_without_rewriting) {
    CK_CHECK(check_win32_component(u"").has(Win32NameIssue::Empty));
    for(const auto value:{u".",u".."})CK_CHECK(check_win32_component(value).has(Win32NameIssue::Navigation));
    for(const auto value:{u"file.",u"file ",u"file... ",u" "})CK_CHECK(check_win32_component(value).has(Win32NameIssue::TrailingDotOrSpace));
    for(const auto& value:std::vector<std::u16string>{u"ordinary.txt",u"Case",u"case",u"\u65e5\u672c-\U0001f642",std::u16string{u'x',0xd800}})
        CK_CHECK(check_win32_component(value).ordinary_component());
}
CK_TEST(successful_name_transport_is_independent_of_win32_compatibility_and_display_escaping) {
    for(const auto& bytes:std::vector<std::string>{"CON","file.","file:stream","line\ncontrol"}) {
        const auto source=posix(bytes);const auto mapped=required(map_native_name(source,NameEncoding::WindowsUtf16));
        CK_CHECK(!check_win32_component(*mapped.windows_units()).ordinary_component());
        CK_CHECK(required(map_native_name(mapped,NameEncoding::PosixBytes))==source);
    }
    const auto newline=required(map_native_name(posix("line\n"),NameEncoding::WindowsUtf16));
    CK_CHECK(newline.display_name()=="line\\u{000A}"&&*newline.windows_units()==u"line\n");
}
