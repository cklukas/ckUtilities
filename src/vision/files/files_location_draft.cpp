// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_location_draft.hpp"
#include "unicode_scalar.hpp"
namespace ck::vision {
namespace {
using namespace ck::files;
template <class T> Observation<T> error(AdapterError code, const char *operation) {
    return Observation<T>::unavailable(code == AdapterError::InventoryLimit ? ObservationState::Unknown
                                                                            : ObservationState::Failed,
                                       {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
Observation<std::string> path_projection(const FilesPaneLocation &location, FilesLocationTextLimits limits) {
    std::string text;
    auto failure = AdapterError::UnrepresentableName;
    const auto append = [&](std::string_view value) {
        if (value.size() > limits.text_bytes - text.size()) {
            failure = AdapterError::InventoryLimit;
            return false;
        }
        text += value;
        return true;
    };
    const auto windows = [&](std::u16string_view units) {
        for (std::size_t at = 0; at < units.size();) {
            const auto scalar = detail::decode_utf16_scalar(units.substr(at));
            if (!scalar.units) {
                failure = AdapterError::UnrepresentableName;
                return false;
            }
            const auto encoded = detail::encode_utf8_scalar(scalar.value);
            if (!append(std::string_view(encoded.data.data(), encoded.units)))
                return false;
            at += scalar.units;
        }
        return true;
    };
    const auto &root = location.root_address();
    if (root.posix_bytes()) {
        if (!append(*root.posix_bytes()))
            return error<std::string>(AdapterError::InventoryLimit, "location_draft.text_bytes");
        for (const auto &name : location.components()) {
            if ((text.back() != '/' && !append("/")) || !append(*name.posix_bytes()))
                return error<std::string>(AdapterError::InventoryLimit, "location_draft.text_bytes");
        }
    } else {
        if (!windows(*root.windows_units()))
            return error<std::string>(failure, "location_draft.utf16_or_text");
        for (const auto &name : location.components()) {
            if ((text.back() != '\\' && text.back() != '/' && !append("\\")) || !windows(*name.windows_units()))
                return error<std::string>(failure, "location_draft.utf16_or_text");
        }
    }
    auto parsed = parse_files_location_text(text, FilesLocationTextSyntax::Path, root.encoding(), limits);
    if (!parsed.value())
        return Observation<std::string>::unavailable(parsed.unavailable_detail()->state,
                                                     parsed.unavailable_detail()->error);
    return Observation<std::string>::available(std::move(text));
}
} // namespace
FilesLocationDraft::FilesLocationDraft(std::optional<FilesPaneLocation> location, ck::files::NameEncoding encoding,
                                       std::string text, FilesLocationTextSyntax syntax, FilesLocationTextLimits limits)
    : original_(std::move(location)), encoding_(encoding), text_(std::move(text)), syntax_(syntax), limits_(limits) {}
ck::files::Observation<FilesLocationDraft>
FilesLocationDraft::make(FilesPaneLocation location, FilesLocationTextSyntax syntax, FilesLocationTextLimits limits) {
    using namespace ck::files;
    if (!limits.path.max_native_bytes || limits.path.max_native_bytes > NativePathLimits::hard_native_bytes ||
        !limits.components || limits.components > FilesPaneLocation::hard_components || !limits.text_bytes ||
        limits.text_bytes > FilesLocationTextLimits::hard_text_bytes)
        return error<FilesLocationDraft>(AdapterError::InvalidLimit, "location_draft.limits");
    if (!location.root_address().is_valid())
        return error<FilesLocationDraft>(AdapterError::InvalidIdentifier, "location_draft.root");
    if (location.native_bytes() > limits.path.max_native_bytes || location.components().size() > limits.components)
        return error<FilesLocationDraft>(AdapterError::InventoryLimit, "location_draft.route_budget");
    if (syntax != FilesLocationTextSyntax::Path && syntax != FilesLocationTextSyntax::ExactNames)
        return error<FilesLocationDraft>(AdapterError::InvalidIdentifier, "location_draft.syntax");
    auto text = syntax == FilesLocationTextSyntax::Path ? path_projection(location, limits)
                                                        : format_files_exact_location(location, limits);
    if (!text.value())
        return Observation<FilesLocationDraft>::unavailable(text.unavailable_detail()->state,
                                                            text.unavailable_detail()->error);
    const auto encoding = location.root_address().encoding();
    return Observation<FilesLocationDraft>::available(
        FilesLocationDraft(std::move(location), encoding, std::move(text).take_value(), syntax, limits));
}
ck::files::Observation<FilesLocationDraft> FilesLocationDraft::new_location(ck::files::NameEncoding encoding,
                                                                            FilesLocationTextLimits limits) {
    using namespace ck::files;
    if (!limits.path.max_native_bytes || limits.path.max_native_bytes > NativePathLimits::hard_native_bytes ||
        !limits.components || limits.components > FilesPaneLocation::hard_components || !limits.text_bytes ||
        limits.text_bytes > FilesLocationTextLimits::hard_text_bytes)
        return error<FilesLocationDraft>(AdapterError::InvalidLimit, "location_draft.limits");
    if (encoding != NameEncoding::PosixBytes && encoding != NameEncoding::WindowsUtf16)
        return error<FilesLocationDraft>(AdapterError::InvalidIdentifier, "location_draft.encoding");
    return Observation<FilesLocationDraft>::available(
        FilesLocationDraft(std::nullopt, encoding, {}, FilesLocationTextSyntax::Path, limits));
}
ck::files::Observation<FilesLocationDraft> FilesLocationDraft::start(FilesPaneLocation location,
                                                                     FilesLocationTextLimits limits) {
    auto plain = make(location, FilesLocationTextSyntax::Path, limits);
    if (plain.value() ||
        plain.unavailable_detail()->error.code == static_cast<std::int64_t>(ck::files::AdapterError::InvalidLimit) ||
        plain.unavailable_detail()->error.code == static_cast<std::int64_t>(ck::files::AdapterError::InventoryLimit))
        return plain;
    return make(std::move(location), FilesLocationTextSyntax::ExactNames, limits);
}
ck::files::Observation<FilesPaneLocation> FilesLocationDraft::resolve(std::string_view text) const {
    if (original_ && !original_->root_address().is_valid())
        return error<FilesPaneLocation>(ck::files::AdapterError::InvalidIdentifier, "location_draft.moved");
    if (original_ && text == text_)
        return ck::files::Observation<FilesPaneLocation>::available(*original_);
    return parse_files_location_text(text, syntax_, encoding_, limits_);
}
ck::files::Observation<FilesLocationDraft> FilesLocationDraft::reformat(std::string_view text,
                                                                        FilesLocationTextSyntax syntax) const {
    auto location = resolve(text);
    if (!location.value())
        return ck::files::Observation<FilesLocationDraft>::unavailable(location.unavailable_detail()->state,
                                                                       location.unavailable_detail()->error);
    return make(std::move(location).take_value(), syntax, limits_);
}
} // namespace ck::vision
