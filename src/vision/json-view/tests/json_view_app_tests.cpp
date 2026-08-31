#include "json_view_app.hpp"

#include <iostream>
#include <string>

#include <cvision/core/clock.hpp>
#include <cvision/core/event.hpp>
#include <cvision/core/filesystem.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>

namespace
{

bool expect(bool condition, const char *message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

std::string deeply_nested_document(int depth)
{
    std::string document = "{\"caf\xC3\xA9\":";
    for (int index = 0; index < depth; ++index)
        document += "{\"level" + std::to_string(index) + "\":";
    document += "{\"needle\":\"\xF0\x9F\x8C\xB1\"}";
    for (int index = 0; index < depth; ++index)
        document += '}';
    document += '}';
    return document;
}

std::string wide_document(std::size_t entries)
{
    std::string document = "{";
    for (std::size_t index = 0; index < entries; ++index)
    {
        if (index != 0)
            document += ',';
        document += "\"entry" + std::to_string(index) + "\":{\"label\":\"record-" +
                    std::to_string(index) + "\"}";
    }
    document += '}';
    return document;
}

bool materialized_view_teardown_is_lifetime_safe()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ckv::MemoryFileSystem files;
    files.add_file("/teardown.json", R"({"nested":{"name":"ckVision"}})");

    {
        ck::vision::JsonViewApp json_view(application, files);
        if (!expect(json_view.load_file("/teardown.json") && json_view.tree() != nullptr &&
                        json_view.tree()->selected() != nullptr,
                    "the teardown fixture did not materialize its JSON tree"))
            return false;
        application.step(0);
    }

    application.step(0);
    return expect(application.current_frame().size() == ckv::Size{100, 30},
                  "destroying a JSON tree view left stale ckVision chrome behind");
}

} // namespace

int main()
{
    if (!materialized_view_teardown_is_lifetime_safe())
        return 1;

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ckv::MemoryFileSystem files;
    files.add_file("/fixture.json",
                   R"({"name":"ckVision","nested":{"name":"ckUtilities"},"enabled":true})");
    ck::vision::JsonViewApp json_view(application, files);

    if (!expect(json_view.load_file("/fixture.json"),
                "JSON document did not load"))
        return 1;
    if (!expect(json_view.tree() != nullptr && json_view.tree()->selected() != nullptr,
                "JSON tree was not materialized from its application-owned document"))
        return 1;
    if (!expect(json_view.find("ckutilities", false, true), "value search did not find a result"))
        return 1;
    if (!expect(json_view.selected_json_node() != nullptr &&
                    json_view.selected_json_node()->key == "name",
                "search did not reveal and select its matching node"))
        return 1;
    if (!expect(application.execute_command(json_view.level_command(0)), "level command was unavailable"))
        return 1;
    if (!expect(json_view.tree() != nullptr && json_view.tree()->selected() != nullptr,
                "tree projection was lost after an expansion-level change"))
        return 1;
    if (!expect(json_view.find("ckutilities", false, true), "search did not recover after an expansion-level change"))
        return 1;
    if (!expect(application.execute_command(json_view.copy_command()), "copy command was unavailable"))
        return 1;
    if (!expect(application.clipboard_text() == "\"ckUtilities\"", "copy command did not export selected JSON"))
        return 1;
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}});
    application.step(0);

    // The native view must accept UTF-8 content and retain its document while
    // reporting a malformed replacement.  A moderately deep document is an
    // inexpensive regression guard for the tree materialization path used by
    // real configuration exports.
    const std::string deep_document = deeply_nested_document(64);
    if (!expect(json_view.load_document("deep-utf8.json", deep_document),
                "the JSON viewer did not load a deep UTF-8 document"))
        return 1;
    const std::string unicode_leaf = "\xF0\x9F\x8C\xB1";
    if (!expect(json_view.find(unicode_leaf, false, true),
                "value search did not find a UTF-8 JSON value"))
        return 1;
    if (!expect(json_view.selected_json_node() != nullptr && json_view.selected_json_node()->key == "needle",
                "UTF-8 search did not reveal the deep matching node"))
        return 1;
    terminal.resize(ckv::Size{42, 12});
    application.step(0);
    if (!expect(application.current_frame().size() == ckv::Size{42, 12},
                "the JSON viewer did not recompose at a narrow terminal size"))
        return 1;
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Up, ckv::Modifier::None, ""}});
    application.step(0);
    if (!expect(json_view.selected_json_node() != nullptr && json_view.selected_json_node()->key != "needle",
                "the narrowed JSON tree did not retain keyboard navigation"))
        return 1;
    const std::string keyboard_selected_key = json_view.selected_json_node()->key;
    const ckv::Rect tree_bounds = json_view.tree()->absolute_bounds();
    terminal.inject_event(ckv::MouseEvent{
        .action = ckv::MouseAction::Down,
        .button = ckv::MouseButton::Left,
        .cell = ckv::Point{tree_bounds.x + 3, tree_bounds.y},
    });
    application.step(0);
    if (!expect(json_view.selected_json_node() != nullptr &&
                    json_view.selected_json_node()->key != keyboard_selected_key,
                "the narrowed JSON tree did not retain mouse selection"))
        return 1;
    terminal.resize(ckv::Size{100, 30});
    application.step(0);
    if (!expect(!json_view.load_document("malformed.json", "{\"unterminated\":"),
                "the JSON viewer accepted malformed JSON"))
        return 1;
    if (!expect(json_view.tree() != nullptr && json_view.find(unicode_leaf, false, true),
                "a malformed reload discarded the previously open document"))
        return 1;
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}});
    application.step(0);
    files.add_file("/refresh.json", R"({"settings":{"version":"before"},"obsolete":true})");
    if (!expect(json_view.load_file("/refresh.json") && json_view.find("before", false, true),
                "the JSON reload fixture did not select its original value"))
        return 1;
    const std::uint64_t retained_selection_id = json_view.tree()->selected()->id;
    auto *const retained_tree = json_view.tree();
    if (!expect(retained_selection_id != 0,
                "the JSON reload fixture did not expose a stable tree selection"))
        return 1;
    files.add_file("/refresh.json", R"({"settings":{"version":"after"},"introduced":true})");
    if (!expect(application.execute_command(json_view.reload_command()),
                "reload command was unavailable"))
        return 1;
    if (!expect(json_view.tree() != retained_tree &&
                    json_view.tree()->selected() != nullptr &&
                    json_view.tree()->selected()->id == retained_selection_id &&
                    json_view.selected_json_node() != nullptr &&
                    reconstructJson(json_view.selected_json_node()) == json("after"),
                "JSON reload did not retain a surviving materialized-tree selection"))
        return 1;
    files.add_file("/refresh.json", "{\"settings\":");
    if (!expect(application.execute_command(json_view.reload_command()),
                "malformed reload command was unavailable"))
        return 1;
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}});
    application.step(0);
    if (!expect(json_view.tree()->selected() != nullptr &&
                    json_view.tree()->selected()->id == retained_selection_id &&
                    json_view.find("after", false, true),
                "a malformed refresh discarded the previous JSON snapshot"))
        return 1;
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}});
    application.step(0);
    if (!expect(json_view.load_document("wide.json", wide_document(2048)),
                "the JSON viewer did not load a 2,048-entry document"))
        return 1;
    if (!expect(json_view.find("record-2047", false, true) &&
                    json_view.selected_json_node() != nullptr &&
                    json_view.selected_json_node()->key == "label",
                "large-document search did not reveal the final record"))
        return 1;
    application.step(0);
    const auto terminal_cells = static_cast<std::size_t>(terminal.size().width) *
                                static_cast<std::size_t>(terminal.size().height);
    if (!expect(application.last_compose_cells_touched() <= terminal_cells,
                "large-document navigation exceeded the visible-frame composition budget"))
        return 1;
    constexpr std::size_t scale_entries = 12000;
    files.add_file("/scale.json", wide_document(scale_entries));
    if (!expect(json_view.load_file("/scale.json") &&
                    json_view.tree_node_count() == 1 + (scale_entries * 2) &&
                    json_view.find("record-11999", false, true),
                "the JSON viewer did not materialize an application-scale tree snapshot"))
        return 1;
    application.step(0);
    if (!expect(application.last_compose_cells_touched() <= terminal_cells,
                "application-scale JSON search exceeded the visible-frame composition budget"))
        return 1;
    files.add_file("/scale.json", R"({"status":"refreshed"})");
    if (!expect(application.execute_command(json_view.reload_command()) &&
                    json_view.tree_node_count() == 2 &&
                    json_view.find("refreshed", false, true) &&
                    json_view.selected_json_node() != nullptr &&
                    json_view.selected_json_node()->key == "status",
                "JSON refresh retained indexes from an obsolete large snapshot"))
        return 1;
    application.step(0);
    if (!expect(application.last_compose_cells_touched() <= terminal_cells,
                "small JSON refresh exceeded the visible-frame composition budget"))
        return 1;
    if (!expect(application.execute_command(json_view.close_command()), "close command was unavailable"))
        return 1;
    if (!expect(json_view.tree() == nullptr && json_view.selected_json_node() == nullptr,
                "closing the document did not clear its native tree state"))
        return 1;

    application.step(0);
    return expect(application.current_frame().size() == ckv::Size{100, 30},
                  "native JSON viewer did not render a full frame")
               ? 0
               : 1;
}
