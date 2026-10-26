// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <stdexcept>
namespace {
using namespace ck::files;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("path fixture unavailable");
    return std::move(value).take_value();
}
class Spy final : public BasicFileSystem {
public:
    mutable int calls = 0;
    mutable std::filesystem::path received;
    mutable DirectoryLinkPolicy policy = DirectoryLinkPolicy::RejectFinalLink;
    Observation<BasicMetadata> inspect_basic(const std::filesystem::path& path) const override {
        ++calls; received = path;
        return Observation<BasicMetadata>::unavailable(ObservationState::Denied, {ErrorDomain::Adapter, 42, "spy.metadata"});
    }
    Observation<std::unique_ptr<DirectoryCursor>> open_directory(const std::filesystem::path& path, DirectoryLinkPolicy requested) const override {
        ++calls; received = path; policy = requested;
        return Observation<std::unique_ptr<DirectoryCursor>>::unavailable(ObservationState::Gone, {ErrorDomain::Adapter, 43, "spy.directory"});
    }
};
NativeAbsolutePath host_address() {
#ifdef _WIN32
    return take(NativeAbsolutePath::from_windows((std::u16string(u"V:/path/../raw") + char16_t{0xd800})));
#else
    return take(NativeAbsolutePath::from_posix("//path/../raw\xff"));
#endif
}
NativeAbsolutePath foreign_address() {
#ifdef _WIN32
    return take(NativeAbsolutePath::from_posix("/foreign"));
#else
    return take(NativeAbsolutePath::from_windows(u"V:\\foreign"));
#endif
}
}
CK_TEST(path_posix_preserves_root_slashes_navigation_raw_bytes_and_controls) {
    for (const auto text : {"/", "//", "///", "//host/share//", "/a/.././b/", "/literal\\xFF", "/raw\xff\x1b\n"}) {
        const auto path = take(NativeAbsolutePath::from_posix(text));
        CK_CHECK(path.is_valid() && *path.posix_bytes() == text && !path.windows_units() && path.encoding() == NameEncoding::PosixBytes);
        CK_CHECK(path.display_text() == display_native_text(std::string_view(text)));
    }
    for (int byte = 1; byte <= 255; ++byte) {
        std::string text = "/"; text.push_back(static_cast<char>(byte));
        CK_CHECK(*take(NativeAbsolutePath::from_posix(text)).posix_bytes() == text);
    }
}
CK_TEST(path_posix_rejects_relative_empty_and_embedded_nul_without_cwd_resolution) {
    for (const auto text : {"", "relative", ".", "..", "~/path", "V:/path", "\\rooted"})
        CK_CHECK(NativeAbsolutePath::from_posix(text).state() == ObservationState::Failed);
    const std::string text("/a\0/b",5); CK_CHECK(NativeAbsolutePath::from_posix(text).state() == ObservationState::Failed);
}
CK_TEST(path_windows_accepts_only_qualified_filesystem_namespace_addresses_without_rewriting) {
    for (const auto text : {u"V:\\", u"v:/a/../b/", u"\\\\server\\share", u"//server/share/a", u"\\\\?\\V:\\CON.\\x ",
        u"\\\\?\\UNC\\server\\share\\x", u"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\x"}) {
        const auto path = take(NativeAbsolutePath::from_windows(text));
        CK_CHECK(path.is_valid() && *path.windows_units() == text && !path.posix_bytes() && path.encoding() == NameEncoding::WindowsUtf16);
        CK_CHECK(path.display_text() == display_native_text(std::u16string_view(text)));
    }
}
CK_TEST(path_windows_refuses_relative_devices_streams_pipes_and_malformed_roots) {
    for (const auto text : {u"",u"file",u"V:",u"V:relative",u"\\rooted",u"\\\\server",u"\\\\server\\",u"\\\\.\\PhysicalDrive0",
        u"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1",u"\\\\server\\PIPE\\x",u"\\\\?\\UNC\\server\\IPC$\\x",u"\\\\server\\share:stream",
        u"V:\\file:stream",u"V:\\CON",u"V:\\x\\LPT\u00b9.txt",u"V:/NUL.log",u"\\\\?\\V:/file",u"\\\\?\\V:\\a/b",
        u"\\\\?\\Volume{bad}\\",u"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdeg}\\",u"\\\\?\\V:\\file:stream"})
        CK_CHECK(NativeAbsolutePath::from_windows(text).state() == ObservationState::Failed);
    const std::u16string text(u"V:\\a\0b",6); CK_CHECK(NativeAbsolutePath::from_windows(text).state() == ObservationState::Failed);
}
CK_TEST(path_windows_retains_every_unpaired_surrogate_and_display_never_becomes_the_address) {
    for (unsigned unit = 0xd800; unit <= 0xdfff; ++unit) {
        std::u16string text = u"V:\\"; text.push_back(static_cast<char16_t>(unit));
        const auto path = take(NativeAbsolutePath::from_windows(text));
        CK_CHECK(*path.windows_units() == text && path.display_text().find("\\u{") != std::string::npos);
    }
    const auto raw = take(NativeAbsolutePath::from_posix("/raw\xff"));
    const auto escaped = take(NativeAbsolutePath::from_posix("/raw\\xFF"));
    CK_CHECK(raw != escaped && raw.display_text() != escaped.display_text());
    CK_CHECK(take(NativeAbsolutePath::from_windows(u"V:\\a")) != take(NativeAbsolutePath::from_windows(u"v:\\a")));
}
CK_TEST(path_resource_limits_use_native_bytes_and_never_return_a_prefix) {
    NativePathLimits limits; limits.max_native_bytes = 4;
    CK_CHECK(NativeAbsolutePath::from_posix("/abc", limits).value());
    CK_CHECK(NativeAbsolutePath::from_posix("/abcd", limits).state() == ObservationState::Unknown);
    CK_CHECK(NativeAbsolutePath::from_windows(u"V:\\", limits).state() == ObservationState::Unknown);
    limits.max_native_bytes = 6; CK_CHECK(NativeAbsolutePath::from_windows(u"V:\\", limits).value());
    for (const auto value : {std::size_t{0}, NativePathLimits::hard_native_bytes + 1}) {
        limits.max_native_bytes = value;
        CK_CHECK(NativeAbsolutePath::from_posix("/", limits).state() == ObservationState::Failed);
        CK_CHECK(NativeAbsolutePath::from_windows(u"V:\\", limits).state() == ObservationState::Failed);
    }
    limits.max_native_bytes = NativePathLimits::hard_native_bytes;
    const auto huge = "/" + std::string(limits.max_native_bytes - 1, 'a');
    CK_CHECK(NativeAbsolutePath::from_posix(huge, limits).value());
    CK_CHECK(NativeAbsolutePath::from_posix(huge + 'a', limits).state() == ObservationState::Unknown);
}
CK_TEST(path_host_bridge_preserves_exact_native_units_and_refuses_foreign_encoding) {
    const auto path = host_address(); const auto host = take(path.host_path());
    CK_CHECK(take(NativeAbsolutePath::from_host(host)) == path);
    CK_CHECK(foreign_address().host_path().state() == ObservationState::Unsupported);
    CK_CHECK(!NativeAbsolutePath::from_host(std::filesystem::path("relative")).value());
    Spy spy; CK_CHECK(spy.inspect_native_basic(path).state() == ObservationState::Denied && spy.received.native() == host.native());
    const auto result = spy.open_native_directory(path, DirectoryLinkPolicy::FollowFinalLink);
    CK_CHECK(result.state() == ObservationState::Gone && result.unavailable_detail()->error.operation == "spy.directory");
    CK_CHECK(spy.policy == DirectoryLinkPolicy::FollowFinalLink && spy.received.native() == host.native() && spy.calls == 2);
}
CK_TEST(path_rejected_bridges_make_no_adapter_calls_and_moved_values_do_not_mean_cwd) {
    Spy spy; const auto foreign = foreign_address();
    CK_CHECK(!spy.inspect_native_basic(foreign).value() && !spy.open_native_directory(foreign).value());
    auto source = host_address(); auto moved = std::move(source);
    CK_CHECK(moved.is_valid() && !source.is_valid() && !source.host_path().value());
    CK_CHECK(!spy.inspect_native_basic(source).value() && !spy.open_native_directory(source).value());
    CK_CHECK(!spy.open_native_directory(moved, static_cast<DirectoryLinkPolicy>(255)).value() && spy.calls == 0);
    source = moved; CK_CHECK(source == moved); source = std::move(moved); CK_CHECK(source.is_valid() && !moved.is_valid());
}

CK_TEST(path_requested_parent_and_root_preserve_posix_prefixes_and_never_resolve_dot_or_raw_names) {
    struct Case { const char* path; const char* parent; const char* root; };
    for (const auto& c : {Case{"/", "/", "/"}, {"//", "//", "//"}, {"///", "///", "///"},
        {"//host/share//", "//host", "//"}, {"/a/.././b/", "/a/../.", "/"},
        {"/a//b///", "/a", "/"}, {"/raw\\name\n\xff", "/", "/"}, {"///a", "///", "///"}}) {
        const auto p = take(NativeAbsolutePath::from_posix(c.path));
        const auto parent = take(p.parent_address()), root = take(p.namespace_root());
        CK_CHECK(*parent.posix_bytes() == c.parent && *root.posix_bytes() == c.root);
        CK_CHECK(take(root.parent_address()) == root && take(root.namespace_root()) == root);
    }
}
CK_TEST(path_requested_windows_parent_and_root_respect_drive_unc_volume_and_extended_literal_names) {
    struct Case { const char16_t* path; const char16_t* parent; const char16_t* root; };
    for (const auto& c : {Case{u"V:\\", u"V:\\", u"V:\\"}, {u"v:/a/../b/", u"v:/a/..", u"v:/"},
        {u"\\\\server\\share", u"\\\\server\\share", u"\\\\server\\share"},
        {u"//server/share/a", u"//server/share/", u"//server/share/"},
        {u"\\\\s\\share\\a\\b\\", u"\\\\s\\share\\a", u"\\\\s\\share\\"},
        {u"\\\\?\\V:\\CON.\\x ", u"\\\\?\\V:\\CON.", u"\\\\?\\V:\\"},
        {u"\\\\?\\UNC\\server\\share\\x", u"\\\\?\\UNC\\server\\share\\", u"\\\\?\\UNC\\server\\share\\"},
        {u"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\x\\y",
         u"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\x", u"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\"}}) {
        const auto p = take(NativeAbsolutePath::from_windows(c.path));
        const auto parent = take(p.parent_address()), root = take(p.namespace_root());
        CK_CHECK(*parent.windows_units() == c.parent && *root.windows_units() == c.root);
        CK_CHECK(take(root.parent_address()) == root && take(root.namespace_root()) == root);
    }
    const auto raw = take(NativeAbsolutePath::from_windows(std::u16string(u"V:\\raw") + char16_t{0xd800} + u"\\leaf"));
    CK_CHECK(*take(raw.parent_address()).windows_units() == std::u16string(u"V:\\raw") + char16_t{0xd800});
}
CK_TEST(path_requested_parent_and_root_refuse_moved_values_and_keep_large_qualified_addresses) {
    auto source = host_address(); const auto moved = std::move(source);
    CK_CHECK(!source.parent_address().value() && !source.namespace_root().value());
    CK_CHECK(moved.parent_address().value() && moved.namespace_root().value());
    const auto p = take(NativeAbsolutePath::from_posix("/" + std::string(300000, 'a') + "/b", {NativePathLimits::hard_native_bytes}));
    CK_CHECK(take(p.parent_address()).posix_bytes()->size() == 300001);
    CK_CHECK(*take(p.namespace_root()).posix_bytes() == "/");
}

CK_TEST(path_child_address_preserves_native_units_and_refuses_foreign_or_oversize_components) {
    const auto posix = take(NativeAbsolutePath::from_posix("/root/"));
    std::string raw_bytes = "raw\\name"; raw_bytes.push_back(static_cast<char>(0xff));
    const auto raw = *NativeName::from_posix(raw_bytes);
    CK_CHECK(*take(posix.child_address(raw)).posix_bytes() == std::string("/root/") + raw_bytes);
    const auto windows = take(NativeAbsolutePath::from_windows(u"V:/root/"));
    const auto name = *NativeName::from_windows(std::u16string(u"raw") + char16_t{0xd800});
    CK_CHECK(*take(windows.child_address(name)).windows_units() ==
        std::u16string(u"V:/root/raw") + char16_t{0xd800});
    CK_CHECK(posix.child_address(name).state() == ObservationState::Failed);
    NativePathLimits limits; limits.max_native_bytes = 5;
    CK_CHECK(posix.child_address(*NativeName::from_posix("long"), limits).state() == ObservationState::Unknown);
    auto moved = posix; const auto empty = std::move(moved);
    CK_CHECK(!moved.child_address(*NativeName::from_posix("x")).value());
    CK_CHECK(empty.child_address(*NativeName::from_posix("x")).value());
}

CK_TEST(path_relative_components_are_exact_and_refuse_non_ancestors_or_foreign_encodings) {
    const auto root = take(NativeAbsolutePath::from_posix("/source/root"));
    const auto nested = take(NativeAbsolutePath::from_posix("/source/root/dir/raw"));
    const auto components = take(nested.relative_components_from(root));
    CK_CHECK(components.size() == 2 && *components[0].posix_bytes() == "dir" &&
        *components[1].posix_bytes() == "raw");
    CK_CHECK(take(root.relative_components_from(root)).empty());
    CK_CHECK(nested.relative_components_from(take(NativeAbsolutePath::from_posix("/source/other"))).state() ==
        ObservationState::Changed);
    CK_CHECK(nested.relative_components_from(take(NativeAbsolutePath::from_posix("/source/rootish"))).state() ==
        ObservationState::Changed);
    CK_CHECK(nested.relative_components_from(take(NativeAbsolutePath::from_windows(u"V:\\source\\root"))).state() ==
        ObservationState::Unsupported);

    const auto windows_root = take(NativeAbsolutePath::from_windows(u"V:\\source\\root"));
    const auto windows_nested = take(NativeAbsolutePath::from_windows(u"V:\\source\\root\\dir\\raw"));
    const auto windows_components = take(windows_nested.relative_components_from(windows_root));
    CK_CHECK(windows_components.size() == 2 && *windows_components[0].windows_units() == u"dir" &&
        *windows_components[1].windows_units() == u"raw");
}
