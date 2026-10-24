// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_filesystem.hpp"
#include "manual_monotonic_clock.hpp"
#include "simulated_inventory.hpp"
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace ck::testing {
// Single-owner deterministic discovery model, with no host filesystem access.
// Names use exact native units; no case/normalization, symlink traversal, native
// error-code emulation or preservation-capability claim is made. A cursor takes
// a name sequence at open, while child opens resolve live membership. Handles
// retain objects independently of path rebinding/removal. Inventory replies are
// explicit fixtures; there is no mutation API or native capability emulation.
class SimulatedFileSystem final : public ck::files::BasicFileSystem {
    using Metadata = ck::files::BasicMetadata;
    using Name = ck::files::NativeName;
    using Kind = ck::files::ObjectKind;
    template<class T> using Observed = ck::files::Observation<T>;
public:
    enum class Operation { OpenDirectory, InspectPath, ReadDirectory, OpenChild, OpenDirectoryChild, InspectHandle, LookupChild, InspectLocation,
        InspectStorage, ReadLink, InspectChannels, InspectAttributes, ReadAttribute, ReadSecurity, InspectLayout, OpenParent };
    struct Action {
        std::optional<ck::files::UnavailableObservation> failure;
        ck::files::MonotonicClock::Duration elapsed{};
        std::function<void()> effect;
        bool throws = false;
    };
    struct Call {
        Operation operation; std::uint64_t object; std::optional<Name> name;
        std::optional<ck::files::NativeFeatureIdentifier> feature;
        std::optional<ck::files::SecurityPart> security;
        std::optional<ck::files::InventoryLimits> limits;
        ck::files::EntryAccess access = ck::files::EntryAccess::MetadataOnly;
    };
    class Node;
    using Pointer = std::shared_ptr<Node>;
    class Node final {
        friend class SimulatedFileSystem;
        Node(ck::files::AdapterScope owner, std::uint64_t id, Kind kind)
            : owner_(std::move(owner)), id_(id), kind_(kind) {}
        ck::files::AdapterScope owner_;
        std::uint64_t id_;
        Kind kind_;
        Metadata metadata_;
        std::optional<Observed<ck::files::LocationInfo>> location_;
        SimulatedInventory inventory_;
        std::vector<std::pair<Name, Pointer>> children_;
        std::weak_ptr<Node> parent_;
        bool directory_root_ = false, parent_ambiguous_ = false;
    public:
        std::uint64_t id() const noexcept { return id_; }
    };
    static constexpr std::size_t max_nodes = 4096, max_calls = 100'000, max_actions = 4096;
    SimulatedFileSystem() : state_(std::make_shared<State>()) {}
    SimulatedFileSystem(const SimulatedFileSystem&) = delete;
    SimulatedFileSystem& operator=(const SimulatedFileSystem&) = delete;
    Pointer make_node(Kind kind) {
        if (state_->nodes == max_nodes) throw std::length_error("simulated node limit");
        auto node = Pointer(new Node(state_->scope, ++state_->nodes, kind));
        node->metadata_.kind = Observed<Kind>::available(kind);
        node->metadata_.identity = Observed<ck::files::ObjectIdentity>::available({state_->scope,
            ck::files::IdentityScheme::PosixDeviceInode, ck::files::NativeIdentifier::from_u64(1),
            ck::files::NativeIdentifier::from_u64(node->id_), {}});
        return node;
    }
    Metadata& metadata(const Pointer& node) { require(node); return node->metadata_; }
    void bind(const std::filesystem::path& absolute, Pointer node) {
        require(node); if (!valid_path(absolute)) throw std::invalid_argument("invalid simulated path");
        if (!state_->paths.contains(absolute) && state_->paths.size() == max_nodes) throw std::length_error("simulated path limit");
        if (node->kind_ == Kind::Directory && node->parent_.expired()) node->directory_root_ = true;
        state_->paths.insert_or_assign(absolute, std::move(node));
    }
    void set_location(const Pointer& node, Observed<ck::files::LocationInfo> location) {
        require(node); node->location_ = std::move(location);
    }
    void set_inventory(const Pointer& node, SimulatedInventory inventory) {
        require(node); inventory.validate(); node->inventory_ = std::move(inventory);
    }
    void unbind(const std::filesystem::path& absolute) { state_->paths.erase(absolute); }
    void put(const Pointer& parent, Name name, Pointer child) {
        require(parent); require(child);
        if (parent->kind_ != Kind::Directory || !name.is_valid()) throw std::invalid_argument("invalid simulated child binding");
        if (child->kind_ == Kind::Directory && contains(child, parent)) throw std::invalid_argument("simulated directory cycle");
        if (child->kind_ == Kind::Directory) {
            if (const auto old = child->parent_.lock(); old && old != parent) child->parent_ambiguous_ = true;
            child->parent_ = parent; child->directory_root_ = false;
        }
        for (auto& entry : parent->children_) if (entry.first == name) {
            if (entry.second != child) remove_parent_link(parent, entry.second, name);
            entry.second = std::move(child); return;
        }
        if (parent->children_.size() == max_nodes) throw std::length_error("simulated directory limit");
        parent->children_.emplace_back(std::move(name), std::move(child));
    }
    bool erase(const Pointer& parent, const Name& name) {
        require(parent);
        for (auto it = parent->children_.begin(); it != parent->children_.end(); ++it)
            if (it->first == name) {
            remove_parent_link(parent, it->second, name);
            parent->children_.erase(it); return true; }
        return false;
    }
    void enqueue(Operation operation, Action action) {
        if (action.failure) (void)Observed<int>::unavailable(action.failure->state, action.failure->error);
        if (state_->queued == max_actions) throw std::length_error("simulated action limit");
        state_->actions[operation].push_back(std::move(action)); ++state_->queued;
    }
    std::shared_ptr<ManualMonotonicClock> clock() const { return state_->clock; }
    const std::vector<Call>& calls() const noexcept { return state_->calls; }
    std::size_t live_cursors() const noexcept { return state_->cursors; }
    std::size_t live_entries() const noexcept { return state_->entries; }
    Observed<std::unique_ptr<ck::files::DirectoryCursor>> open_directory(const std::filesystem::path& path,
        ck::files::DirectoryLinkPolicy policy = ck::files::DirectoryLinkPolicy::RejectFinalLink) const override {
        using Result = Observed<std::unique_ptr<ck::files::DirectoryCursor>>;
        if (!valid_path(path)) return failed<Result>("simulated.path");
        if (policy != ck::files::DirectoryLinkPolicy::RejectFinalLink && policy != ck::files::DirectoryLinkPolicy::FollowFinalLink)
            return failed<Result>("simulated.policy");
        const auto fault = step(state_, Operation::OpenDirectory, nullptr);
        if (fault) return Result::unavailable(fault->state, fault->error);
        const auto found = state_->paths.find(path);
        if (found == state_->paths.end()) return missing<Result>();
        if (found->second->kind_ != Kind::Directory) return failed<Result>("simulated.not_directory");
        return Result::available(std::make_unique<Cursor>(state_, found->second));
    }
    Observed<Metadata> inspect_basic(const std::filesystem::path& path) const override {
        using Result = Observed<Metadata>;
        if (!valid_path(path)) return failed<Result>("simulated.path");
        const auto fault = step(state_, Operation::InspectPath, nullptr);
        if (fault) return Result::unavailable(fault->state, fault->error);
        const auto found = state_->paths.find(path);
        return found == state_->paths.end() ? missing<Result>() : Result::available(found->second->metadata_);
    }
private:
    struct State {
        ck::files::AdapterScope scope;
        std::uint64_t nodes = 0;
        std::size_t cursors = 0, entries = 0, queued = 0;
        std::map<std::filesystem::path, Pointer> paths;
        std::map<Operation, std::deque<Action>> actions;
        std::vector<Call> calls;
        std::shared_ptr<ManualMonotonicClock> clock = std::make_shared<ManualMonotonicClock>();
    };
    template<class Result> static Result failed(const char* operation) {
        return Result::unavailable(ck::files::ObservationState::Failed,
            {ck::files::ErrorDomain::Adapter, 1, operation});
    }
    template<class Result> static Result missing() {
        return Result::unavailable(ck::files::ObservationState::Gone,
            {ck::files::ErrorDomain::Adapter, 2, "simulated.missing"});
    }
    static bool valid_path(const std::filesystem::path& path) {
        return path.is_absolute() && path.native().find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos;
    }
    void require(const Pointer& node) const {
        if (!node || node->owner_ != state_->scope) throw std::invalid_argument("foreign simulated node");
    }
    static void remove_parent_link(const Pointer& parent, const Pointer& child, const Name& removed) {
        if (child->kind_ != Kind::Directory || child->parent_.lock() != parent) return;
        for (const auto& entry : parent->children_) if (entry.first != removed && entry.second == child) return;
        child->parent_.reset(); child->directory_root_ = false;
    }
    static bool contains(const Pointer& root, const Pointer& target) {
        std::vector<Pointer> pending{root};
        std::set<std::uint64_t> visited;
        while (!pending.empty()) {
            auto current = std::move(pending.back()); pending.pop_back();
            if (current == target) return true;
            if (!visited.insert(current->id_).second) continue;
            for (const auto& entry : current->children_)
                if (entry.second->kind_ == Kind::Directory && !visited.contains(entry.second->id_)) pending.push_back(entry.second);
        }
        return false;
    }
    static std::optional<ck::files::UnavailableObservation> step(const std::shared_ptr<State>& state,
        Operation operation, const Pointer& node, std::optional<Name> name = {},
        std::optional<ck::files::NativeFeatureIdentifier> feature = {},
        std::optional<ck::files::SecurityPart> security = {}, std::optional<ck::files::InventoryLimits> limits = {},
        ck::files::EntryAccess access = ck::files::EntryAccess::MetadataOnly) {
        if (state->calls.size() == max_calls) throw std::length_error("simulated call limit");
        state->calls.push_back({operation, node ? node->id_ : 0, std::move(name), std::move(feature), security, limits, access});
        auto& queue = state->actions[operation];
        if (queue.empty()) return {};
        auto action = std::move(queue.front()); queue.pop_front(); --state->queued;
        state->clock->advance(action.elapsed);
        if (action.effect) action.effect();
        if (action.throws) throw std::runtime_error("injected simulated filesystem exception");
        return action.failure;
    }
    class Entry final : public ck::files::OpenedEntry {
    public:
        Entry(std::shared_ptr<State> state, Pointer node, ck::files::EntryAccess access = ck::files::EntryAccess::MetadataOnly)
            : state_(std::move(state)), node_(std::move(node)), access_(access) { ++state_->entries; }
        ~Entry() override { --state_->entries; }
        Observed<std::unique_ptr<ck::files::OpenedEntry>> open_parent_directory() const override { return parent_entry(state_, node_); }
        Observed<Metadata> inspect_basic() const override {
            const auto fault = step(state_, Operation::InspectHandle, node_);
            return fault ? Observed<Metadata>::unavailable(fault->state, fault->error) : Observed<Metadata>::available(node_->metadata_);
        }
        Observed<ck::files::StorageInfo> inspect_storage() const override {
            const auto fault = step(state_, Operation::InspectStorage, node_, {}, {}, {}, {}, access_);
            if (fault) return Observed<ck::files::StorageInfo>::unavailable(fault->state, fault->error);
            return node_->inventory_.storage ? *node_->inventory_.storage : OpenedEntry::inspect_storage();
        }
        Observed<ck::files::FileDataLayout> inspect_data_layout(ck::files::InventoryLimits limits = {}, std::stop_token stop = {}) const override {
            using Result = Observed<ck::files::FileDataLayout>;
            const auto cancelled = [] { return Result::unavailable(ck::files::ObservationState::Unknown,
                {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead), "layout.cancelled"}); };
            if (!limits.valid()) return invalid_limits<Result>();
            if (stop.stop_requested()) return cancelled();
            const auto fault = step(state_, Operation::InspectLayout, node_, {}, {}, {}, limits, access_);
            if (stop.stop_requested()) return cancelled();
            if (fault) return Result::unavailable(fault->state, fault->error);
            if (!ck::files::requests_layout(access_)) return access_denied<Result>();
            return node_->inventory_.layout ? bounded(*node_->inventory_.layout, limits) : unsupported<Result>();
        }
        Observed<ck::files::LinkMetadata> read_link_metadata(ck::files::InventoryLimits limits = {}) const override {
            return query(Operation::ReadLink, node_->inventory_.link, limits);
        }
        Observed<ck::files::DataChannelCatalog> inspect_data_channels(ck::files::InventoryLimits limits = {}) const override {
            return query(Operation::InspectChannels, node_->inventory_.channels, limits);
        }
        Observed<ck::files::AttributeCatalog> inspect_attributes(ck::files::InventoryLimits limits = {}) const override {
            return query(Operation::InspectAttributes, node_->inventory_.attributes, limits, {}, {}, ck::files::requests_attributes(access_));
        }
        Observed<ck::files::AttributeValue> read_attribute(const ck::files::NativeFeatureIdentifier& key,
            ck::files::InventoryLimits limits = {}) const override {
            // Capture lookup after the injected effect, not before it; effects
            // may replace the entire inventory and invalidate map references.
            return attribute_query(key, limits);
        }
        Observed<ck::files::NativeSecurityValue> read_security(ck::files::SecurityPart part,
            ck::files::InventoryLimits limits = {}) const override {
            using Result = Observed<ck::files::NativeSecurityValue>;
            if (!ck::files::valid_security_part(part)) return rejected<Result>(ck::files::ObservationState::Failed,
                ck::files::AdapterError::InvalidIdentifier, "simulated.security_part");
            if (!limits.valid()) return invalid_limits<Result>();
            const auto fault = step(state_, Operation::ReadSecurity, node_, {}, {}, part, limits, access_);
            if (fault) return Result::unavailable(fault->state, fault->error);
            if (!ck::files::requests_security(access_) || (part == ck::files::SecurityPart::WindowsFullDescriptor && !ck::files::requests_audit_security(access_)))
                return access_denied<Result>();
            const auto found = node_->inventory_.security.find(part);
            return found == node_->inventory_.security.end() ? unsupported<Result>() : bounded(found->second, limits);
        }
    private:
        template<class Result> static Result rejected(ck::files::ObservationState state, ck::files::AdapterError error, const char* operation) {
            return Result::unavailable(state, {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(error), operation});
        }
        template<class Result> static Result invalid_limits() { return rejected<Result>(ck::files::ObservationState::Failed,
            ck::files::AdapterError::InvalidLimit, "simulated.inventory_limits"); }
        template<class Result> static Result access_denied() { return Result::unavailable(ck::files::ObservationState::Denied,
            {ck::files::ErrorDomain::Adapter, 53, "simulated.access_not_requested"}); }
        template<class Result> static Result unsupported() { return rejected<Result>(ck::files::ObservationState::Unsupported,
            ck::files::AdapterError::UnsupportedQuery, "simulated.inventory_unconfigured"); }
        template<class T> static Observed<T> bounded(const Observed<T>& reply, ck::files::InventoryLimits limits) {
            if (reply.value() && !SimulatedInventory::fits(*reply.value(), limits)) return rejected<Observed<T>>(ck::files::ObservationState::Failed,
                ck::files::AdapterError::InventoryLimit, "simulated.inventory_limit");
            return reply;
        }
        template<class T> Observed<T> query(Operation operation, const std::optional<Observed<T>>& reply,
            ck::files::InventoryLimits limits, std::optional<ck::files::NativeFeatureIdentifier> key = {},
            std::optional<ck::files::SecurityPart> part = {}, bool permitted = true) const {
            using Result = Observed<T>;
            if (!limits.valid()) return invalid_limits<Result>();
            const auto fault = step(state_, operation, node_, {}, std::move(key), part, limits, access_);
            if (fault) return Result::unavailable(fault->state, fault->error);
            if (!permitted) return access_denied<Result>();
            return reply ? bounded(*reply, limits) : unsupported<Result>();
        }
        Observed<ck::files::AttributeValue> attribute_query(const ck::files::NativeFeatureIdentifier& key, ck::files::InventoryLimits limits) const {
            using Result = Observed<ck::files::AttributeValue>;
            if (!limits.valid()) return invalid_limits<Result>();
            if (SimulatedInventory::bytes(key) > ck::files::InventoryLimits::hard_native_bytes)
                return rejected<Result>(ck::files::ObservationState::Failed, ck::files::AdapterError::InventoryLimit, "simulated.feature_limit");
            const auto fault = step(state_, Operation::ReadAttribute, node_, {}, key, {}, limits, access_);
            if (fault) return Result::unavailable(fault->state, fault->error);
            if (!ck::files::requests_attributes(access_)) return access_denied<Result>();
            const auto found = node_->inventory_.values.find(key);
            return found == node_->inventory_.values.end() ? unsupported<Result>() : bounded(found->second, limits);
        }
        std::shared_ptr<State> state_; Pointer node_; ck::files::EntryAccess access_;
    };
    static Observed<std::unique_ptr<ck::files::OpenedEntry>> parent_entry(const std::shared_ptr<State>& state, const Pointer& node) {
        using Result = Observed<std::unique_ptr<ck::files::OpenedEntry>>;
        if (const auto fault = step(state, Operation::OpenParent, node)) return Result::unavailable(fault->state, fault->error);
        if (node->kind_ != Kind::Directory) return Result::unavailable(ck::files::ObservationState::Unsupported,
            {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(ck::files::AdapterError::NotApplicable), "simulated.parent_kind"});
        if (node->parent_ambiguous_) return Result::unavailable(ck::files::ObservationState::Unknown,
            {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(ck::files::AdapterError::MetadataChanged), "simulated.parent_ambiguous"});
        auto parent = node->directory_root_ ? node : node->parent_.lock();
        if (!parent) return missing<Result>();
        return Result::available(std::make_unique<Entry>(state, std::move(parent)));
    }
    class Cursor final : public ck::files::DirectoryCursor {
    public:
        Cursor(std::shared_ptr<State> state, Pointer node)
            : DirectoryCursor(node->metadata_.identity), state_(std::move(state)), node_(std::move(node)) {
            for (const auto& entry : node_->children_) names_.push_back(entry.first);
            ++state_->cursors;
        }
        ~Cursor() override { --state_->cursors; }
        Observed<Metadata> inspect_basic() const override {
            const auto fault = step(state_, Operation::InspectHandle, node_);
            if (fault) return Observed<Metadata>::unavailable(fault->state, fault->error);
            return Observed<Metadata>::available(node_->metadata_);
        }
        Observed<std::unique_ptr<ck::files::OpenedEntry>> open_parent_directory() const override { return parent_entry(state_, node_); }
        Observed<std::unique_ptr<ck::files::OpenedEntry>> open_child(const Name& name,
            ck::files::EntryAccess access = ck::files::EntryAccess::MetadataOnly) const override {
            using Result = Observed<std::unique_ptr<ck::files::OpenedEntry>>;
            if (!name.is_valid() || !ck::files::valid_entry_access(access)) return failed<Result>("simulated.child_access");
            const auto fault = step(state_, Operation::OpenChild, node_, name, {}, {}, {}, access);
            if (fault) return Result::unavailable(fault->state, fault->error);
            auto child = find(name);
            return child ? Result::available(std::make_unique<Entry>(state_, std::move(child), access)) : missing<Result>();
        }
        Observed<ck::files::LocationInfo> inspect_location() const override {
            using Result = Observed<ck::files::LocationInfo>;
            const auto fault = step(state_, Operation::InspectLocation, node_);
            if (fault) return Result::unavailable(fault->state, fault->error);
            if (!node_->location_) return DirectoryCursor::inspect_location();
            return *node_->location_;
        }
        Observed<ck::files::ChildLookup> lookup_child(const Name& name, ck::files::ChildLookupMode mode) const override {
            using Result = Observed<ck::files::ChildLookup>;
            if (!name.is_valid() || !ck::files::valid_child_lookup_mode(mode)) return failed<Result>("simulated.lookup_input");
            if (mode != ck::files::ChildLookupMode::NativeDefault)
                return Result::unavailable(ck::files::ObservationState::Unsupported,
                    {ck::files::ErrorDomain::Adapter, 3, "simulated.lookup_mode"});
            const auto fault = step(state_, Operation::LookupChild, node_, name);
            if (fault) return Result::unavailable(fault->state, fault->error);
            auto child = find(name);
            ck::files::ChildLookupContext context{name, mode, identity()};
            if (child) return Result::available(ck::files::ChildLookup::found(std::move(context), std::make_unique<Entry>(state_, std::move(child))));
            return Result::available(ck::files::ChildLookup::missing(std::move(context),
                {ck::files::ErrorDomain::Adapter, 2, "simulated.missing"}));
        }
        Observed<std::unique_ptr<ck::files::DirectoryCursor>> open_directory_child(const Name& name) const override {
            using Result = Observed<std::unique_ptr<ck::files::DirectoryCursor>>;
            if (!name.is_valid()) return failed<Result>("simulated.name");
            const auto fault = step(state_, Operation::OpenDirectoryChild, node_, name);
            if (fault) return Result::unavailable(fault->state, fault->error);
            auto child = find(name);
            if (!child) return missing<Result>();
            if (child->kind_ != Kind::Directory) return failed<Result>("simulated.not_directory");
            return Result::available(std::make_unique<Cursor>(state_, std::move(child)));
        }
    private:
        Pointer find(const Name& name) const {
            for (const auto& entry : node_->children_) if (entry.first == name) return entry.second;
            return {};
        }
        Observed<ck::files::DirectoryStep> next_step() override {
            using Result = Observed<ck::files::DirectoryStep>;
            const auto fault = step(state_, Operation::ReadDirectory, node_);
            if (fault) return Result::unavailable(fault->state, fault->error);
            if (at_ == names_.size()) return Result::available(ck::files::DirectoryEnd{});
            return Result::available(names_[at_++]);
        }
        std::shared_ptr<State> state_; Pointer node_; std::vector<Name> names_; std::size_t at_ = 0;
    };
    std::shared_ptr<State> state_;
};
} // namespace ck::testing
