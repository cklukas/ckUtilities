// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_location_text.hpp"
#include "unicode_scalar.hpp"
#include "windows_name.hpp"
#include <utility>
namespace ck::vision {
namespace {
using namespace ck::files;
struct Rejected {
    UnavailableObservation detail;
};
[[noreturn]] void reject(AdapterError code, const char *operation) {
    throw Rejected{{code == AdapterError::InventoryLimit ? ObservationState::Unknown : ObservationState::Failed,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}}};
}
void validate(FilesLocationTextLimits limits) {
    if (!limits.path.max_native_bytes || limits.path.max_native_bytes > NativePathLimits::hard_native_bytes ||
        !limits.components || limits.components > FilesPaneLocation::hard_components || !limits.text_bytes ||
        limits.text_bytes > FilesLocationTextLimits::hard_text_bytes)
        reject(AdapterError::InvalidLimit, "location_text.limits");
}
bool safe_scalar(std::uint32_t value) {
    return value >= 0x20 && !(value >= 0x7F && value <= 0x9F) && value != 0x061C && value != 0x200E &&
           value != 0x200F && !(value >= 0x2028 && value <= 0x202E) && !(value >= 0x2066 && value <= 0x2069) &&
           value != 0xFEFF && !(value >= 0xFDD0 && value <= 0xFDEF) && (value & 0xFFFFU) != 0xFFFE &&
           (value & 0xFFFFU) != 0xFFFF;
}
int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
struct NativeText {
    NameEncoding encoding;
    NativePathLimits limits;
    std::string bytes;
    std::u16string units;
    void byte(unsigned char value) {
        if (encoding != NameEncoding::PosixBytes)
            reject(AdapterError::InvalidPath, "location_text.byte_escape_encoding");
        if (bytes.size() == limits.max_native_bytes)
            reject(AdapterError::InventoryLimit, "location_text.native_bytes");
        bytes.push_back(static_cast<char>(value));
    }
    void unit(char16_t value) {
        if (units.size() == limits.max_native_bytes / sizeof(char16_t))
            reject(AdapterError::InventoryLimit, "location_text.native_bytes");
        units.push_back(value);
    }
    void scalar(std::uint32_t value, bool escaped = false) {
        if (encoding == NameEncoding::PosixBytes) {
            const auto encoded = detail::encode_utf8_scalar(value);
            if (!encoded.units)
                reject(AdapterError::InvalidPath, "location_text.scalar");
            for (std::size_t i = 0; i < encoded.units; ++i)
                byte(static_cast<unsigned char>(encoded.data[i]));
        } else if (escaped && value >= 0xD800 && value <= 0xDFFF) {
            unit(static_cast<char16_t>(value));
        } else {
            const auto encoded = detail::encode_utf16_scalar(value);
            if (!encoded.units)
                reject(AdapterError::InvalidPath, "location_text.scalar");
            for (std::size_t i = 0; i < encoded.units; ++i)
                unit(encoded.data[i]);
        }
    }
    Observation<NativeAbsolutePath> address() && {
        return encoding == NameEncoding::PosixBytes ? NativeAbsolutePath::from_posix(bytes, limits)
                                                    : NativeAbsolutePath::from_windows(units, limits);
    }
    std::optional<NativeName> name() && {
        return encoding == NameEncoding::PosixBytes ? NativeName::from_posix(std::move(bytes))
                                                    : NativeName::from_windows(std::move(units));
    }
};
class Reader {
    std::string_view text_;
    std::size_t at_ = 0;
    NameEncoding encoding_;

  public:
    Reader(std::string_view text, NameEncoding encoding) : text_(text), encoding_(encoding) {}
    void space() {
        while (at_ < text_.size() && text_[at_] == ' ')
            ++at_;
    }
    bool consume(char c) {
        if (at_ == text_.size() || text_[at_] != c)
            return false;
        ++at_;
        return true;
    }
    void expect(char c) {
        if (!consume(c))
            reject(AdapterError::InvalidPath, "location_text.syntax");
    }
    bool end() const noexcept { return at_ == text_.size(); }
    std::size_t position() const noexcept { return at_; }
    std::uint32_t hex_value(std::size_t count) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (at_ == text_.size() || hex(text_[at_]) < 0)
                reject(AdapterError::InvalidPath, "location_text.hex");
            value = value * 16 + static_cast<unsigned>(hex(text_[at_++]));
        }
        return value;
    }
    NativeText quoted(NativePathLimits budget, bool allow_open = false, bool *closed = nullptr) {
        expect('"');
        NativeText value{encoding_, budget, {}, {}};
        for (;;) {
            if (consume('"')) {
                if (closed)
                    *closed = true;
                return value;
            }
            if (end()) {
                if (allow_open) {
                    if (closed)
                        *closed = false;
                    return value;
                }
                reject(AdapterError::InvalidPath, "location_text.quote");
            }
            if (consume('\\')) {
                if (end())
                    reject(AdapterError::InvalidPath, "location_text.escape");
                const auto c = text_[at_++];
                if (c == '\\' || c == '"' || c == '/')
                    value.scalar(static_cast<unsigned char>(c));
                else if (c == 'x')
                    value.byte(static_cast<unsigned char>(hex_value(2)));
                else if (c == 'u') {
                    expect('{');
                    const auto start = at_;
                    while (at_ < text_.size() && hex(text_[at_]) >= 0 && at_ - start < 6)
                        ++at_;
                    const auto digits = at_ - start;
                    if (!digits || at_ == text_.size() || text_[at_] != '}')
                        reject(AdapterError::InvalidPath, "location_text.scalar_escape");
                    at_ = start;
                    const auto scalar = hex_value(digits);
                    expect('}');
                    value.scalar(scalar, true);
                } else if (c == 'n')
                    value.scalar('\n');
                else if (c == 'r')
                    value.scalar('\r');
                else if (c == 't')
                    value.scalar('\t');
                else if (c == 'b')
                    value.scalar('\b');
                else if (c == 'f')
                    value.scalar('\f');
                else
                    reject(AdapterError::InvalidPath, "location_text.escape");
            } else {
                const auto decoded = detail::decode_utf8_scalar(text_.substr(at_));
                if (!decoded.units || !safe_scalar(decoded.value))
                    reject(AdapterError::InvalidPath, "location_text.utf8");
                at_ += decoded.units;
                value.scalar(decoded.value);
            }
        }
    }
};
// No automatic Win32 trimming/device interpretation in an ordinary editor path.
void ordinary_windows_path(const NativeAbsolutePath &path) {
    const auto &text = *path.windows_units();
    if (text.starts_with(u"\\\\?\\"))
        return; // Explicit extended filesystem namespace.
    const auto root = path.namespace_root();
    if (!root.value())
        reject(AdapterError::InvalidPath, "location_text.windows_root");
    auto start = root.value()->windows_units()->size();
    if (text.size() >= 2 && (text[0] == u'\\' || text[0] == u'/') && (text[1] == u'\\' || text[1] == u'/')) {
        auto end = text.find_first_of(u"\\/", 2);
        for (auto begin : {std::size_t{2}, end + 1}) {
            auto last = text.find_first_of(u"\\/", begin);
            if (last == text.npos)
                last = text.size();
            if (last > begin && (text[last - 1] == u' ' || text[last - 1] == u'.'))
                reject(AdapterError::UnrepresentableName, "location_text.windows_namespace_spelling");
        }
    }
    while (start < text.size()) {
        auto end = text.find_first_of(u"\\/", start);
        if (end == text.npos)
            end = text.size();
        const auto name = std::u16string_view(text).substr(start, end - start);
        if (!name.empty() && name != u"." && name != u".." && !check_win32_component(name).ordinary_component())
            reject(AdapterError::UnrepresentableName, "location_text.windows_component_spelling");
        start = end == text.size() ? end : end + 1;
    }
}
class Writer {
    std::string text_;
    std::size_t limit_;

  public:
    explicit Writer(std::size_t limit) : limit_(limit) {}
    void add(std::string_view value) {
        if (value.size() > limit_ - text_.size())
            reject(AdapterError::InventoryLimit, "location_text.text_bytes");
        text_ += value;
    }
    void scalar(std::uint32_t value, bool byte = false) {
        if (!byte && value >= 0x20 && value <= 0x7E) {
            const auto c = static_cast<char>(value);
            if (c == '\\' || c == '"')
                add("\\");
            add(std::string_view(&c, 1));
            return;
        }
        constexpr std::string_view digits = "0123456789ABCDEF";
        const auto count = byte ? 2 : value > 0xFFFFF ? 6 : value > 0xFFFF ? 5 : 4;
        add(byte ? "\\x" : "\\u{");
        for (int i = count - 1; i >= 0; --i) {
            const char c = digits[(value >> (static_cast<unsigned>(i) * 4)) & 15];
            add(std::string_view(&c, 1));
        }
        if (!byte)
            add("}");
    }
    void quoted(std::string_view bytes) {
        add("\"");
        for (std::size_t at = 0; at < bytes.size();) {
            const auto decoded = detail::decode_utf8_scalar(bytes.substr(at));
            if (!decoded.units) {
                scalar(static_cast<unsigned char>(bytes[at++]), true);
            } else {
                scalar(decoded.value);
                at += decoded.units;
            }
        }
        add("\"");
    }
    void quoted(std::u16string_view units) {
        add("\"");
        for (std::size_t at = 0; at < units.size();) {
            const auto decoded = detail::decode_utf16_scalar(units.substr(at));
            if (!decoded.units)
                scalar(units[at++]);
            else {
                scalar(decoded.value);
                at += decoded.units;
            }
        }
        add("\"");
    }
    std::string take() && { return std::move(text_); }
};
} // namespace
ck::files::Observation<FilesPaneLocation> parse_files_location_text(std::string_view text,
                                                                    FilesLocationTextSyntax syntax,
                                                                    ck::files::NameEncoding encoding,
                                                                    FilesLocationTextLimits limits) {
    using Result = ck::files::Observation<FilesPaneLocation>;
    try {
        validate(limits);
        if (encoding != NameEncoding::PosixBytes && encoding != NameEncoding::WindowsUtf16)
            reject(AdapterError::InvalidIdentifier, "location_text.encoding");
        if (text.size() > limits.text_bytes)
            reject(AdapterError::InventoryLimit, "location_text.text_bytes");
        if (syntax == FilesLocationTextSyntax::Path) {
            NativeText value{encoding, limits.path, {}, {}};
            for (std::size_t at = 0; at < text.size();) {
                const auto decoded = detail::decode_utf8_scalar(text.substr(at));
                if (!decoded.units || !safe_scalar(decoded.value))
                    reject(AdapterError::InvalidPath, "location_text.utf8");
                value.scalar(decoded.value);
                at += decoded.units;
            }
            auto path = std::move(value).address();
            if (!path.value())
                return Result::unavailable(path.unavailable_detail()->state, path.unavailable_detail()->error);
            if (encoding == NameEncoding::WindowsUtf16)
                ordinary_windows_path(*path.value());
            return Result::available(FilesPaneLocation(std::move(path).take_value()));
        }
        if (syntax != FilesLocationTextSyntax::ExactNames)
            reject(AdapterError::InvalidIdentifier, "location_text.syntax_kind");
        Reader reader(text, encoding);
        reader.space();
        reader.expect('[');
        reader.space();
        auto root = std::move(reader.quoted(limits.path)).address();
        if (!root.value())
            return Result::unavailable(root.unavailable_detail()->state, root.unavailable_detail()->error);
        std::vector<NativeName> names;
        const auto &address = *root.value();
        const auto width = address.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
        auto used = address.posix_bytes() ? address.posix_bytes()->size() : address.windows_units()->size() * width;
        bool separator = address.posix_bytes()
                             ? address.posix_bytes()->back() != '/'
                             : address.windows_units()->back() != u'\\' && address.windows_units()->back() != u'/';
        reader.space();
        while (reader.consume(',')) {
            if (names.size() == limits.components)
                reject(AdapterError::InventoryLimit, "location_text.depth");
            const auto extra = separator ? width : 0;
            if (extra > limits.path.max_native_bytes - used)
                reject(AdapterError::InventoryLimit, "location_text.native_bytes");
            reader.space();
            auto name = std::move(reader.quoted({limits.path.max_native_bytes - used - extra})).name();
            if (!name)
                reject(AdapterError::InvalidIdentifier, "location_text.component");
            used += extra + (name->posix_bytes() ? name->posix_bytes()->size() : name->windows_units()->size() * width);
            separator = true;
            names.push_back(std::move(*name));
            reader.space();
        }
        reader.expect(']');
        reader.space();
        if (!reader.end())
            reject(AdapterError::InvalidPath, "location_text.trailing_text");
        return FilesPaneLocation::from_components(std::move(root).take_value(), std::move(names), limits.path,
                                                  limits.components);
    } catch (const Rejected &error) {
        return Result::unavailable(error.detail.state, error.detail.error);
    }
}
ck::files::Observation<std::string> format_files_exact_location(const FilesPaneLocation &location,
                                                                FilesLocationTextLimits limits) {
    using Result = ck::files::Observation<std::string>;
    try {
        validate(limits);
        if (!location.root_address().is_valid())
            reject(AdapterError::InvalidIdentifier, "location_text.root");
        if (location.native_bytes() > limits.path.max_native_bytes || location.components().size() > limits.components)
            reject(AdapterError::InventoryLimit, "location_text.route_budget");
        Writer writer(limits.text_bytes);
        writer.add("[");
        const auto &root = location.root_address();
        if (root.posix_bytes())
            writer.quoted(*root.posix_bytes());
        else
            writer.quoted(*root.windows_units());
        for (const auto &name : location.components()) {
            writer.add(", ");
            if (name.posix_bytes())
                writer.quoted(*name.posix_bytes());
            else
                writer.quoted(*name.windows_units());
        }
        writer.add("]");
        return Result::available(std::move(writer).take());
    } catch (const Rejected &error) {
        return Result::unavailable(error.detail.state, error.detail.error);
    }
}
ck::files::Observation<FilesLocationCompletionQuery> parse_files_location_completion(std::string_view text,
                                                                                     FilesLocationTextSyntax syntax,
                                                                                     ck::files::NameEncoding encoding,
                                                                                     FilesLocationTextLimits limits) {
    using Result = ck::files::Observation<FilesLocationCompletionQuery>;
    try {
        validate(limits);
        if (encoding != NameEncoding::PosixBytes && encoding != NameEncoding::WindowsUtf16)
            reject(AdapterError::InvalidIdentifier, "location_completion.encoding");
        if (text.size() > limits.text_bytes)
            reject(AdapterError::InventoryLimit, "location_completion.text_bytes");
        if (syntax != FilesLocationTextSyntax::Path && syntax != FilesLocationTextSyntax::ExactNames)
            reject(AdapterError::InvalidIdentifier, "location_completion.syntax");
        const auto build = [&](FilesPaneLocation parent, NativeText prefix, std::string leading, char separator) {
            if (parent.components().size() == limits.components)
                reject(AdapterError::InventoryLimit, "location_completion.depth");
            // Partial navigation names are legal prefixes, but separators and
            // NUL are not. A completed proposal must pass NativeName validation.
            if (encoding == NameEncoding::PosixBytes) {
                if (prefix.bytes.find_first_of(std::string_view("\0/", 2)) != prefix.bytes.npos)
                    reject(AdapterError::InvalidPath, "location_completion.prefix");
                return Result::available(FilesLocationCompletionQuery(std::move(parent), std::move(prefix.bytes),
                                                                      std::move(leading), syntax, separator, limits));
            }
            if (prefix.units.find_first_of(std::u16string_view(u"\0/\\", 3)) != prefix.units.npos)
                reject(AdapterError::InvalidPath, "location_completion.prefix");
            return Result::available(FilesLocationCompletionQuery(std::move(parent), std::move(prefix.units),
                                                                  std::move(leading), syntax, separator, limits));
        };
        const auto remaining = [&](const FilesPaneLocation &parent) {
            const auto &root = parent.root_address();
            const auto width = encoding == NameEncoding::PosixBytes ? std::size_t{1} : sizeof(char16_t);
            const bool separator =
                !parent.components().empty() ||
                (root.posix_bytes() ? root.posix_bytes()->back() != '/'
                                    : root.windows_units()->back() != u'/' && root.windows_units()->back() != u'\\');
            const auto extra = separator ? width : 0;
            if (extra > limits.path.max_native_bytes - parent.native_bytes())
                reject(AdapterError::InventoryLimit, "location_completion.native_bytes");
            return NativePathLimits{limits.path.max_native_bytes - parent.native_bytes() - extra};
        };
        if (syntax == FilesLocationTextSyntax::Path) {
            const auto split = text.find_last_of(encoding == NameEncoding::PosixBytes ? "/" : "/\\");
            if (split == text.npos)
                reject(AdapterError::InvalidPath, "location_completion.absolute_parent");
            auto parent = parse_files_location_text(text.substr(0, split + 1), syntax, encoding, limits);
            if (!parent.value())
                return Result::unavailable(parent.unavailable_detail()->state, parent.unavailable_detail()->error);
            NativeText prefix{encoding, remaining(*parent.value()), {}, {}};
            for (auto at = split + 1; at < text.size();) {
                const auto decoded = detail::decode_utf8_scalar(text.substr(at));
                if (!decoded.units || !safe_scalar(decoded.value))
                    reject(AdapterError::InvalidPath, "location_completion.utf8");
                prefix.scalar(decoded.value);
                at += decoded.units;
            }
            return build(std::move(parent).take_value(), std::move(prefix), std::string(text), text[split]);
        }
        Reader reader(text, encoding);
        reader.space();
        reader.expect('[');
        reader.space();
        auto root = std::move(reader.quoted(limits.path)).address();
        if (!root.value())
            return Result::unavailable(root.unavailable_detail()->state, root.unavailable_detail()->error);
        auto parent = FilesPaneLocation(std::move(root).take_value());
        // Preserve the user's complete anchor lexeme; a closing list marker
        // is replaced only by an explicit completion edit, never during parsing.
        const auto root_end = reader.position();
        reader.space();
        if (!reader.consume(',')) {
            if (!reader.end()) {
                reader.expect(']');
                reader.space();
                if (!reader.end())
                    reject(AdapterError::InvalidPath, "location_completion.trailing_text");
            }
            NativeText prefix{encoding, remaining(parent), {}, {}};
            return build(std::move(parent), std::move(prefix), std::string(text.substr(0, root_end)) + ", \"", '/');
        }
        // Gather complete parents once. Avoid repeated route-vector copies for
        // deeply nested literal routes; the complete factory adopts at the end.
        std::vector<NativeName> names;
        auto used = parent.native_bytes();
        for (;;) {
            if (names.size() == limits.components)
                reject(AdapterError::InventoryLimit, "location_completion.depth");
            reader.space();
            const auto &address = parent.root_address();
            const auto width = encoding == NameEncoding::PosixBytes ? std::size_t{1} : sizeof(char16_t);
            const bool separator =
                !names.empty() || (address.posix_bytes() ? address.posix_bytes()->back() != '/'
                                                         : address.windows_units()->back() != u'/' &&
                                                               address.windows_units()->back() != u'\\');
            const auto extra = separator ? width : 0;
            if (extra > limits.path.max_native_bytes - used)
                reject(AdapterError::InventoryLimit, "location_completion.native_bytes");
            const NativePathLimits budget{limits.path.max_native_bytes - used - extra};
            const bool empty = reader.end();
            bool closed = false;
            auto prefix = empty ? NativeText{encoding, budget, {}, {}} : reader.quoted(budget, true, &closed);
            const auto content_end = reader.position() - (closed ? 1 : 0);
            reader.space();
            if (closed && reader.consume(',')) {
                auto name = std::move(prefix).name();
                if (!name)
                    reject(AdapterError::InvalidIdentifier, "location_completion.parent_component");
                used +=
                    extra + (name->posix_bytes() ? name->posix_bytes()->size() : name->windows_units()->size() * width);
                names.push_back(std::move(*name));
                continue;
            }
            if (closed && !reader.end()) {
                reader.expect(']');
                reader.space();
            }
            if (!reader.end())
                reject(AdapterError::InvalidPath, "location_completion.trailing_text");
            auto route = FilesPaneLocation::from_components(parent.root_address(), std::move(names), limits.path,
                                                            limits.components);
            if (!route.value())
                return Result::unavailable(route.unavailable_detail()->state, route.unavailable_detail()->error);
            auto leading = std::string(text.substr(0, content_end));
            if (empty)
                leading += '"';
            return build(std::move(route).take_value(), std::move(prefix), std::move(leading), '/');
        }
    } catch (const Rejected &error) {
        return Result::unavailable(error.detail.state, error.detail.error);
    }
}
bool FilesLocationCompletionQuery::matches(const ck::files::NativeName &name) const noexcept {
    if (!parent_.root_address().is_valid() || !name.is_valid())
        return false;
    if (const auto *bytes = posix_prefix())
        return name.posix_bytes() && name.posix_bytes()->starts_with(*bytes);
    return name.windows_units() && name.windows_units()->starts_with(*windows_prefix());
}
ck::files::Observation<FilesLocationProposal>
FilesLocationCompletionQuery::propose_directory(const ck::files::NativeName &name) const {
    using Result = ck::files::Observation<FilesLocationProposal>;
    try {
        if (!matches(name))
            reject(AdapterError::InvalidIdentifier, "location_completion.candidate");
        auto location = parent_.descend(name, limits_.path, limits_.components);
        if (!location.value())
            return Result::unavailable(location.unavailable_detail()->state, location.unavailable_detail()->error);
        Writer writer(limits_.text_bytes);
        writer.add(leading_);
        if (syntax_ == FilesLocationTextSyntax::ExactNames) {
            Writer suffix(limits_.text_bytes);
            if (const auto *bytes = posix_prefix())
                suffix.quoted(std::string_view(*name.posix_bytes()).substr(bytes->size()));
            else
                suffix.quoted(std::u16string_view(*name.windows_units()).substr(windows_prefix()->size()));
            const auto quoted = std::move(suffix).take();
            writer.add(std::string_view(quoted).substr(1, quoted.size() - 2));
            writer.add("\"]");
        } else {
            if (const auto *bytes = posix_prefix())
                writer.add(std::string_view(*name.posix_bytes()).substr(bytes->size()));
            else {
                const auto tail = std::u16string_view(*name.windows_units()).substr(windows_prefix()->size());
                for (std::size_t at = 0; at < tail.size();) {
                    const auto scalar = detail::decode_utf16_scalar(tail.substr(at));
                    if (!scalar.units)
                        reject(AdapterError::UnrepresentableName, "location_completion.utf16");
                    const auto encoded = detail::encode_utf8_scalar(scalar.value);
                    writer.add(std::string_view(encoded.data.data(), encoded.units));
                    at += scalar.units;
                }
            }
            writer.add(std::string_view(&separator_, 1));
        }
        auto text = std::move(writer).take();
        auto parsed = parse_files_location_text(text, syntax_, parent_.root_address().encoding(), limits_);
        if (!parsed.value())
            return Result::unavailable(parsed.unavailable_detail()->state, parsed.unavailable_detail()->error);
        if (syntax_ == FilesLocationTextSyntax::ExactNames && *parsed.value() != *location.value())
            reject(AdapterError::InvalidIdentifier, "location_completion.roundtrip");
        return Result::available({std::move(text), std::move(location).take_value()});
    } catch (const Rejected &error) {
        return Result::unavailable(error.detail.state, error.detail.error);
    }
}
} // namespace ck::vision
