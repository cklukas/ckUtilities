// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "opened_entry.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <stdexcept>
namespace {
using namespace ck::files;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("inode flags fixture unavailable");
    return std::move(value).take_value();
}
class Empty final : public OpenedEntry {
    Observation<BasicMetadata> inspect_basic() const override { throw std::runtime_error("default inode flags performed I/O"); }
};
BasicMetadata metadata() {
    BasicMetadata m; m.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
    m.identity = Observation<ObjectIdentity>::available({AdapterScope{},IdentityScheme::PosixDeviceInode,NativeIdentifier::from_u64(7),NativeIdentifier::from_u64(9),{}}); return m;
}
auto qualify(const BasicMetadata& a, const BasicMetadata& b) { return detail::qualify_inode_flags_subject(Observation<BasicMetadata>::available(a),Observation<BasicMetadata>::available(b)); }
FileInodeFlags flags(std::uint32_t raw) { return {InodeFlagQueryFamily::LinuxIoctlGetFlags,metadata().identity,ObjectKind::RegularFile,raw}; }
constexpr std::array bits{0x4U,0x8U,0x10U,0x20U,0x40U,0x80U,0x400U,0x800U,0x4000U,0x10000U,0x100000U,0x800000U,0x2000000U,0x20000000U,0x40000000U};
}
CK_TEST(inode_flags_default_is_unsupported_and_invalid_limits_do_not_make_native_calls) {
    Empty entry; CK_CHECK(entry.inspect_inode_flags().state() == ObservationState::Unsupported);
    InventoryLimits limits; limits.max_native_bytes = 0; CK_CHECK(entry.inspect_inode_flags(limits).state() == ObservationState::Failed);
}
CK_TEST(inode_flags_literal_mapping_preserves_unknown_bits_and_does_not_mix_statx_dax_namespace) {
    for (std::size_t i = 0; i < bits.size(); ++i) {
        const auto info = flags(bits[i] | 0x80000000U);
        for (std::size_t j = 0; j < bits.size(); ++j)
            CK_CHECK(take(reported_inode_flag(info,static_cast<LinuxInodeFlag>(j))) == (i == j ? ReportedFlag::Set : ReportedFlag::Clear));
        CK_CHECK(info.raw == (bits[i] | 0x80000000U));
    }
    CK_CHECK(take(reported_inode_flag(flags(0x200000U),LinuxInodeFlag::Dax)) == ReportedFlag::Clear);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        CK_CHECK(take(reported_inode_flag(flags(0),static_cast<LinuxInodeFlag>(i))) == ReportedFlag::Clear);
        CK_CHECK(take(reported_inode_flag(flags(0xffffffffU),static_cast<LinuxInodeFlag>(i))) == ReportedFlag::Set);
    }
}
CK_TEST(inode_flags_unknown_family_invalid_kind_and_invalid_feature_never_become_clear) {
    auto info = flags(0); info.family = InodeFlagQueryFamily::Unknown;
    CK_CHECK(reported_inode_flag(info,LinuxInodeFlag::NoDump).state() == ObservationState::Unknown);
    info = flags(0); info.kind = ObjectKind::Directory;
    CK_CHECK(reported_inode_flag(info,LinuxInodeFlag::NoDump).state() == ObservationState::Failed);
    CK_CHECK(reported_inode_flag(flags(0),static_cast<LinuxInodeFlag>(255)).state() == ObservationState::Failed);
}
CK_TEST(inode_flags_subject_requires_regular_file_and_valid_posix_identity) {
    const auto m = metadata(); CK_CHECK(qualify(m,m).value());
    for (auto kind : {ObjectKind::Directory,ObjectKind::SymbolicLink,ObjectKind::Junction,ObjectKind::ReparsePoint,ObjectKind::Fifo,
        ObjectKind::Socket,ObjectKind::CharacterDevice,ObjectKind::BlockDevice,ObjectKind::Other}) {
        auto special = m; special.kind = Observation<ObjectKind>::available(kind);
        CK_CHECK(qualify(special,special).state() == ObservationState::Unsupported);
        CK_CHECK(qualify(m,special).state() == ObservationState::Changed);
    }
    auto invalid = m; invalid.kind = Observation<ObjectKind>::available(static_cast<ObjectKind>(255));
    CK_CHECK(qualify(m,invalid).state() == ObservationState::Failed);
    auto id = take(m.identity); id.scheme = IdentityScheme::WindowsFileId128; invalid = m; invalid.identity = Observation<ObjectIdentity>::available(id);
    CK_CHECK(qualify(invalid,invalid).state() == ObservationState::Unknown);
}
CK_TEST(inode_flags_subject_refuses_changed_recycled_and_incomparable_identity_bookends) {
    const auto before = metadata();
    for (int mode = 0; mode < 6; ++mode) {
        auto after = before; auto id = take(before.identity);
        if (mode == 0) id.object = NativeIdentifier::from_u64(123);
        if (mode == 1) id.volume = NativeIdentifier::from_u64(123);
        if (mode == 2) id.scope = AdapterScope{};
        if (mode == 3) id.object = {};
        if (mode == 4) id.scheme = IdentityScheme::Unknown;
        if (mode == 5) id.generation = 1;
        after.identity = Observation<ObjectIdentity>::available(id); const auto result = qualify(before,after);
        CK_CHECK(result.state() == (mode < 2 ? ObservationState::Changed : ObservationState::Unknown));
    }
}
CK_TEST(inode_flags_subject_preserves_all_unavailable_metadata_kind_and_identity_states) {
    const auto m = metadata();
    for (auto state : {ObservationState::Unsupported,ObservationState::Unknown,ObservationState::Denied,ObservationState::Gone,ObservationState::Changed,ObservationState::Failed})
        for (bool second : {false,true}) for (int field = 0; field < 3; ++field) {
            const NativeError e{ErrorDomain::Posix,13,"injected.inode_flags"}; auto a = Observation<BasicMetadata>::available(m), b = a;
            if (field == 0) { if (second) b = Observation<BasicMetadata>::unavailable(state,e); else a = Observation<BasicMetadata>::unavailable(state,e); }
            else { auto value = m;
                if (field == 1) value.kind = Observation<ObjectKind>::unavailable(state,e); else value.identity = Observation<ObjectIdentity>::unavailable(state,e);
                if (second) b = Observation<BasicMetadata>::available(value); else a = Observation<BasicMetadata>::available(value);
            }
            const auto result = detail::qualify_inode_flags_subject(a,b);
            CK_CHECK(result.state() == state && result.unavailable_detail()->error == e);
        }
}
CK_TEST(inode_flags_access_is_independent_of_layout_attributes_and_security) {
    for (unsigned bits_value = 0; bits_value < 64; ++bits_value) {
        const auto access = static_cast<EntryAccess>(bits_value);
        CK_CHECK(valid_entry_access(access) == ((bits_value & ~63U) == 0 && ((bits_value & 4U) == 0 || (bits_value & 2U) != 0)));
        CK_CHECK(requests_inode_flags(access) == ((bits_value & 16U) != 0));
        CK_CHECK(requests_layout(access) == ((bits_value & 8U) != 0));
    }
}
CK_TEST(inode_flags_simulation_obeys_explicit_access_and_native_byte_limits) {
    using FS = ck::testing::SimulatedFileSystem; FS filesystem;
    const auto root = filesystem.make_node(ObjectKind::Directory), file = filesystem.make_node(ObjectKind::RegularFile);
#ifdef _WIN32
    const std::filesystem::path path(L"V:/simulated-inode-flags");
#else
    const std::filesystem::path path("/simulated-inode-flags");
#endif
    const auto name = *NativeName::from_posix("file"); filesystem.bind(path,root); filesystem.put(root,name,file);
    ck::testing::SimulatedInventory inventory; auto info = flags(0x80000040U); info.owner = filesystem.metadata(file).identity;
    inventory.inode_flags = Observation<FileInodeFlags>::available(info); filesystem.set_inventory(file,inventory);
    auto parent = take(filesystem.open_directory(path));
    for (auto access : {EntryAccess::MetadataOnly,EntryAccess::MetadataAndLayout,EntryAccess::MetadataAndInodeFlags,
        EntryAccess::MetadataAndLayout | EntryAccess::MetadataAndInodeFlags}) {
        auto entry = take(parent->open_child(name,access)); const auto report = entry->inspect_inode_flags();
        CK_CHECK(report.state() == (requests_inode_flags(access) ? ObservationState::Available : ObservationState::Denied));
        if (report.value()) CK_CHECK(report.value()->raw == 0x80000040U);
    }
    auto entry = take(parent->open_child(name,EntryAccess::MetadataAndInodeFlags)); InventoryLimits limits; limits.max_native_bytes = 3;
    CK_CHECK(entry->inspect_inode_flags(limits).state() == ObservationState::Failed);
}
