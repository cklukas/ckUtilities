// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_entry_probe.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <stdexcept>
namespace {
using namespace ck::files;
using FileSystem = ck::testing::SimulatedFileSystem;
using Operation = FileSystem::Operation;
NativeName name(const char* text) { return *NativeName::from_posix(text); }
std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/simulated-probe";
#else
    return "/simulated-probe";
#endif
}
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required probe fixture observation unavailable");
    return std::move(value).take_value();
}
struct Fixture {
    FileSystem fs;
    FileSystem::Pointer root = fs.make_node(ObjectKind::Directory);
    Fixture() {
        fs.bind(path(), root); LocationInfo location; location.family = LocationQueryFamily::LinuxDescriptor;
        location.directory = fs.metadata(root).identity; location.posix_name_max = Observation<std::uint64_t>::available(255);
        fs.set_location(root, Observation<LocationInfo>::available(std::move(location)));
    }
    std::shared_ptr<const DirectoryCursor> cursor() { return std::shared_ptr<const DirectoryCursor>(take(fs.open_directory(path()))); }
};
UnavailableObservation denied() { return {ObservationState::Denied, {ErrorDomain::Adapter, 13, "probe.denied"}}; }
class BadCursor final : public DirectoryCursor {
public:
    explicit BadCursor(int mode) : mode_(mode) {}
    Observation<ChildLookup> lookup_child(const NativeName& requested, ChildLookupMode mode) const override {
        if (mode_ == 0) return Observation<ChildLookup>::available(ChildLookup{});
        auto context = ChildLookupContext{mode_ == 1 ? name("other") : requested,
            mode_ == 2 ? ChildLookupMode::WindowsIgnoreCase : mode, identity()};
        if (mode_ == 3) context.directory = Observation<ObjectIdentity>::unavailable(ObservationState::Denied, {ErrorDomain::Adapter, 2, "wrong.parent"});
        return Observation<ChildLookup>::available(ChildLookup::missing(std::move(context), {ErrorDomain::Adapter, 2, "fake.missing"}));
    }
private:
    Observation<DirectoryStep> next_step() override { return Observation<DirectoryStep>::available(DirectoryEnd{}); }
    int mode_;
};
}
CK_TEST(entry_probe_keeps_found_handles_and_immutable_metadata_independent_across_refresh) {
    Fixture fixture; auto file = fixture.fs.make_node(ObjectKind::RegularFile); fixture.fs.put(fixture.root, name("a"), file);
    fixture.fs.metadata(file).logical_size = Observation<std::uint64_t>::available(12);
    auto parent = fixture.cursor(); auto probe = take(DirectoryEntryProbe::inspect(parent, name("a"))); parent.reset();
    CK_CHECK(take(probe->existing_metadata()).logical_size.value() && take(take(probe->existing_metadata()).logical_size) == 12);
    CK_CHECK(probe->lookup().value() && probe->lookup().value()->state() == ChildLookupState::Found);
    CK_CHECK(probe->length().value() && !probe->length().value()->exceeds_reported_maximum());
    fixture.fs.metadata(file).logical_size = Observation<std::uint64_t>::available(99);
    auto fresh = take(probe->refresh());
    CK_CHECK(take(take(probe->existing_metadata()).logical_size) == 12 && take(take(fresh->existing_metadata()).logical_size) == 99);
    CK_CHECK(fixture.fs.live_entries() == 2 && fixture.fs.live_cursors() == 1);
    probe.reset(); fresh.reset(); CK_CHECK(fixture.fs.live_entries() == 0 && fixture.fs.live_cursors() == 0);
}
CK_TEST(entry_probe_missing_is_only_a_momentary_lookup_and_refresh_uses_the_held_parent) {
    Fixture fixture; auto parent = fixture.cursor(); auto before = take(DirectoryEntryProbe::inspect(parent, name("a")));
    CK_CHECK(before->lookup().value() && before->lookup().value()->state() == ChildLookupState::Missing && before->lookup().value()->missing_witness());
    CK_CHECK(before->existing_metadata().state() == ObservationState::Unsupported);
    fixture.fs.bind(path(), fixture.fs.make_node(ObjectKind::Directory));
    auto file = fixture.fs.make_node(ObjectKind::RegularFile); fixture.fs.put(fixture.root, name("a"), file);
    auto after = take(before->refresh());
    CK_CHECK(after->lookup().value() && after->lookup().value()->state() == ChildLookupState::Found);
    CK_CHECK(before->lookup().value()->state() == ChildLookupState::Missing);
    CK_CHECK(compare_identity(take(take(after->existing_metadata()).identity), take(fixture.fs.metadata(file).identity)) == IdentityComparison::Same);
}
CK_TEST(entry_probe_keeps_location_lookup_and_metadata_failures_separate) {
    Fixture fixture; fixture.fs.put(fixture.root, name("a"), fixture.fs.make_node(ObjectKind::RegularFile));
    auto parent = fixture.cursor(); fixture.fs.enqueue(Operation::InspectLocation, {denied(), {}, {}, false});
    auto probe = take(DirectoryEntryProbe::inspect(parent, name("a")));
    CK_CHECK(probe->location().state() == ObservationState::Denied && probe->length().state() == ObservationState::Denied);
    CK_CHECK(probe->lookup().value() && probe->existing_metadata().value());
    fixture.fs.enqueue(Operation::LookupChild, {denied(), {}, {}, false});
    auto refused = take(probe->refresh());
    CK_CHECK(refused->location().value() && refused->lookup().state() == ObservationState::Denied && refused->existing_metadata().state() == ObservationState::Denied);
    CK_CHECK(refused->existing_metadata().unavailable_detail() && refused->existing_metadata().unavailable_detail()->error == denied().error);
}
CK_TEST(entry_probe_inspection_denial_does_not_hide_the_found_object_or_release_its_handle) {
    Fixture fixture; fixture.fs.put(fixture.root, name("a"), fixture.fs.make_node(ObjectKind::RegularFile));
    fixture.fs.enqueue(Operation::InspectHandle, {denied(), {}, {}, false});
    auto probe = take(DirectoryEntryProbe::inspect(fixture.cursor(), name("a")));
    CK_CHECK(probe->lookup().value() && probe->lookup().value()->state() == ChildLookupState::Found);
    CK_CHECK(probe->existing_metadata().state() == ObservationState::Denied && fixture.fs.live_entries() == 1);
    probe.reset(); CK_CHECK(fixture.fs.live_entries() == 0 && fixture.fs.live_cursors() == 0);
}
CK_TEST(entry_probe_validates_bounds_and_inputs_before_native_queries_and_counts_utf16_bytes) {
    Fixture fixture; auto parent = fixture.cursor(); const auto calls = fixture.fs.calls().size();
    CK_CHECK(!DirectoryEntryProbe::inspect(nullptr, name("a")).value());
    CK_CHECK(!DirectoryEntryProbe::inspect(parent, name("a"), static_cast<ChildLookupMode>(999)).value());
    for (auto limit : {std::size_t{0}, DirectoryEntryProbeLimits::hard_component_bytes + 1})
        CK_CHECK(DirectoryEntryProbe::inspect(parent, name("a"), ChildLookupMode::NativeDefault, {limit}).state() == ObservationState::Failed);
    CK_CHECK(DirectoryEntryProbe::inspect(parent, *NativeName::from_windows(u"abc"), ChildLookupMode::NativeDefault, {5}).state() == ObservationState::Unknown);
    CK_CHECK(fixture.fs.calls().size() == calls);
    auto exact = DirectoryEntryProbe::inspect(parent, *NativeName::from_windows(u"abc"), ChildLookupMode::NativeDefault, {6});
    CK_CHECK(exact.value() && !fixture.fs.calls().empty());
}
CK_TEST(entry_probe_rejects_unknown_or_mismatched_lookup_context_without_fabricating_absence) {
    for (int mode = 0; mode < 4; ++mode) {
        auto probe = take(DirectoryEntryProbe::inspect(std::make_shared<BadCursor>(mode), name("a")));
        CK_CHECK(probe->lookup().state() == ObservationState::Failed && probe->existing_metadata().state() == ObservationState::Failed);
    }
    auto unqualified = take(DirectoryEntryProbe::inspect(std::make_shared<BadCursor>(4), name("a")));
    CK_CHECK(unqualified->lookup().value() && unqualified->lookup().value()->state() == ChildLookupState::Missing);
    CK_CHECK(unqualified->lookup().value()->context()->directory.state() == ObservationState::Unknown);
}
CK_TEST(entry_probe_exceptions_release_private_child_owners_and_do_not_retry_queries) {
    for (auto operation : {Operation::InspectLocation, Operation::LookupChild, Operation::InspectHandle}) {
        Fixture fixture; fixture.fs.put(fixture.root, name("a"), fixture.fs.make_node(ObjectKind::RegularFile)); auto parent = fixture.cursor();
        fixture.fs.enqueue(operation, {{}, {}, {}, true});
        bool threw = false; try { (void)DirectoryEntryProbe::inspect(parent, name("a")); } catch (const std::runtime_error&) { threw = true; }
        CK_CHECK(threw && fixture.fs.live_entries() == 0 && fixture.fs.live_cursors() == 1);
    }
}
CK_TEST(entry_probe_win32_conventions_and_reported_length_do_not_override_native_lookup) {
    Fixture fixture; LocationInfo location; location.family = LocationQueryFamily::WindowsHandle;
    location.windows_component_utf16_max = Observation<std::uint64_t>::available(3);
    fixture.fs.set_location(fixture.root, Observation<LocationInfo>::available(std::move(location)));
    auto probe = take(DirectoryEntryProbe::inspect(fixture.cursor(), *NativeName::from_windows(u"CON")));
    CK_CHECK(probe->win32_conventions() && probe->win32_conventions()->has(Win32NameIssue::DeviceAliasRisk));
    CK_CHECK(probe->length().value() && !probe->length().value()->exceeds_reported_maximum());
    CK_CHECK(probe->lookup().value() && probe->lookup().value()->state() == ChildLookupState::Missing);
}
CK_TEST(entry_probe_cancellation_between_queries_releases_private_owners_without_partial_records) {
    for (auto operation : {Operation::InspectLocation, Operation::LookupChild, Operation::InspectHandle}) {
        Fixture fixture; fixture.fs.put(fixture.root, name("a"), fixture.fs.make_node(ObjectKind::RegularFile)); auto parent = fixture.cursor();
        std::stop_source stop; fixture.fs.enqueue(operation, {{}, {}, [&] { stop.request_stop(); }, false});
        const auto result = DirectoryEntryProbe::inspect(parent, name("a"), ChildLookupMode::NativeDefault, {}, stop.get_token());
        CK_CHECK(result.state() == ObservationState::Unknown && result.unavailable_detail() && result.unavailable_detail()->error.operation == "entry_probe.cancelled");
        CK_CHECK(fixture.fs.live_entries() == 0 && fixture.fs.live_cursors() == 1);
        const auto calls = fixture.fs.calls().size();
        CK_CHECK(!DirectoryEntryProbe::inspect(parent, name("a"), ChildLookupMode::NativeDefault, {}, stop.get_token()).value());
        CK_CHECK(fixture.fs.calls().size() == calls);
    }
}
