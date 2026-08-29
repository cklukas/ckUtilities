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

} // namespace

int main()
{
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
    if (!expect(json_view.tree() != nullptr, "JSON tree was not materialized"))
        return 1;
    if (!expect(json_view.find("ckutilities", false, true), "value search did not find a result"))
        return 1;
    if (!expect(json_view.selected_json_node() != nullptr &&
                    json_view.selected_json_node()->key == "name",
                "search did not reveal and select its matching node"))
        return 1;
    if (!expect(application.execute_command(json_view.level_command(0)), "level command was unavailable"))
        return 1;
    if (!expect(json_view.tree() != nullptr, "tree was lost after an expansion-level change"))
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
