// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_name.hpp"
#include "opened_entry.hpp"

namespace ck::files {
// These select native lookup requests, not a Unicode comparison algorithm.
// NativeDefault delegates to the filesystem/system. WindowsIgnoreCase requests
// OBJ_CASE_INSENSITIVE and is Unsupported on POSIX; it is never simulated.
enum class ChildLookupMode { NativeDefault, WindowsIgnoreCase };
constexpr bool valid_child_lookup_mode(ChildLookupMode mode) noexcept {
    return mode==ChildLookupMode::NativeDefault||mode==ChildLookupMode::WindowsIgnoreCase;
}
enum class ChildLookupState { Unknown, Found, Missing };
struct ChildLookupContext {
    NativeName requested;
    ChildLookupMode mode;
    Observation<ObjectIdentity> directory;
};
// A momentary, read-only observation through a held directory. Found owns the
// opened final object without following links; Missing retains native evidence.
// Neither reserves a name, grants write access nor proves a future create will
// succeed. Callers must retain/revalidate their parent and use exclusive native
// creation at execution. Context IDs alone are not parent-handle authority.
class ChildLookup final {
public:
    ChildLookup() = default;
    ChildLookup(const ChildLookup&) = delete;
    ChildLookup& operator=(const ChildLookup&) = delete;
    ChildLookup(ChildLookup&& other) noexcept;
    ChildLookup& operator=(ChildLookup&& other) noexcept;
    static ChildLookup found(ChildLookupContext context, std::unique_ptr<OpenedEntry> entry);
    static ChildLookup missing(ChildLookupContext context, NativeError witness);
    ChildLookupState state() const noexcept;
    const ChildLookupContext* context() const noexcept { return context_ ? &*context_ : nullptr; }
    const OpenedEntry* entry() const noexcept;
    const NativeError* missing_witness() const noexcept;
private:
    using Payload = std::variant<std::monostate,std::unique_ptr<OpenedEntry>,NativeError>;
    ChildLookup(ChildLookupContext context, Payload payload);
    std::optional<ChildLookupContext> context_;
    Payload payload_;
};
namespace detail {
enum class ChildLookupFamily { PosixAt, WindowsNt };
// Qualifies only the definitive per-component errors from these native APIs.
// A generic Gone state or path-not-found is deliberately insufficient.
bool definite_child_absence(const UnavailableObservation& error, ChildLookupFamily family) noexcept;
}
} // namespace ck::files
