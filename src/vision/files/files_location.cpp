// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_location.hpp"
#include <stdexcept>
namespace ck::vision {
FilesPaneLocation::FilesPaneLocation(ck::files::NativeAbsolutePath root) : root_(std::move(root)), bytes_(0) {
    if (!root_.is_valid())
        throw std::invalid_argument("invalid Files location root");
    bytes_ = root_.posix_bytes() ? root_.posix_bytes()->size() : root_.windows_units()->size() * sizeof(char16_t);
}
ck::files::Observation<FilesPaneLocation>
FilesPaneLocation::from_components(ck::files::NativeAbsolutePath root, std::vector<ck::files::NativeName> components,
                                   ck::files::NativePathLimits limits, std::size_t max_components) {
    using namespace ck::files;
    using Result = Observation<FilesPaneLocation>;
    const auto fail = [](AdapterError code, const char *operation) {
        return Result::unavailable(ObservationState::Failed,
                                   {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
    };
    if (!limits.max_native_bytes || limits.max_native_bytes > NativePathLimits::hard_native_bytes || !max_components ||
        max_components > hard_components)
        return fail(AdapterError::InvalidLimit, "pane_location.limits");
    if (!root.is_valid())
        return fail(AdapterError::InvalidIdentifier, "pane_location.root");
    if (components.size() > max_components)
        return fail(AdapterError::InventoryLimit, "pane_location.depth");
    auto route = FilesPaneLocation(std::move(root));
    const auto width = route.root_.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
    bool separator = route.root_.posix_bytes()
                         ? route.root_.posix_bytes()->back() != '/'
                         : route.root_.windows_units()->back() != u'\\' && route.root_.windows_units()->back() != u'/';
    if (route.bytes_ > limits.max_native_bytes)
        return fail(AdapterError::InventoryLimit, "pane_location.bytes");
    for (const auto &name : components) {
        if (!name.is_valid() || name.encoding() != route.root_.encoding())
            return fail(AdapterError::InvalidIdentifier, "pane_location.encoding_or_name");
        const auto units = name.posix_bytes() ? name.posix_bytes()->size() : name.windows_units()->size();
        const auto extra = separator ? width : 0;
        if (extra > limits.max_native_bytes - route.bytes_ ||
            units > (limits.max_native_bytes - route.bytes_ - extra) / width)
            return fail(AdapterError::InventoryLimit, "pane_location.bytes");
        route.bytes_ += extra + units * width;
        separator = true;
    }
    route.components_ = std::move(components);
    return Result::available(std::move(route));
}
ck::files::Observation<FilesPaneLocation> FilesPaneLocation::descend(const ck::files::NativeName &name,
                                                                     ck::files::NativePathLimits limits,
                                                                     std::size_t max_components) const {
    using namespace ck::files;
    using Result = Observation<FilesPaneLocation>;
    const auto fail = [](AdapterError code, const char *op) {
        return Result::unavailable(ObservationState::Failed,
                                   {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op});
    };
    if (!limits.max_native_bytes || limits.max_native_bytes > NativePathLimits::hard_native_bytes || !max_components ||
        max_components > hard_components)
        return fail(AdapterError::InvalidLimit, "pane_location.limits");
    if (!root_.is_valid() || !name.is_valid() || root_.encoding() != name.encoding())
        return fail(AdapterError::InvalidIdentifier, "pane_location.encoding_or_name");
    const auto width = name.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
    const auto units = name.posix_bytes() ? name.posix_bytes()->size() : name.windows_units()->size();
    const bool separator = !components_.empty() || (root_.posix_bytes() ? root_.posix_bytes()->back() != '/'
                                                                        : root_.windows_units()->back() != u'\\' &&
                                                                              root_.windows_units()->back() != u'/');
    const auto extra = separator ? width : 0;
    if (components_.size() >= max_components || bytes_ > limits.max_native_bytes ||
        extra > limits.max_native_bytes - bytes_ || units > (limits.max_native_bytes - bytes_ - extra) / width)
        return fail(AdapterError::InventoryLimit, "pane_location.bytes_or_depth");
    auto next = *this;
    next.components_.push_back(name);
    next.bytes_ += extra + units * width;
    return Result::available(std::move(next));
}
std::string FilesPaneLocation::display_text() const {
    if (!root_.is_valid())
        return root_.display_text();
    if (root_.posix_bytes()) {
        auto bytes = *root_.posix_bytes();
        for (const auto &name : components_) {
            if (bytes.back() != '/')
                bytes.push_back('/');
            bytes += *name.posix_bytes();
        }
        return ck::files::display_native_text(bytes);
    }
    auto units = *root_.windows_units();
    for (const auto &name : components_) {
        if (units.back() != u'\\' && units.back() != u'/')
            units.push_back(u'\\');
        units += *name.windows_units();
    }
    return ck::files::display_native_text(units);
}
ck::files::Observation<FilesPaneLocation> FilesPaneLocation::parent() const {
    using Result = ck::files::Observation<FilesPaneLocation>;
    if (!root_.is_valid()) {
        const auto error = root_.parent_address();
        return Result::unavailable(error.unavailable_detail()->state, error.unavailable_detail()->error);
    }
    if (components_.empty()) {
        auto parent = root_.parent_address();
        if (!parent.value())
            return Result::unavailable(parent.unavailable_detail()->state, parent.unavailable_detail()->error);
        return Result::available(FilesPaneLocation(std::move(parent).take_value()));
    }
    auto next = *this;
    next.components_.pop_back();
    // Derive the exact budget from the retained route, including the root's
    // existing delimiter. Never infer native byte length from escaped display.
    next.bytes_ = root_.posix_bytes() ? root_.posix_bytes()->size() : root_.windows_units()->size() * sizeof(char16_t);
    const auto width = root_.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
    bool delimiter = root_.posix_bytes()
                         ? root_.posix_bytes()->back() == '/'
                         : root_.windows_units()->back() == u'\\' || root_.windows_units()->back() == u'/';
    for (const auto &name : next.components_) {
        next.bytes_ += (delimiter ? 0 : width) +
                       (name.posix_bytes() ? name.posix_bytes()->size() : name.windows_units()->size() * width);
        delimiter = false;
    }
    return Result::available(std::move(next));
}
ck::files::Observation<FilesPaneLocation> FilesPaneLocation::namespace_root() const {
    using Result = ck::files::Observation<FilesPaneLocation>;
    auto root = root_.namespace_root();
    if (!root.value())
        return Result::unavailable(root.unavailable_detail()->state, root.unavailable_detail()->error);
    return Result::available(FilesPaneLocation(std::move(root).take_value()));
}
} // namespace ck::vision
