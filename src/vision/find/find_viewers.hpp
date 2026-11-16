// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cvision/core/filesystem.hpp>
#include <cvision/ui/application.hpp>
#include <cvision/widgets/desktop.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/editor_window.hpp>
#include <cvision/widgets/file_dialog.hpp>
#include <cvision/widgets/message_box.hpp>
#include <cvision/widgets/syntax_profile.hpp>
#include <cvision/widgets/window.hpp>

namespace ck::vision
{

enum class FindPreviewMode
{
    Auto,
    Text,
    Json,
    Hex,
};

// Owns and coordinates modeless viewer and editor windows opened from Found
// Files lists: Text/Hex preview, structured JSON tree view/edit, ckVision
// EditorWindow (text/code/Markdown/JSON), and virtualized 16-column Hex editor.
class FindWorkspaceViewers
{
public:
    FindWorkspaceViewers(ckv::ui::Application &application,
                         ckv::widgets::Desktop &desktop,
                         ckv::FileSystem &files);
    ~FindWorkspaceViewers();

    FindWorkspaceViewers(const FindWorkspaceViewers &) = delete;
    FindWorkspaceViewers &operator=(const FindWorkspaceViewers &) = delete;

    // Builds a fast bounded inline preview (first 4 KiB) for a Found Files
    // split pane without blocking or mutating state.
    std::string inline_preview_for(const std::string &path) const;

    bool open_preview(const std::string &path, FindPreviewMode mode = FindPreviewMode::Auto);
    bool open_text_editor(const std::string &path);
    bool open_hex_editor(const std::string &path);

    // If the currently active desktop window is an editor managed here (Text,
    // Hex, or JSON), saves it and returns true; otherwise returns false so the
    // caller can run its own Ctrl+S handler.
    bool save_active_document();

    std::size_t open_editor_count() const noexcept;
    std::size_t open_hex_editor_count() const noexcept;
    std::size_t open_json_viewer_count() const noexcept;

private:
    struct TextEditorSession;
    struct HexEditorSession;
    struct JsonViewerSession;

    bool open_text_or_hex_preview(const std::string &path, bool force_hex);
    bool open_json_viewer(const std::string &path);
    ckv::Rect next_document_bounds() noexcept;
    void show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message);

    ckv::ui::Application &application_;
    ckv::widgets::Desktop &desktop_;
    ckv::FileSystem &files_;
    ckv::widgets::SyntaxProfileRegistry syntax_profiles_;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    int cascade_step_ = 0;

    std::vector<std::unique_ptr<TextEditorSession>> text_editors_;
    std::vector<std::unique_ptr<HexEditorSession>> hex_editors_;
    std::vector<std::unique_ptr<JsonViewerSession>> json_viewers_;

    std::optional<ckv::widgets::MessageBoxPresentation> message_box_;
    std::optional<ckv::widgets::MessageBoxPresentation> close_confirmation_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> edit_node_dialog_;
    std::optional<ckv::widgets::FileDialogPresentation> save_as_dialog_;
};

} // namespace ck::vision
