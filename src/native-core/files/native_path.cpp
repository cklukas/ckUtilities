// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_path.hpp"
#include "windows_name.hpp"
namespace ck::files {
namespace {
template<class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
char16_t upper(char16_t c) noexcept { return c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - u'a' + u'A') : c; }
bool equal_ascii(std::u16string_view a, std::u16string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) if (upper(a[i]) != upper(b[i])) return false;
    return true;
}
bool separator(char16_t c) noexcept { return c == u'\\' || c == u'/'; }
bool components(std::u16string_view tail, bool ordinary) noexcept {
    if (tail.find(u':') != tail.npos || (!ordinary && tail.find(u'/') != tail.npos)) return false;
    while (!tail.empty()) {
        const auto end = tail.find_first_of(ordinary ? u"\\/" : u"\\");
        if (ordinary && detail::win32_device_component(tail.substr(0, end))) return false;
        if (end == tail.npos) break;
        tail.remove_prefix(end + 1);
    }
    return true;
}
bool drive(std::u16string_view path, bool ordinary) noexcept {
    return path.size() >= 3 && upper(path[0]) >= u'A' && upper(path[0]) <= u'Z' && path[1] == u':' &&
        (ordinary ? separator(path[2]) : path[2] == u'\\') && components(path.substr(3), ordinary);
}
bool unc(std::u16string_view tail, bool ordinary) noexcept {
    const auto separators = ordinary ? u"\\/" : u"\\";
    if (!ordinary && tail.find(u'/') != tail.npos) return false;
    const auto end = tail.find_first_of(separators);
    if (end == 0 || end == tail.npos) return false;
    const auto server = tail.substr(0, end);
    if (server == u"." || server == u".." || server == u"?" || server.find(u':') != server.npos) return false;
    tail.remove_prefix(end + 1);
    const auto share_end = tail.find_first_of(separators);
    auto share = tail.substr(0, share_end);
    while (!share.empty() && (share.back() == u' ' || share.back() == u'.')) share.remove_suffix(1);
    if (share.empty() || share.find(u':') != share.npos || equal_ascii(share, u"PIPE") || equal_ascii(share, u"IPC$")) return false;
    return share_end == tail.npos || components(tail.substr(share_end + 1), ordinary);
}
bool volume(std::u16string_view tail) noexcept {
    if (tail.size() < 45 || !equal_ascii(tail.substr(0, 7), u"Volume{") || tail[43] != u'}' || tail[44] != u'\\') return false;
    for (std::size_t i = 0; i < 36; ++i) {
        const auto c = upper(tail[7 + i]);
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != u'-') return false; }
        else if (!((c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'F'))) return false;
    }
    return components(tail.substr(45), false);
}
bool valid_posix(std::string_view bytes) noexcept { return !bytes.empty() && bytes.front() == '/' && bytes.find('\0') == bytes.npos; }
bool valid_windows(std::u16string_view text) noexcept {
    if (text.empty() || text.find(u'\0') != text.npos) return false;
    if (text.starts_with(u"\\\\?\\")) {
        text.remove_prefix(4);
        if (drive(text, false) || volume(text)) return true;
        return text.size() >= 4 && equal_ascii(text.substr(0, 4), u"UNC\\") && unc(text.substr(4), false);
    }
    if (drive(text, true)) return true;
    return text.size() >= 2 && separator(text[0]) && separator(text[1]) && unc(text.substr(2), true);
}
Observation<std::monostate> budget(std::size_t units, std::size_t width, NativePathLimits limits) {
    if (limits.max_native_bytes == 0 || limits.max_native_bytes > NativePathLimits::hard_native_bytes)
        return error<std::monostate>(ObservationState::Failed, AdapterError::InvalidLimit, "native_path.limit");
    if (units > limits.max_native_bytes / width)
        return error<std::monostate>(ObservationState::Unknown, AdapterError::InventoryLimit, "native_path.bytes");
    return Observation<std::monostate>::available({});
}
Observation<NativeAbsolutePath> refuse(const UnavailableObservation& e) {
    return Observation<NativeAbsolutePath>::unavailable(e.state, e.error);
}
}
Observation<NativeAbsolutePath> NativeAbsolutePath::from_posix(std::string_view bytes, NativePathLimits limits) {
    const auto bounded = budget(bytes.size(), 1, limits);
    if (!bounded.value()) return refuse(*bounded.unavailable_detail());
    if (!valid_posix(bytes)) return error<NativeAbsolutePath>(ObservationState::Failed, AdapterError::InvalidPath, "native_path.posix");
    return Observation<NativeAbsolutePath>::available(NativeAbsolutePath(std::string(bytes)));
}
Observation<NativeAbsolutePath> NativeAbsolutePath::from_windows(std::u16string_view units, NativePathLimits limits) {
    const auto bounded = budget(units.size(), sizeof(char16_t), limits);
    if (!bounded.value()) return refuse(*bounded.unavailable_detail());
    if (!valid_windows(units)) return error<NativeAbsolutePath>(ObservationState::Failed, AdapterError::InvalidPath, "native_path.windows");
    return Observation<NativeAbsolutePath>::available(NativeAbsolutePath(std::u16string(units)));
}
Observation<NativeAbsolutePath> NativeAbsolutePath::from_host(const std::filesystem::path& path, NativePathLimits limits) {
#ifdef _WIN32
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    const auto bounded = budget(path.native().size(), sizeof(char16_t), limits);
    if (!bounded.value()) return refuse(*bounded.unavailable_detail());
    std::u16string units; units.reserve(path.native().size());
    for (const auto c : path.native()) units.push_back(static_cast<char16_t>(c));
    return from_windows(units, limits);
#else
    return from_posix(path.native(), limits);
#endif
}
bool NativeAbsolutePath::is_valid() const noexcept {
    if (const auto* bytes = posix_bytes()) return bytes->size() <= NativePathLimits::hard_native_bytes && valid_posix(*bytes);
    const auto* units = windows_units();
    return units && units->size() <= NativePathLimits::hard_native_bytes / sizeof(char16_t) && valid_windows(*units);
}
NameEncoding NativeAbsolutePath::encoding() const noexcept { return posix_bytes() ? NameEncoding::PosixBytes : NameEncoding::WindowsUtf16; }
const std::string* NativeAbsolutePath::posix_bytes() const noexcept { return std::get_if<std::string>(&value_); }
const std::u16string* NativeAbsolutePath::windows_units() const noexcept { return std::get_if<std::u16string>(&value_); }
std::string NativeAbsolutePath::display_text() const {
    return posix_bytes() ? display_native_text(*posix_bytes()) : display_native_text(*windows_units());
}
Observation<std::filesystem::path> NativeAbsolutePath::host_path() const {
    if (!is_valid()) return error<std::filesystem::path>(ObservationState::Failed, AdapterError::InvalidPath, "native_path.moved_or_invalid");
#ifdef _WIN32
    if (const auto* units = windows_units()) {
        static_assert(sizeof(wchar_t) == sizeof(char16_t));
        std::wstring text; text.reserve(units->size());
        for (const auto c : *units) text.push_back(static_cast<wchar_t>(c));
        return Observation<std::filesystem::path>::available(std::filesystem::path(std::move(text)));
    }
#else
    if (const auto* bytes = posix_bytes()) return Observation<std::filesystem::path>::available(std::filesystem::path(*bytes));
#endif
    return error<std::filesystem::path>(ObservationState::Unsupported, AdapterError::NotApplicable, "native_path.foreign_encoding");
}
} // namespace ck::files
