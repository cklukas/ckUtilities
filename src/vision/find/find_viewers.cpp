// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "find_viewers.hpp"

#include <algorithm>
#include <any>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <cvision/core/text.hpp>
#include <cvision/core/utf8.hpp>
#include <cvision/ui/layout.hpp>
#include <cvision/widgets/button.hpp>
#include <cvision/widgets/label.hpp>
#include <cvision/widgets/table.hpp>
#include <cvision/widgets/text_layout.hpp>
#include <cvision/widgets/text_view.hpp>
#include <cvision/widgets/tree_view.hpp>

#include "json_view_core.hpp"
#include "markdown_profile.hpp"

namespace ck::vision
{
namespace
{

constexpr std::size_t kPreviewMaxBytes = 256 * 1024;
constexpr std::size_t kInlinePreviewMaxBytes = 4096;
constexpr std::size_t kHexEditorMaxBytes = 16 * 1024 * 1024;

std::string ascii_lower(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (unsigned char ch : input)
        out.push_back(static_cast<char>(std::tolower(ch)));
    return out;
}

std::string display_basename(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos || slash + 1 >= path.size())
        return std::string(path);
    return std::string(path.substr(slash + 1));
}

bool has_json_extension(std::string_view path)
{
    const std::string lower = ascii_lower(display_basename(path));
    return lower.size() >= 5 && lower.substr(lower.size() - 5) == ".json";
}

std::optional<std::string> decode_display_text(std::string_view bytes)
{
    if (bytes.find('\0') != std::string_view::npos || !ckv::utf8::is_valid(bytes))
        return std::nullopt;

    std::string out;
    out.reserve(bytes.size());
    std::size_t pos = 0;
    while (pos < bytes.size())
    {
        const char ch = bytes[pos];
        if (ch == '\r')
        {
            ++pos;
            if (pos < bytes.size() && bytes[pos] == '\n')
                ++pos;
            out.push_back('\n');
            continue;
        }
        if (ch == '\n')
        {
            ++pos;
            out.push_back('\n');
            continue;
        }
        if (ch == '\t')
        {
            ++pos;
            out.append("    ");
            continue;
        }
        const std::size_t start = pos;
        const char32_t cp = ckv::utf8::decode(bytes, pos);
        if (cp == ckv::utf8::replacement_char && bytes.substr(start, pos - start) != "\xEF\xBF\xBD")
            return std::nullopt;
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F))
            return std::nullopt;
        out.append(bytes.substr(start, pos - start));
    }
    return out;
}

std::string format_hex_dump(std::string_view bytes)
{
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    if (bytes.empty())
        return "(empty file — 0 bytes)";

    std::ostringstream out;
    for (std::size_t row_start = 0; row_start < bytes.size(); row_start += 16)
    {
        out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << row_start << std::dec << "  ";
        const std::size_t count = std::min<std::size_t>(16, bytes.size() - row_start);
        for (std::size_t col = 0; col < 16; ++col)
        {
            if (col < count)
            {
                const auto value = static_cast<unsigned char>(bytes[row_start + col]);
                out << kHexDigits[value >> 4U] << kHexDigits[value & 0x0FU] << ' ';
            }
            else
            {
                out << "   ";
            }
            if (col == 7)
                out << ' ';
        }
        out << " | ";
        for (std::size_t col = 0; col < count; ++col)
        {
            const auto value = static_cast<unsigned char>(bytes[row_start + col]);
            out << ((value >= 0x20U && value <= 0x7EU) ? static_cast<char>(value) : '.');
        }
        out << '\n';
    }
    return out.str();
}

std::string format_hex_byte(std::uint8_t value)
{
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string text(2, '0');
    text[0] = kHexDigits[value >> 4U];
    text[1] = kHexDigits[value & 0x0FU];
    return text;
}

std::string format_hex_offset(std::size_t offset)
{
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << offset;
    return out.str();
}

int parse_hex_nibble(char ch) noexcept
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return 10 + (ch - 'a');
    if (ch >= 'A' && ch <= 'F')
        return 10 + (ch - 'A');
    return -1;
}

std::optional<std::uint8_t> parse_hex_byte(std::string_view input) noexcept
{
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
        input.remove_prefix(1);
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
        input.remove_suffix(1);
    if (input.size() >= 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X'))
        input.remove_prefix(2);
    if (input.empty() || input.size() > 2)
        return std::nullopt;
    if (input.size() == 1)
    {
        const int low = parse_hex_nibble(input[0]);
        return low < 0 ? std::nullopt : std::optional<std::uint8_t>(static_cast<std::uint8_t>(low));
    }
    const int high = parse_hex_nibble(input[0]);
    const int low = parse_hex_nibble(input[1]);
    if (high < 0 || low < 0)
        return std::nullopt;
    return static_cast<std::uint8_t>((high << 4) | low);
}

std::string escape_json_pointer_token(std::string_view segment)
{
    std::string out;
    out.reserve(segment.size());
    for (char ch : segment)
    {
        if (ch == '~')
            out += "~0";
        else if (ch == '/')
            out += "~1";
        else
            out.push_back(ch);
    }
    return out;
}

std::string json_pointer_for_node(const Node *node)
{
    std::vector<std::string> segments;
    const Node *cursor = node;
    while (cursor != nullptr && cursor->parent != nullptr)
    {
        const Node *parent = cursor->parent;
        if (parent->value != nullptr && parent->value->is_array())
        {
            std::size_t index = 0;
            for (; index < parent->children.size(); ++index)
            {
                if (parent->children[index].get() == cursor)
                    break;
            }
            segments.push_back(std::to_string(index));
        }
        else
        {
            segments.push_back(escape_json_pointer_token(cursor->key));
        }
        cursor = parent;
    }
    std::string pointer;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it)
    {
        pointer.push_back('/');
        pointer.append(*it);
    }
    return pointer;
}

class HexTableModel final : public ckv::widgets::TableModel
{
public:
    std::vector<std::uint8_t> bytes;
    std::function<void()> on_modified;

    std::size_t row_count() const override
    {
        return bytes.empty() ? 1 : (bytes.size() + 15U) / 16U;
    }

    ckv::widgets::TableRowId row_id_at(std::size_t display_index) const override
    {
        return static_cast<ckv::widgets::TableRowId>(display_index + 1U);
    }

    std::optional<std::size_t> index_of(ckv::widgets::TableRowId row) const override
    {
        if (row == ckv::widgets::kInvalidTableRowId)
            return std::nullopt;
        const std::size_t idx = static_cast<std::size_t>(row - 1U);
        return idx < row_count() ? std::optional<std::size_t>(idx) : std::nullopt;
    }

    ckv::widgets::TableCell cell(ckv::widgets::TableCellRef reference) const override
    {
        const std::size_t row_idx = static_cast<std::size_t>(reference.row - 1U);
        const std::size_t row_offset = row_idx * 16U;
        if (reference.column == 0)
        {
            return {.value = format_hex_offset(row_offset), .display = {}, .style = std::nullopt, .editable = false};
        }
        if (reference.column >= 1 && reference.column <= 16)
        {
            const std::size_t byte_index = row_offset + (reference.column - 1U);
            if (byte_index < bytes.size())
            {
                return {.value = format_hex_byte(bytes[byte_index]),
                        .display = {},
                        .style = std::nullopt,
                        .editable = true};
            }
            const bool append_slot = (byte_index == bytes.size());
            return {.value = std::string("--"),
                    .display = {},
                    .style = std::nullopt,
                    .editable = append_slot};
        }
        std::string ascii;
        ascii.reserve(16);
        const std::size_t count = row_offset < bytes.size()
                                      ? std::min<std::size_t>(16U, bytes.size() - row_offset)
                                      : 0U;
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::uint8_t value = bytes[row_offset + i];
            ascii.push_back((value >= 0x20U && value <= 0x7EU) ? static_cast<char>(value) : '.');
        }
        return {.value = std::move(ascii), .display = {}, .style = std::nullopt, .editable = count > 0};
    }

    ckv::widgets::TableEditResult commit(ckv::widgets::TableCellRef reference,
                                         const ckv::widgets::CellValue &value) override
    {
        const auto *text = std::get_if<std::string>(&value);
        if (text == nullptr)
            return ckv::widgets::TableEditResult::reject("Expected text input.");

        const std::size_t row_idx = static_cast<std::size_t>(reference.row - 1U);
        const std::size_t row_offset = row_idx * 16U;
        if (reference.column >= 1 && reference.column <= 16)
        {
            const std::size_t byte_index = row_offset + (reference.column - 1U);
            const auto parsed = parse_hex_byte(*text);
            if (!parsed)
                return ckv::widgets::TableEditResult::reject("Enter a hex byte (00-FF).");
            if (byte_index == bytes.size())
                bytes.push_back(*parsed);
            else if (byte_index < bytes.size())
                bytes[byte_index] = *parsed;
            else
                return ckv::widgets::TableEditResult::reject("Byte offset is past end of file.");
            if (on_modified)
                on_modified();
            return ckv::widgets::TableEditResult::accept();
        }
        if (reference.column == 17)
        {
            if (row_offset >= bytes.size())
                return ckv::widgets::TableEditResult::reject("No bytes on this row.");
            const std::size_t available = std::min<std::size_t>(16U, bytes.size() - row_offset);
            if (text->size() > available)
                return ckv::widgets::TableEditResult::reject("ASCII slice exceeds row byte count (" + std::to_string(available) + ").");
            for (std::size_t i = 0; i < text->size(); ++i)
                bytes[row_offset + i] = static_cast<std::uint8_t>((*text)[i]);
            if (on_modified)
                on_modified();
            return ckv::widgets::TableEditResult::accept();
        }
        return ckv::widgets::TableEditResult::reject("Offset column is read-only.");
    }
};

std::string editor_status_message(ckv::widgets::EditorFileStatus status)
{
    switch (status)
    {
    case ckv::widgets::EditorFileStatus::Ok:
        return "Saved.";
    case ckv::widgets::EditorFileStatus::NotFound:
        return "The file does not exist or could not be read.";
    case ckv::widgets::EditorFileStatus::InvalidText:
        return "The file is not valid UTF-8 text or exceeds editor size limits (try Edit Hex instead).";
    case ckv::widgets::EditorFileStatus::Conflict:
        return "The file was modified on disk since it was opened.";
    case ckv::widgets::EditorFileStatus::NoPath:
        return "No target file path is set.";
    case ckv::widgets::EditorFileStatus::Error:
        return "Could not write the file.";
    }
    return "File operation failed.";
}

} // namespace

struct FindWorkspaceViewers::TextEditorSession
{
    std::string path;
    std::shared_ptr<ckv::widgets::EditorDocument> document;
    ckv::widgets::EditorWindow *window = nullptr;
    bool closing_confirmed = false;
};

struct FindWorkspaceViewers::HexEditorSession
{
    std::string path;
    std::vector<std::uint8_t> original_bytes;
    std::optional<ckv::FileFingerprint> fingerprint;
    HexTableModel model;
    ckv::widgets::Window *window = nullptr;
    ckv::widgets::Table *table = nullptr;
    bool modified = false;
    bool closing_confirmed = false;
};

struct FindWorkspaceViewers::JsonViewerSession
{
    std::string path;
    std::optional<ckv::FileFingerprint> fingerprint;
    std::unique_ptr<json> document;
    std::unique_ptr<Node> root;
    std::unordered_map<const Node *, std::uint64_t> node_ids;
    std::unordered_map<std::uint64_t, Node *> nodes_by_id;
    std::uint64_t next_id = 1;
    ckv::widgets::Window *window = nullptr;
    ckv::widgets::TreeView *tree = nullptr;
    bool modified = false;
    bool closing_confirmed = false;

    void reindex(Node &current)
    {
        const std::uint64_t id = next_id++;
        node_ids.emplace(&current, id);
        nodes_by_id.emplace(id, &current);
        for (auto &child : current.children)
            reindex(*child);
    }

    ckv::widgets::TreeNode make_tree_node(const Node &current) const
    {
        ckv::widgets::TreeNode node;
        node.label = getContentLabel(&current, 96);
        node.expanded = current.expanded;
        node.id = node_ids.at(&current);
        node.user_data = const_cast<Node *>(&current);
        node.children.reserve(current.children.size());
        for (const auto &child : current.children)
            node.children.push_back(make_tree_node(*child));
        return node;
    }

    void rebuild_tree()
    {
        if (tree == nullptr || !root)
            return;
        node_ids.clear();
        nodes_by_id.clear();
        next_id = 1;
        reindex(*root);
        tree->set_roots({make_tree_node(*root)});
    }
};

FindWorkspaceViewers::FindWorkspaceViewers(ckv::ui::Application &application,
                                           ckv::widgets::Desktop &desktop,
                                           ckv::FileSystem &files)
    : application_(application), desktop_(desktop), files_(files)
{
    ckv::widgets::register_standard_syntax_profiles(syntax_profiles_);
    register_markdown_syntax_profile(syntax_profiles_);
}

FindWorkspaceViewers::~FindWorkspaceViewers()
{
    lifetime_.reset();
    for (auto &session : hex_editors_)
    {
        if (session && session->table != nullptr)
            session->table->clear_model();
    }
}

ckv::Rect FindWorkspaceViewers::next_document_bounds() noexcept
{
    const ckv::Rect area = desktop_.content_area();
    const int offset = (cascade_step_ % 5) * 2;
    ++cascade_step_;
    const int width = std::max(44, area.width - std::min(8, std::max(0, area.width - 44)));
    const int height = std::max(12, area.height - std::min(4, std::max(0, area.height - 12)));
    const int x = area.x + std::min(offset, std::max(0, area.width - width));
    const int y = area.y + std::min(offset / 2, std::max(0, area.height - height));
    return {x, y, std::min(width, area.width), std::min(height, area.height)};
}

void FindWorkspaceViewers::show_message(ckv::widgets::MessageBoxKind kind,
                                        std::string title,
                                        std::string message)
{
    message_box_.reset();
    message_box_.emplace(ckv::widgets::present_modal_message_box(
        application_, desktop_,
        {kind, std::move(title), std::move(message), ckv::widgets::MessageBoxButtons::Ok}));
    message_box_->set_completion_handler([](ckv::widgets::MessageBoxResult) {});
}

std::string FindWorkspaceViewers::inline_preview_for(const std::string &path) const
{
    if (path.empty())
        return "Select a matched file to preview its contents.";
    if (files_.is_directory(path))
        return "[Directory] " + path;

    const auto bounded = files_.read_file_bounded(path, kInlinePreviewMaxBytes);
    if (!bounded)
        return "Preview unavailable (file could not be read): " + path;

    if (has_json_extension(path))
    {
        const auto full = files_.read_file_bounded(path, kPreviewMaxBytes);
        if (full)
        {
            try
            {
                const json parsed = parseJsonWithSpecialNumbers(full->contents);
                std::string formatted = parsed.dump(2);
                if (formatted.size() > kInlinePreviewMaxBytes)
                {
                    formatted.resize(kInlinePreviewMaxBytes);
                    formatted += "\n... (JSON preview truncated; press F3 for full JSON Tree)";
                }
                return formatted;
            }
            catch (...)
            {
            }
        }
    }

    if (const auto decoded = decode_display_text(bounded->contents))
    {
        if (decoded->empty())
            return "(empty text file — 0 bytes)";
        if (bounded->contents.size() == kInlinePreviewMaxBytes)
            return *decoded + "\n... (first 4 KiB shown; press F3 to view or F4 to edit)";
        return *decoded;
    }

    std::string hex = format_hex_dump(bounded->contents);
    if (bounded->contents.size() == kInlinePreviewMaxBytes)
        hex += "... (first 4 KiB hex dump; press F3 for Hex View or F8 for Hex Edit)";
    return hex;
}

bool FindWorkspaceViewers::open_preview(const std::string &path, FindPreviewMode mode)
{
    if (path.empty())
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "View file", "Select a file from a Found Files window first.");
        return false;
    }
    if (files_.is_directory(path))
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "View directory", "'" + path + "' is a directory.");
        return false;
    }

    switch (mode)
    {
    case FindPreviewMode::Json:
        return open_json_viewer(path);
    case FindPreviewMode::Hex:
        return open_text_or_hex_preview(path, true);
    case FindPreviewMode::Text:
        return open_text_or_hex_preview(path, false);
    case FindPreviewMode::Auto:
        if (has_json_extension(path) && open_json_viewer(path))
            return true;
        return open_text_or_hex_preview(path, false);
    }
    return false;
}

bool FindWorkspaceViewers::open_text_or_hex_preview(const std::string &path, bool force_hex)
{
    const auto read = files_.read_file_bounded(path, kPreviewMaxBytes);
    if (!read)
    {
        show_message(ckv::widgets::MessageBoxKind::Error, "View file", "Could not read '" + path + "'.");
        return false;
    }

    std::optional<std::string> decoded = force_hex ? std::nullopt : decode_display_text(read->contents);
    const bool starts_hex = !decoded.has_value();
    const std::string base = display_basename(path);

    auto window = std::make_unique<ckv::widgets::Window>((starts_hex ? "Hex View: " : "Text View: ") + base);
    auto *const win_ptr = window.get();
    window->set_bounds(next_document_bounds());
    window->set_min_size({42, 10});
    window->set_footer(read->contents.size() >= kPreviewMaxBytes
                           ? path + " (first 256 KiB read-only)"
                           : path + " (" + std::to_string(read->contents.size()) + " B, read-only)");
    window->cancel_request = [win_ptr] { win_ptr->close(); };

    auto root = std::make_unique<ckv::ui::Column>();
    root->set_spacing(1);
    auto text_view = std::make_unique<ckv::widgets::TextView>();
    auto *const tv_ptr = text_view.get();
    text_view->set_wrap_mode(ckv::widgets::WrapMode::None);
    const std::string raw_bytes = read->contents;
    const std::string text_body = decoded.value_or(std::string{});
    text_view->set_text(starts_hex ? format_hex_dump(raw_bytes) : text_body);
    root->add_item(std::move(text_view), {.policy = ckv::ui::SizePolicy::Expanding});

    auto bar = std::make_unique<ckv::ui::Row>();
    bar->set_spacing(1);
    const std::weak_ptr<void> weak_self = lifetime_;

    if (decoded.has_value())
    {
        auto toggle_hex = std::make_unique<ckv::widgets::Button>("Show &Hex");
        auto *const toggle_ptr = toggle_hex.get();
        toggle_hex->on_press = [win_ptr, tv_ptr, toggle_ptr, base, raw_bytes, text_body, showing_hex = false]() mutable {
            showing_hex = !showing_hex;
            tv_ptr->set_text(showing_hex ? format_hex_dump(raw_bytes) : text_body);
            tv_ptr->set_top_line(0);
            toggle_ptr->set_text(showing_hex ? "Show &Text" : "Show &Hex");
            win_ptr->set_title((showing_hex ? "Hex View: " : "Text View: ") + base);
        };
        bar->add_item(std::move(toggle_hex));

        auto toggle_wrap = std::make_unique<ckv::widgets::Button>("&Wrap: Off");
        auto *const wrap_ptr = toggle_wrap.get();
        toggle_wrap->on_press = [tv_ptr, wrap_ptr, wrapped = false]() mutable {
            wrapped = !wrapped;
            tv_ptr->set_wrap_mode(wrapped ? ckv::widgets::WrapMode::Word : ckv::widgets::WrapMode::None);
            wrap_ptr->set_text(wrapped ? "&Wrap: On" : "&Wrap: Off");
        };
        bar->add_item(std::move(toggle_wrap));

        auto edit_btn = std::make_unique<ckv::widgets::Button>("&Edit Text...");
        edit_btn->on_press = [this, weak_self, path] {
            if (!weak_self.expired())
                open_text_editor(path);
        };
        bar->add_item(std::move(edit_btn));
    }

    auto hex_edit_btn = std::make_unique<ckv::widgets::Button>("Hex E&dit...");
    hex_edit_btn->on_press = [this, weak_self, path] {
        if (!weak_self.expired())
            open_hex_editor(path);
    };
    bar->add_item(std::move(hex_edit_btn));

    auto close_btn = std::make_unique<ckv::widgets::Button>("&Close");
    close_btn->on_press = [win_ptr] { win_ptr->close(); };
    bar->add_item(std::move(close_btn));

    root->add_item(std::move(bar));
    window->set_content(std::move(root));
    window->set_content_margins(1, 0, 1, 0);
    desktop_.present_modeless(ckv::widgets::WindowHandle{std::move(window), tv_ptr}, application_);
    return true;
}

bool FindWorkspaceViewers::open_text_editor(const std::string &path)
{
    if (path.empty())
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Edit file", "Select a file from a Found Files window first.");
        return false;
    }
    for (const auto &existing : text_editors_)
    {
        if (existing->path == path && existing->window != nullptr)
        {
            desktop_.activate(existing->window);
            return true;
        }
    }

    auto session = std::make_unique<TextEditorSession>();
    session->path = path;
    session->document = std::make_shared<ckv::widgets::EditorDocument>();
    auto window = std::make_unique<ckv::widgets::EditorWindow>(
        "Edit: " + display_basename(path), session->document, files_, &syntax_profiles_);
    const auto open_status = window->open(path);
    if (open_status != ckv::widgets::EditorFileStatus::Ok)
    {
        show_message(ckv::widgets::MessageBoxKind::Error, "Edit text", editor_status_message(open_status));
        return false;
    }
    (void)window->editor().set_selection({session->document->begin(), session->document->begin()});

    window->set_bounds(next_document_bounds());
    window->set_min_size({48, 12});
    window->editor().set_show_line_numbers(true);
    window->set_footer("Ctrl+S Save | Alt+F3 Close");

    TextEditorSession *const raw_session = session.get();
    session->window = window.get();
    const std::weak_ptr<void> weak_self = lifetime_;

    window->close_request = [this, weak_self, raw_session] {
        if (weak_self.expired() || raw_session->closing_confirmed || raw_session->window == nullptr)
            return true;
        if (!raw_session->window->controller().modified())
            return true;

        close_confirmation_.reset();
        close_confirmation_.emplace(ckv::widgets::present_modal_message_box(
            application_, desktop_,
            {ckv::widgets::MessageBoxKind::Warning,
             "Unsaved text changes",
             "Save changes to '" + display_basename(raw_session->path) + "' before closing?",
             ckv::widgets::MessageBoxButtons::YesNoCancel}));
        close_confirmation_->set_completion_handler([this, weak_self, raw_session](ckv::widgets::MessageBoxResult choice) {
            if (weak_self.expired() || raw_session->window == nullptr)
                return;
            if (choice == ckv::widgets::MessageBoxResult::Cancel)
                return;
            const auto close_choice = choice == ckv::widgets::MessageBoxResult::Yes
                                          ? ckv::widgets::EditorCloseChoice::Save
                                          : ckv::widgets::EditorCloseChoice::Discard;
            const auto st = raw_session->window->request_close(close_choice);
            if (st != ckv::widgets::EditorFileStatus::Ok)
            {
                show_message(ckv::widgets::MessageBoxKind::Error, "Save file", editor_status_message(st));
                return;
            }
            raw_session->closing_confirmed = true;
            raw_session->window->close();
            raw_session->closing_confirmed = false;
        });
        return false;
    };

    window->on_closed = [this, weak_self, raw_session] {
        if (weak_self.expired())
            return;
        ckv::widgets::EditorWindow *const closing = raw_session->window;
        raw_session->window = nullptr;
        auto it = std::find_if(text_editors_.begin(), text_editors_.end(),
                               [raw_session](const auto &item) { return item.get() == raw_session; });
        std::unique_ptr<TextEditorSession> removed;
        if (it != text_editors_.end())
        {
            removed = std::move(*it);
            text_editors_.erase(it);
        }
        if (closing != nullptr)
            desktop_.remove_window(closing);
    };

    auto *const focus_target = &window->editor();
    text_editors_.push_back(std::move(session));
    desktop_.present_modeless(ckv::widgets::WindowHandle{std::move(window), focus_target}, application_);
    return true;
}

bool FindWorkspaceViewers::open_hex_editor(const std::string &path)
{
    if (path.empty())
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Hex editor", "Select a file from a Found Files window first.");
        return false;
    }
    for (const auto &existing : hex_editors_)
    {
        if (existing->path == path && existing->window != nullptr)
        {
            desktop_.activate(existing->window);
            return true;
        }
    }

    const auto probe = files_.read_file_bounded(path, kHexEditorMaxBytes + 1U);
    if (!probe)
    {
        show_message(ckv::widgets::MessageBoxKind::Error, "Hex editor", "Could not read '" + path + "'.");
        return false;
    }
    if (probe->contents.size() > kHexEditorMaxBytes)
    {
        show_message(ckv::widgets::MessageBoxKind::Warning,
                     "Hex editor limit",
                     "File exceeds the 16 MiB interactive hex-edit buffer; opening read-only Hex View instead.");
        return open_text_or_hex_preview(path, true);
    }

    const auto full_read = files_.read_file(path);
    if (!full_read)
    {
        show_message(ckv::widgets::MessageBoxKind::Error, "Hex editor", "Could not read '" + path + "'.");
        return false;
    }

    auto session = std::make_unique<HexEditorSession>();
    session->path = path;
    session->fingerprint = full_read->fingerprint;
    session->original_bytes.assign(full_read->contents.begin(), full_read->contents.end());
    session->model.bytes = session->original_bytes;

    HexEditorSession *const raw_session = session.get();
    const std::weak_ptr<void> weak_self = lifetime_;

    auto window = std::make_unique<ckv::widgets::Window>("Hex Edit: " + display_basename(path));
    raw_session->window = window.get();
    window->set_bounds(next_document_bounds());
    window->set_min_size({68, 12});

    auto root = std::make_unique<ckv::ui::Column>();
    root->set_spacing(1);

    auto table = std::make_unique<ckv::widgets::Table>();
    raw_session->table = table.get();
    table->set_banded_rows(true);

    std::vector<ckv::widgets::TableColumn> columns;
    columns.reserve(18);
    columns.push_back({"Offset", 8, 8, ckv::widgets::TableCellType::Text, false});
    for (unsigned col = 0; col < 16U; ++col)
    {
        std::ostringstream hdr;
        hdr << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << col;
        columns.push_back({hdr.str(), 2, 2, ckv::widgets::TableCellType::Text, true});
    }
    columns.push_back({"ASCII", 16, 8, ckv::widgets::TableCellType::Text, true});
    table->set_columns(std::move(columns));
    table->set_model(raw_session->model);

    auto refresh_chrome = [raw_session] {
        if (raw_session->window == nullptr || raw_session->table == nullptr)
            return;
        const std::string base = display_basename(raw_session->path);
        raw_session->window->set_title("Hex Edit: " + base + (raw_session->modified ? " *" : ""));

        std::ostringstream footer;
        if (const auto sel = raw_session->table->selected_cell())
        {
            const std::size_t row_idx = static_cast<std::size_t>(sel->row - 1U);
            const std::size_t col_offset = (sel->column >= 1U && sel->column <= 16U) ? (sel->column - 1U) : 0U;
            const std::size_t byte_offset = row_idx * 16U + col_offset;
            footer << "0x" << format_hex_offset(byte_offset) << " (" << byte_offset << ") | ";
            if (byte_offset < raw_session->model.bytes.size())
            {
                const std::uint8_t b = raw_session->model.bytes[byte_offset];
                footer << "0x" << format_hex_byte(b) << " (" << static_cast<unsigned>(b) << ") | ";
            }
        }
        footer << raw_session->model.bytes.size() << " B";
        if (raw_session->modified)
            footer << " [Modified - Ctrl+S Save]";
        if (!raw_session->table->edit_diagnostic().empty())
            footer << " | " << raw_session->table->edit_diagnostic();
        raw_session->window->set_footer(footer.str());
    };

    raw_session->model.on_modified = [raw_session, refresh_chrome] {
        raw_session->modified = true;
        refresh_chrome();
    };
    table->on_selection_changed = [refresh_chrome](ckv::widgets::TableCellRef) { refresh_chrome(); };
    table->on_edit_committed = [refresh_chrome](ckv::widgets::TableCellRef, const ckv::widgets::TableEditResult &) {
        refresh_chrome();
    };

    auto save_hex = [this, raw_session, refresh_chrome]() -> bool {
        const std::string payload(reinterpret_cast<const char *>(raw_session->model.bytes.data()),
                                  raw_session->model.bytes.size());
        const auto expectation = raw_session->fingerprint
                                     ? ckv::FileWriteExpectation::matching(*raw_session->fingerprint)
                                     : ckv::FileWriteExpectation::any();
        const auto written = files_.write_file_atomic(raw_session->path, payload, expectation);
        if (written.status == ckv::FileWriteStatus::Ok)
        {
            raw_session->fingerprint = written.fingerprint;
            raw_session->original_bytes = raw_session->model.bytes;
            raw_session->modified = false;
            refresh_chrome();
            return true;
        }
        if (written.status == ckv::FileWriteStatus::Conflict)
        {
            show_message(ckv::widgets::MessageBoxKind::Error,
                         "Save conflict",
                         "'" + raw_session->path + "' changed on disk since it was opened. Use Save As to keep a copy.");
            return false;
        }
        show_message(ckv::widgets::MessageBoxKind::Error, "Save hex", "Could not write '" + raw_session->path + "'.");
        return false;
    };

    root->add_item(std::move(table), {.policy = ckv::ui::SizePolicy::Expanding});

    auto bar = std::make_unique<ckv::ui::Row>();
    bar->set_spacing(1);

    auto save_btn = std::make_unique<ckv::widgets::Button>("&Save");
    save_btn->on_press = [weak_self, save_hex] {
        if (!weak_self.expired())
            (void)save_hex();
    };
    bar->add_item(std::move(save_btn));

    auto save_as_btn = std::make_unique<ckv::widgets::Button>("Save &As...");
    save_as_btn->on_press = [this, weak_self, raw_session, refresh_chrome] {
        if (weak_self.expired())
            return;
        ckv::widgets::FileDialogOptions opts;
        opts.suggested_name = display_basename(raw_session->path);
        save_as_dialog_.reset();
        save_as_dialog_.emplace(ckv::widgets::present_modal_file_dialog(
            ckv::widgets::FileDialogMode::Save, files_.parent(raw_session->path), files_, std::move(opts),
            application_, desktop_));
        save_as_dialog_->set_completion_handler([this, weak_self, raw_session, refresh_chrome](ckv::widgets::FileDialogResult res) {
            if (weak_self.expired() || !res.accepted || res.path.empty())
                return;
            const std::string payload(reinterpret_cast<const char *>(raw_session->model.bytes.data()),
                                      raw_session->model.bytes.size());
            const auto written = files_.write_file_atomic(res.path, payload, ckv::FileWriteExpectation::any());
            if (written.status != ckv::FileWriteStatus::Ok)
            {
                show_message(ckv::widgets::MessageBoxKind::Error, "Save As", "Could not write '" + res.path + "'.");
                return;
            }
            raw_session->path = res.path;
            raw_session->fingerprint = written.fingerprint;
            raw_session->original_bytes = raw_session->model.bytes;
            raw_session->modified = false;
            refresh_chrome();
        });
    };
    bar->add_item(std::move(save_as_btn));

    auto append_btn = std::make_unique<ckv::widgets::Button>("&Append 0x00");
    append_btn->on_press = [weak_self, raw_session, refresh_chrome] {
        if (weak_self.expired() || raw_session->table == nullptr)
            return;
        raw_session->model.bytes.push_back(0U);
        raw_session->modified = true;
        raw_session->table->model_changed();
        const std::size_t idx = raw_session->model.bytes.size() - 1U;
        raw_session->table->set_selected_cell({static_cast<ckv::widgets::TableRowId>(idx / 16U + 1U), (idx % 16U) + 1U});
        refresh_chrome();
    };
    bar->add_item(std::move(append_btn));

    auto revert_btn = std::make_unique<ckv::widgets::Button>("&Revert");
    revert_btn->on_press = [weak_self, raw_session, refresh_chrome] {
        if (weak_self.expired() || raw_session->table == nullptr)
            return;
        raw_session->model.bytes = raw_session->original_bytes;
        raw_session->modified = false;
        raw_session->table->model_changed();
        refresh_chrome();
    };
    bar->add_item(std::move(revert_btn));

    auto close_btn = std::make_unique<ckv::widgets::Button>("&Close");
    close_btn->on_press = [weak_self, raw_session] {
        if (!weak_self.expired() && raw_session->window != nullptr)
            raw_session->window->close();
    };
    bar->add_item(std::move(close_btn));

    root->add_item(std::move(bar));
    window->set_content(std::move(root));
    window->set_content_margins(1, 0, 1, 0);

    window->close_request = [this, weak_self, raw_session, save_hex] {
        if (weak_self.expired() || raw_session->closing_confirmed || !raw_session->modified)
            return true;
        close_confirmation_.reset();
        close_confirmation_.emplace(ckv::widgets::present_modal_message_box(
            application_, desktop_,
            {ckv::widgets::MessageBoxKind::Warning,
             "Unsaved hex changes",
             "Save byte changes to '" + display_basename(raw_session->path) + "' before closing?",
             ckv::widgets::MessageBoxButtons::YesNoCancel}));
        close_confirmation_->set_completion_handler([weak_self, raw_session, save_hex](ckv::widgets::MessageBoxResult choice) {
            if (weak_self.expired() || raw_session->window == nullptr || choice == ckv::widgets::MessageBoxResult::Cancel)
                return;
            if (choice == ckv::widgets::MessageBoxResult::Yes && !save_hex())
                return;
            raw_session->closing_confirmed = true;
            raw_session->window->close();
            raw_session->closing_confirmed = false;
        });
        return false;
    };

    window->on_closed = [this, weak_self, raw_session] {
        if (weak_self.expired())
            return;
        if (raw_session->table != nullptr)
        {
            raw_session->table->clear_model();
            raw_session->table = nullptr;
        }
        ckv::widgets::Window *const closing = raw_session->window;
        raw_session->window = nullptr;
        auto it = std::find_if(hex_editors_.begin(), hex_editors_.end(),
                               [raw_session](const auto &item) { return item.get() == raw_session; });
        std::unique_ptr<HexEditorSession> removed;
        if (it != hex_editors_.end())
        {
            removed = std::move(*it);
            hex_editors_.erase(it);
        }
        if (closing != nullptr)
            desktop_.remove_window(closing);
    };

    auto *const focus_target = raw_session->table;
    refresh_chrome();
    hex_editors_.push_back(std::move(session));
    desktop_.present_modeless(ckv::widgets::WindowHandle{std::move(window), focus_target}, application_);
    return true;
}

bool FindWorkspaceViewers::open_json_viewer(const std::string &path)
{
    for (const auto &existing : json_viewers_)
    {
        if (existing->path == path && existing->window != nullptr)
        {
            desktop_.activate(existing->window);
            return true;
        }
    }

    const auto read = files_.read_file(path);
    if (!read)
    {
        show_message(ckv::widgets::MessageBoxKind::Error, "JSON View", "Could not read '" + path + "'.");
        return false;
    }

    std::unique_ptr<json> parsed;
    std::unique_ptr<Node> parsed_root;
    try
    {
        parsed = std::make_unique<json>(parseJsonWithSpecialNumbers(read->contents));
        parsed_root = buildTree(parsed.get(), display_basename(path), nullptr, true);
    }
    catch (const std::exception &ex)
    {
        show_message(ckv::widgets::MessageBoxKind::Error,
                     "Invalid JSON",
                     "Could not parse '" + display_basename(path) + "' as JSON: " + ex.what());
        return false;
    }

    parsed_root->sourceSize = read->contents.size();
    expandToLevel(parsed_root.get(), 2, 0);

    auto session = std::make_unique<JsonViewerSession>();
    session->path = path;
    session->fingerprint = read->fingerprint;
    session->document = std::move(parsed);
    session->root = std::move(parsed_root);

    JsonViewerSession *const raw_session = session.get();
    const std::weak_ptr<void> weak_self = lifetime_;

    auto window = std::make_unique<ckv::widgets::Window>("JSON: " + display_basename(path));
    raw_session->window = window.get();
    window->set_bounds(next_document_bounds());
    window->set_min_size({52, 12});

    auto root_col = std::make_unique<ckv::ui::Column>();
    root_col->set_spacing(1);

    auto tree = std::make_unique<ckv::widgets::TreeView>();
    tree->set_connector_style(ckv::widgets::TreeConnectorStyle::Outline);
    raw_session->tree = tree.get();
    raw_session->rebuild_tree();

    auto refresh_chrome = [raw_session] {
        if (raw_session->window == nullptr)
            return;
        raw_session->window->set_title("JSON: " + display_basename(raw_session->path) + (raw_session->modified ? " *" : ""));
        std::string pointer_label = "/";
        if (raw_session->tree != nullptr)
        {
            if (auto *sel = raw_session->tree->selected(); sel != nullptr && sel->user_data.has_value())
            {
                if (auto *const n = std::any_cast<Node *>(sel->user_data); n != nullptr)
                {
                    const std::string ptr = json_pointer_for_node(n);
                    pointer_label = ptr.empty() ? "/" : ptr;
                }
            }
        }
        raw_session->window->set_footer("Node: " + pointer_label +
                                        (raw_session->modified ? " | [Modified - Ctrl+S Save]" : " | Enter/Twisties Expand"));
    };

    raw_session->tree->on_selection_changed = [refresh_chrome](ckv::widgets::TreeNode &) { refresh_chrome(); };
    root_col->add_item(std::move(tree), {.policy = ckv::ui::SizePolicy::Expanding});

    auto save_json = [this, raw_session, refresh_chrome]() -> bool {
        if (!raw_session->document)
            return false;
        const std::string serialized = raw_session->document->dump(2) + "\n";
        const auto expectation = raw_session->fingerprint
                                     ? ckv::FileWriteExpectation::matching(*raw_session->fingerprint)
                                     : ckv::FileWriteExpectation::any();
        const auto written = files_.write_file_atomic(raw_session->path, serialized, expectation);
        if (written.status != ckv::FileWriteStatus::Ok)
        {
            show_message(ckv::widgets::MessageBoxKind::Error, "Save JSON", "Could not write '" + raw_session->path + "'.");
            return false;
        }
        raw_session->fingerprint = written.fingerprint;
        raw_session->modified = false;
        refresh_chrome();
        return true;
    };

    auto bar = std::make_unique<ckv::ui::Row>();
    bar->set_spacing(1);

    auto expand_btn = std::make_unique<ckv::widgets::Button>("Expand &All");
    expand_btn->on_press = [weak_self, raw_session] {
        if (weak_self.expired() || !raw_session->root)
            return;
        expandAll(raw_session->root.get());
        raw_session->rebuild_tree();
    };
    bar->add_item(std::move(expand_btn));

    auto collapse_btn = std::make_unique<ckv::widgets::Button>("C&ollapse");
    collapse_btn->on_press = [weak_self, raw_session] {
        if (weak_self.expired() || !raw_session->root)
            return;
        collapseAll(raw_session->root.get(), true);
        raw_session->rebuild_tree();
    };
    bar->add_item(std::move(collapse_btn));

    auto edit_node_btn = std::make_unique<ckv::widgets::Button>("&Edit Node...");
    edit_node_btn->on_press = [this, weak_self, raw_session, refresh_chrome] {
        if (weak_self.expired() || raw_session->tree == nullptr || !raw_session->document)
            return;
        auto *sel = raw_session->tree->selected();
        Node *target_node = (sel != nullptr && sel->user_data.has_value()) ? std::any_cast<Node *>(sel->user_data) : raw_session->root.get();
        if (target_node == nullptr || target_node->value == nullptr)
            return;

        const std::string ptr_str = json_pointer_for_node(target_node);
        const std::string initial_json = target_node->value->dump();

        ckv::widgets::DialogDescriptor desc;
        desc.title = "Edit JSON node (" + (ptr_str.empty() ? std::string("/") : ptr_str) + ")";
        desc.minimum_window_size = {58, 9};
        desc.fields.push_back({"&JSON value:", initial_json, [](const std::string &v) { return !v.empty(); }});
        desc.fields.push_back({.label = "Enter any valid JSON literal (e.g. \"text\", 42, true, null, [1,2]).",
                               .kind = ckv::widgets::FieldKind::Note});
        desc.buttons.push_back({"&Update", ckv::widgets::ButtonRole::Accept, nullptr});
        desc.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});

        edit_node_dialog_.reset();
        edit_node_dialog_.emplace(ckv::widgets::present_modal_dialog(std::move(desc), application_, desktop_));
        edit_node_dialog_->set_completion_handler([this, weak_self, raw_session, ptr_str, refresh_chrome](ckv::widgets::DialogResult res) {
            if (weak_self.expired() || !res.accepted || res.values.empty() || !raw_session->document)
                return;
            json replacement;
            try
            {
                replacement = json::parse(res.values[0]);
            }
            catch (const std::exception &ex)
            {
                show_message(ckv::widgets::MessageBoxKind::Error, "Invalid JSON literal", ex.what());
                return;
            }
            if (ptr_str.empty())
                *raw_session->document = std::move(replacement);
            else
                (*raw_session->document)[json::json_pointer(ptr_str)] = std::move(replacement);

            raw_session->root = buildTree(raw_session->document.get(), display_basename(raw_session->path), nullptr, true);
            expandToLevel(raw_session->root.get(), 3, 0);
            raw_session->modified = true;
            raw_session->rebuild_tree();
            refresh_chrome();
        });
    };
    bar->add_item(std::move(edit_node_btn));

    auto save_btn = std::make_unique<ckv::widgets::Button>("&Save");
    save_btn->on_press = [weak_self, save_json] {
        if (!weak_self.expired())
            (void)save_json();
    };
    bar->add_item(std::move(save_btn));

    auto text_edit_btn = std::make_unique<ckv::widgets::Button>("&Text Edit...");
    text_edit_btn->on_press = [this, weak_self, path] {
        if (!weak_self.expired())
            open_text_editor(path);
    };
    bar->add_item(std::move(text_edit_btn));

    auto copy_btn = std::make_unique<ckv::widgets::Button>("Cop&y");
    copy_btn->on_press = [this, weak_self, raw_session] {
        if (weak_self.expired() || raw_session->tree == nullptr)
            return;
        auto *sel = raw_session->tree->selected();
        Node *n = (sel != nullptr && sel->user_data.has_value()) ? std::any_cast<Node *>(sel->user_data) : raw_session->root.get();
        if (n != nullptr)
            application_.set_clipboard_text(reconstructJson(n).dump(2));
    };
    bar->add_item(std::move(copy_btn));

    auto close_btn = std::make_unique<ckv::widgets::Button>("&Close");
    close_btn->on_press = [weak_self, raw_session] {
        if (!weak_self.expired() && raw_session->window != nullptr)
            raw_session->window->close();
    };
    bar->add_item(std::move(close_btn));

    root_col->add_item(std::move(bar));
    window->set_content(std::move(root_col));
    window->set_content_margins(1, 0, 1, 0);

    window->close_request = [this, weak_self, raw_session, save_json] {
        if (weak_self.expired() || raw_session->closing_confirmed || !raw_session->modified)
            return true;
        close_confirmation_.reset();
        close_confirmation_.emplace(ckv::widgets::present_modal_message_box(
            application_, desktop_,
            {ckv::widgets::MessageBoxKind::Warning,
             "Unsaved JSON changes",
             "Save changes to '" + display_basename(raw_session->path) + "' before closing?",
             ckv::widgets::MessageBoxButtons::YesNoCancel}));
        close_confirmation_->set_completion_handler([weak_self, raw_session, save_json](ckv::widgets::MessageBoxResult choice) {
            if (weak_self.expired() || raw_session->window == nullptr || choice == ckv::widgets::MessageBoxResult::Cancel)
                return;
            if (choice == ckv::widgets::MessageBoxResult::Yes && !save_json())
                return;
            raw_session->closing_confirmed = true;
            raw_session->window->close();
            raw_session->closing_confirmed = false;
        });
        return false;
    };

    window->on_closed = [this, weak_self, raw_session] {
        if (weak_self.expired())
            return;
        raw_session->tree = nullptr;
        ckv::widgets::Window *const closing = raw_session->window;
        raw_session->window = nullptr;
        auto it = std::find_if(json_viewers_.begin(), json_viewers_.end(),
                               [raw_session](const auto &item) { return item.get() == raw_session; });
        std::unique_ptr<JsonViewerSession> removed;
        if (it != json_viewers_.end())
        {
            removed = std::move(*it);
            json_viewers_.erase(it);
        }
        if (closing != nullptr)
            desktop_.remove_window(closing);
    };

    auto *const focus_target = raw_session->tree;
    refresh_chrome();
    json_viewers_.push_back(std::move(session));
    desktop_.present_modeless(ckv::widgets::WindowHandle{std::move(window), focus_target}, application_);
    return true;
}

bool FindWorkspaceViewers::save_active_document()
{
    ckv::widgets::Window *const active = desktop_.active_window();
    if (active == nullptr)
        return false;

    for (const auto &editor : text_editors_)
    {
        if (editor->window == active)
        {
            const auto st = editor->window->save();
            if (st != ckv::widgets::EditorFileStatus::Ok)
                show_message(ckv::widgets::MessageBoxKind::Error, "Save file", editor_status_message(st));
            return true;
        }
    }
    for (const auto &hex : hex_editors_)
    {
        if (hex->window == active)
        {
            const std::string payload(reinterpret_cast<const char *>(hex->model.bytes.data()), hex->model.bytes.size());
            const auto expectation = hex->fingerprint
                                         ? ckv::FileWriteExpectation::matching(*hex->fingerprint)
                                         : ckv::FileWriteExpectation::any();
            const auto written = files_.write_file_atomic(hex->path, payload, expectation);
            if (written.status != ckv::FileWriteStatus::Ok)
            {
                show_message(ckv::widgets::MessageBoxKind::Error, "Save hex", "Could not write '" + hex->path + "'.");
                return true;
            }
            hex->fingerprint = written.fingerprint;
            hex->original_bytes = hex->model.bytes;
            hex->modified = false;
            hex->window->set_title("Hex Edit: " + display_basename(hex->path));
            return true;
        }
    }
    for (const auto &jv : json_viewers_)
    {
        if (jv->window == active && jv->document != nullptr)
        {
            const std::string serialized = jv->document->dump(2) + "\n";
            const auto expectation = jv->fingerprint
                                         ? ckv::FileWriteExpectation::matching(*jv->fingerprint)
                                         : ckv::FileWriteExpectation::any();
            const auto written = files_.write_file_atomic(jv->path, serialized, expectation);
            if (written.status != ckv::FileWriteStatus::Ok)
            {
                show_message(ckv::widgets::MessageBoxKind::Error, "Save JSON", "Could not write '" + jv->path + "'.");
                return true;
            }
            jv->fingerprint = written.fingerprint;
            jv->modified = false;
            jv->window->set_title("JSON: " + display_basename(jv->path));
            return true;
        }
    }
    return false;
}

std::size_t FindWorkspaceViewers::open_editor_count() const noexcept
{
    return text_editors_.size();
}

std::size_t FindWorkspaceViewers::open_hex_editor_count() const noexcept
{
    return hex_editors_.size();
}

std::size_t FindWorkspaceViewers::open_json_viewer_count() const noexcept
{
    return json_viewers_.size();
}

} // namespace ck::vision
