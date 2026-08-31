// Produces the screenshots embedded by the user-facing guides. The capture
// fixtures exercise the actual ckUtilities views rather than illustrative
// mockups, following ckVision's own HeadlessTerminal-to-SVG documentation
// pipeline.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <cvision/core/clock.hpp>
#include <cvision/core/event.hpp>
#include <cvision/core/filesystem.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>

#include "chat_app.hpp"
#include "chat_options.hpp"
#include "chat_startup_options.hpp"
#include "config_app.hpp"
#include "disk_usage_app.hpp"
#include "disk_usage_options.hpp"
#include "edit_app.hpp"
#include "find_app.hpp"
#include "frame_svg.hpp"
#include "json_view_app.hpp"
#include "launcher_app.hpp"

namespace
{

void write_svg(const std::filesystem::path &directory, const std::string &name,
               const ckv::term::VirtualDisplay &display)
{
    const std::filesystem::path path = directory / (name + ".svg");
    std::ofstream output(path, std::ios::binary);
    output << ckv::docgen::render_virtual_display_svg(display);
    if (!output)
        throw std::runtime_error("could not write " + path.string());
    std::cerr << "wrote " << path << " (" << display.size().width << "x" << display.size().height
              << " cells)\n";
}

class DocumentationFindStore final : public ck::vision::FindSpecificationStore
{
public:
    std::vector<ck::find::SavedSpecification> list() const override { return {}; }
    std::optional<ck::find::SearchSpecification> load(const std::string &) const override { return std::nullopt; }
    bool save(const ck::find::SearchSpecification &, const std::string &) override { return true; }
};

class DocumentationFindExecutionService final : public ck::vision::FindExecutionService
{
public:
    void start(ck::find::SearchSpecification,
               bool,
               CompletionHandler on_complete) override
    {
        if (on_complete)
        {
            ck::find::SearchExecutionResult result;
            result.matchCount = 3;
            result.matches = {"/workspace/src/vision/find/find_app.cpp",
                              "/workspace/docs/tools/ck-find.md",
                              "/workspace/tests/find_app_tests.cpp"};
            on_complete(std::move(result));
        }
    }

    ck::vision::FindCustomCommandCapability custom_command_capability(
        const ck::find::SearchSpecification &) const override
    {
        return {};
    }

    void cancel() noexcept override {}
    bool running() const noexcept override { return false; }
};

class DocumentationConfigPersistence final : public ck::vision::ConfigPersistence
{
public:
    bool load(ck::config::OptionRegistry &) override { return true; }
    bool save(const ck::config::OptionRegistry &) override { return true; }
    bool import_from(ck::config::OptionRegistry &, const std::string &) override { return true; }
    bool export_to(const ck::config::OptionRegistry &, const std::string &) override { return true; }
};

class DocumentationResponseService final : public ck::vision::ChatResponseService
{
public:
    void start(ck::vision::ChatResponseRequest,
               ChunkHandler on_chunk,
               CompletionHandler on_complete) override
    {
        running_ = true;
        on_chunk_ = std::move(on_chunk);
        on_complete_ = std::move(on_complete);
    }

    void cancel() noexcept override { running_ = false; }
    bool running() const noexcept override { return running_; }

    void emit(std::string text)
    {
        if (on_chunk_)
            on_chunk_(std::move(text));
    }

    void complete()
    {
        running_ = false;
        if (on_complete_)
            on_complete_({});
    }

private:
    bool running_ = false;
    ChunkHandler on_chunk_;
    CompletionHandler on_complete_;
};

class DocumentationTranscriptStore final : public ck::vision::ChatTranscriptStore
{
public:
    bool write(const std::filesystem::path &, const std::string &) override { return true; }
};

class DocumentationSelectionPersistence final : public ck::vision::ChatSelectionPersistence
{
public:
    bool save_active_model(std::string_view) override { return true; }
    bool save_active_prompt(std::string_view) override { return true; }
};

class DocumentationPromptService final : public ck::vision::ChatPromptService
{
public:
    DocumentationPromptService()
        : prompts_{{"guide", "Documentation guide",
                    "Help people use CK Utilities confidently and concisely.", true, true}}
    {
    }

    std::vector<ck::vision::ChatSystemPrompt> prompts() const override { return prompts_; }

    std::optional<ck::vision::ChatSystemPrompt> active_prompt() const override
    {
        const auto found = std::find_if(prompts_.begin(), prompts_.end(), [](const auto &prompt) {
            return prompt.is_active;
        });
        return found == prompts_.end() ? std::nullopt
                                      : std::optional<ck::vision::ChatSystemPrompt>(*found);
    }

    bool add_or_update(ck::vision::ChatSystemPrompt prompt) override
    {
        if (prompt.id.empty() || prompt.name.empty() || prompt.message.empty())
            return false;
        prompts_ = {std::move(prompt)};
        prompts_.front().is_active = true;
        return true;
    }

    bool remove(std::string_view) override { return false; }

    bool activate(std::string_view id) override
    {
        if (prompts_.empty() || prompts_.front().id != id)
            return false;
        prompts_.front().is_active = true;
        return true;
    }

    bool restore_default(std::string_view id) override { return activate(id); }
    bool is_default_modified(std::string_view) const override { return false; }

private:
    std::vector<ck::vision::ChatSystemPrompt> prompts_;
};

class DocumentationModelService final : public ck::vision::ChatModelService
{
public:
    DocumentationModelService()
        : model_{.id = "guide-assistant",
                 .name = "Guide assistant",
                 .description = "A local model selected for the documentation capture.",
                 .hardware_requirements = "CPU",
                 .size_bytes = 512 * 1024 * 1024,
                 .local_path = "/models/guide-assistant.gguf",
                 .context_window_tokens = 4096,
                 .max_output_tokens = 512,
                 .is_downloaded = true,
                 .is_active = true}
    {
    }

    std::vector<ck::vision::ChatModel> available_models() const override { return {model_}; }
    std::vector<ck::vision::ChatModel> downloaded_models() const override { return {model_}; }
    std::optional<ck::vision::ChatModel> active_model() const override { return model_; }
    bool activate(std::string_view id) override { return id == model_.id; }
    bool deactivate(std::string_view) override { return false; }
    bool remove(std::string_view) override { return false; }
    bool start_download(std::string_view, DownloadProgressHandler, DownloadCompletionHandler) override { return false; }
    void cancel_download() noexcept override {}
    bool download_running() const noexcept override { return false; }

private:
    ck::vision::ChatModel model_;
};

void capture_launcher(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ck::vision::UtilitiesLauncherApp launcher(application, {});
    application.step(0);
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Down, ckv::Modifier::None, ""}});
    application.step(0);
    write_svg(directory, "ck-utilities-launcher", terminal.display());
}

void capture_json_view(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ckv::MemoryFileSystem files;
    files.add_file("/workspace/release-notes.json",
                   R"({"product":"CK Utilities","framework":"ckVision 0.1.3","tools":["JSON View","Find","Disk Usage","Markdown Editor","Chat"],"release":{"platforms":["macOS","Linux"],"packages":["archive","deb","rpm"],"status":"ready"}})");
    ck::vision::JsonViewApp json_view(application, files);
    if (!json_view.load_file("/workspace/release-notes.json"))
        throw std::runtime_error("could not load JSON capture fixture");
    application.execute_command(json_view.level_command(2));
    application.step(0);
    write_svg(directory, "ck-json-view", terminal.display());
}

void capture_find(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    DocumentationFindStore specifications;
    DocumentationFindExecutionService execution;
    ck::vision::FindApp find(application, specifications, execution);
    if (!application.execute_command(find.execute_command()))
        throw std::runtime_error("could not start the Find capture search");
    application.step(0);
    application.step(0);
    write_svg(directory, "ck-find", terminal.display());
}

ck::du::BuildDirectoryTreeResult documentation_disk_usage_snapshot()
{
    ck::du::BuildDirectoryTreeResult snapshot;
    snapshot.root = std::make_unique<ck::du::DirectoryNode>();
    snapshot.root->path = "/workspace";
    snapshot.root->stats = {.totalSize = 25 * 1024 * 1024,
                            .fileCount = 128,
                            .directoryCount = 3};
    snapshot.root->expanded = true;

    auto source = std::make_unique<ck::du::DirectoryNode>();
    source->path = "/workspace/src";
    source->parent = snapshot.root.get();
    source->stats = {.totalSize = 13 * 1024 * 1024,
                     .fileCount = 68,
                     .directoryCount = 1};
    source->expanded = true;
    auto vision = std::make_unique<ck::du::DirectoryNode>();
    vision->path = "/workspace/src/vision";
    vision->parent = source.get();
    vision->stats = {.totalSize = 5 * 1024 * 1024,
                     .fileCount = 24,
                     .directoryCount = 0};
    source->children.push_back(std::move(vision));
    snapshot.root->children.push_back(std::move(source));

    auto docs = std::make_unique<ck::du::DirectoryNode>();
    docs->path = "/workspace/docs";
    docs->parent = snapshot.root.get();
    docs->stats = {.totalSize = 7 * 1024 * 1024,
                   .fileCount = 42,
                   .directoryCount = 0};
    snapshot.root->children.push_back(std::move(docs));

    auto tests = std::make_unique<ck::du::DirectoryNode>();
    tests->path = "/workspace/tests";
    tests->parent = snapshot.root.get();
    tests->stats = {.totalSize = 5 * 1024 * 1024,
                    .fileCount = 18,
                    .directoryCount = 0};
    snapshot.root->children.push_back(std::move(tests));
    return snapshot;
}

void capture_disk_usage(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ck::vision::DiskUsageApp disk_usage(application, documentation_disk_usage_snapshot());
    application.step(0);
    write_svg(directory, "ck-du", terminal.display());
}

void capture_config(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ck::config::OptionRegistry disk_usage_registry("ck-du");
    ck::du::registerDiskUsageOptions(disk_usage_registry);
    disk_usage_registry.loadDefaults();
    ck::config::OptionRegistry chat_registry("ck-chat");
    ck::chat::registerChatOptions(chat_registry);
    chat_registry.loadDefaults();
    DocumentationConfigPersistence persistence;
    ck::vision::ConfigApp config(application,
                                 {{"ck-du", "Disk Usage", &disk_usage_registry, &persistence},
                                  {"ck-chat", "Chat", &chat_registry, &persistence}});
    if (!config.select_application("ck-chat"))
        throw std::runtime_error("could not select Chat options for the configuration capture");
    application.step(0);
    write_svg(directory, "ck-config", terminal.display());
}

void capture_markdown_editor(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ckv::MemoryFileSystem files;
    files.add_file("/workspace/notes.md",
                   "# CK Utilities\n\n"
                   "A collection of focused terminal tools built with ckVision.\n\n"
                   "## Release checklist\n\n"
                   "- [x] Browse JSON\n"
                   "- [x] Search files\n"
                   "- [x] Inspect disk usage\n"
                   "- [x] Edit Markdown\n");
    ck::vision::EditApp editor(application, files);
    if (!editor.open_file("/workspace/notes.md"))
        throw std::runtime_error("could not load Markdown capture fixture");
    application.step(0);
    write_svg(directory, "ck-edit", terminal.display());
}

void capture_chat(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    DocumentationResponseService responses;
    DocumentationTranscriptStore transcripts;
    DocumentationPromptService prompts;
    DocumentationModelService models;
    DocumentationSelectionPersistence selections;
    ck::vision::ChatApp chat(application, responses, transcripts, prompts, models, selections, {});
    if (!chat.submit_prompt("How do I start with CK Utilities?"))
        throw std::runtime_error("could not start the Chat capture conversation");
    responses.emit("Open the launcher, choose a tool, and use its keyboard-first menus to explore.");
    responses.complete();
    application.step(0);
    application.step(0);
    write_svg(directory, "ck-chat", terminal.display());
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "capture_ckutilities_screenshots")
                  << " <output-directory>\n";
        return 1;
    }

#if defined(_WIN32)
    _putenv_s("CKTOOLS_DOCUMENTATION_CAPTURE", "1");
#else
    setenv("CKTOOLS_DOCUMENTATION_CAPTURE", "1", 1);
#endif
    const std::filesystem::path output_directory = argv[1];
    std::filesystem::create_directories(output_directory);
    try
    {
        capture_launcher(output_directory);
        capture_json_view(output_directory);
        capture_find(output_directory);
        capture_disk_usage(output_directory);
        capture_config(output_directory);
        capture_markdown_editor(output_directory);
        capture_chat(output_directory);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
