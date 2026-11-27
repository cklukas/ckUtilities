// Produces the screenshots embedded by the user-facing guides. The capture
// fixtures exercise the actual ckUtilities views rather than illustrative
// mockups, following ckVision's own HeadlessTerminal-to-SVG documentation
// pipeline.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <cvision/core/clock.hpp>
#include <cvision/core/event.hpp>
#include <cvision/core/filesystem.hpp>
#include <cvision/core/version.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>

#include "chat_app.hpp"
#include "chat_options.hpp"
#include "chat_startup_options.hpp"
#include "config_app.hpp"
#include "disk_usage_app.hpp"
#include "disk_usage_options.hpp"
#include "edit_app.hpp"
#include "files_app.hpp"
#include "files_ordinary_archive.hpp"
#include "find_app.hpp"
#include "frame_svg.hpp"
#include "json_view_app.hpp"
#include "launcher_app.hpp"
#include "native_filesystem.hpp"
#include "simulated_filesystem.hpp"
#include "svg_align.hpp"

namespace
{

void write_svg(const std::filesystem::path &directory, const std::string &name,
               const ckv::term::VirtualDisplay &display,
               std::string_view replace_from = {}, std::string_view replace_to = {})
{
    const std::filesystem::path path = directory / (name + ".svg");
    std::string svg = ck::docgen::align_svg_text_runs(ckv::docgen::render_virtual_display_svg(display));
    if (!replace_from.empty())
    {
        std::size_t pos = 0;
        while ((pos = svg.find(replace_from, pos)) != std::string::npos)
        {
            svg.replace(pos, replace_from.size(), replace_to);
            pos += replace_to.size();
        }
        // Also sanitize a truncated prefix of replace_from clipped by a window border.
        for (std::size_t len = replace_from.size() - 1; len >= 6; --len)
        {
            const std::string needle = ">" + std::string(replace_from.substr(0, len)) + "</text>";
            const std::string replacement =
                ">" + std::string(replace_to.substr(0, std::min(len, replace_to.size()))) + "</text>";
            pos = 0;
            while ((pos = svg.find(needle, pos)) != std::string::npos)
            {
                svg.replace(pos, needle.size(), replacement);
                pos += replacement.size();
            }
        }
    }
    std::ofstream output(path, std::ios::binary);
    output << svg;
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
            result.scannedEntries = 412;
            result.scannedDirectories = 28;
            result.matchCount = 3;
            result.matches = {"/workspace/docs/tools/ck-find.md",
                              "/workspace/README.md",
                              "/workspace/src/vision/find/find_app.cpp"};
            result.matchEntries = {
                {.path = "/workspace/docs/tools/ck-find.md",
                 .sizeBytes = 2480,
                 .modifiedUnixSeconds = 1763280000,
                 .sizeKnown = true,
                 .modifiedKnown = true},
                {.path = "/workspace/README.md",
                 .sizeBytes = 5708,
                 .modifiedUnixSeconds = 1763280000,
                 .sizeKnown = true,
                 .modifiedKnown = true},
                {.path = "/workspace/src/vision/find/find_app.cpp",
                 .sizeBytes = 41280,
                 .modifiedUnixSeconds = 1763280000,
                 .sizeKnown = true,
                 .modifiedKnown = true},
            };
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

class DocumentationIndexFile final : public ck::files::IndexByteFile
{
public:
    ck::files::Observation<std::uint64_t> size() override
    {
        return ck::files::Observation<std::uint64_t>::available(bytes_.size());
    }
    ck::files::Observation<std::size_t> write_at(std::uint64_t offset,
                                                  std::span<const std::byte> bytes) override
    {
        if (offset > bytes_.size())
            return ck::files::Observation<std::size_t>::unavailable(
                ck::files::ObservationState::Failed,
                {ck::files::ErrorDomain::Adapter, 1, "documentation.index.write_offset"});
        const auto start = static_cast<std::size_t>(offset);
        if (bytes.size() > std::numeric_limits<std::size_t>::max() - start)
            return ck::files::Observation<std::size_t>::unavailable(
                ck::files::ObservationState::Failed,
                {ck::files::ErrorDomain::Adapter, 2, "documentation.index.write_overflow"});
        const auto end = start + bytes.size();
        if (end > bytes_.size())
            bytes_.resize(end);
        std::copy(bytes.begin(), bytes.end(), bytes_.begin() + start);
        return ck::files::Observation<std::size_t>::available(bytes.size());
    }
    ck::files::Observation<std::size_t> read_at(std::uint64_t offset,
                                                 std::span<std::byte> bytes) override
    {
        if (offset > bytes_.size())
            return ck::files::Observation<std::size_t>::unavailable(
                ck::files::ObservationState::Gone,
                {ck::files::ErrorDomain::Adapter, 3, "documentation.index.read_offset"});
        const auto start = static_cast<std::size_t>(offset);
        if (bytes.size() > bytes_.size() - start)
            return ck::files::Observation<std::size_t>::unavailable(
                ck::files::ObservationState::Gone,
                {ck::files::ErrorDomain::Adapter, 4, "documentation.index.read_range"});
        std::copy(bytes_.begin() + start, bytes_.begin() + start + bytes.size(), bytes.begin());
        return ck::files::Observation<std::size_t>::available(bytes.size());
    }
private:
    std::vector<std::byte> bytes_;
};

class DocumentationIndexFactory final : public ck::files::IndexFileFactory
{
public:
    ck::files::Observation<std::unique_ptr<ck::files::IndexByteFile>> create_file() override
    {
        return ck::files::Observation<std::unique_ptr<ck::files::IndexByteFile>>::available(
            std::make_unique<DocumentationIndexFile>());
    }
};

std::unique_ptr<ck::files::BasicFileSystem> documentation_filesystem()
{
    using FileSystem = ck::testing::SimulatedFileSystem;
    using Kind = ck::files::ObjectKind;
    using Name = ck::files::NativeName;
    auto files = std::make_unique<FileSystem>();
    const auto root = files->make_node(Kind::Directory);
    const auto left = files->make_node(Kind::Directory);
    const auto right = files->make_node(Kind::Directory);
    files->bind("/capture", root);
    files->bind("/", root);
    files->bind("/capture/left", left);
    files->bind("/capture/right", right);
    files->put(root, *Name::from_posix("left"), left);
    files->put(root, *Name::from_posix("right"), right);
    const auto add = [&](const FileSystem::Pointer &parent, std::string name,
                         Kind kind, std::uint64_t size = 0) {
        const auto child = files->make_node(kind);
        if (kind == Kind::RegularFile)
            files->metadata(child).logical_size = ck::files::Observation<std::uint64_t>::available(size);
        files->put(parent, *Name::from_posix(name), child);
    };
    add(left, "Documents", Kind::Directory);
    add(left, "Downloads", Kind::Directory);
    add(left, "README.md", Kind::RegularFile, 1842);
    add(left, "release-notes.json", Kind::RegularFile, 3276);
    add(left, "report.csv", Kind::RegularFile, 9812);
    add(right, "Archive", Kind::Directory);
    add(right, "Incoming", Kind::Directory);
    add(right, "README.md", Kind::RegularFile, 1842);
    add(right, "space-report.txt", Kind::RegularFile, 420);
    return files;
}

ck::files::NativeAbsolutePath documentation_path(std::string_view path)
{
    auto value = ck::files::NativeAbsolutePath::from_posix(path);
    if (!value.value())
        throw std::runtime_error("invalid documentation Files path");
    return std::move(value).take_value();
}

ck::vision::FilesPaneBackend documentation_backend()
{
    ck::vision::FilesPaneBackend backend;
    backend.filesystem = [] { return documentation_filesystem(); };
    backend.workspace = [] {
        return ck::files::Observation<std::shared_ptr<ck::files::IndexFileFactory>>::available(
            std::make_shared<DocumentationIndexFactory>());
    };
    backend.storage_model = ck::files::StorageModel::LinuxStatx;
    backend.home = [](ck::files::NativePathLimits, std::stop_token) {
        return ck::files::Observation<ck::files::NativeAbsolutePath>::available(
            documentation_path("/capture/right"));
    };
    return backend;
}

void capture_files(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ck::vision::FilesAppWake wake(application);
    auto left_service = std::make_unique<ck::vision::FilesPaneService>(
        documentation_backend(), ck::vision::FilesPaneServiceLimits{}, wake.callback());
    auto right_service = std::make_unique<ck::vision::FilesPaneService>(
        documentation_backend(), ck::vision::FilesPaneServiceLimits{}, wake.callback());
    auto planning_service = std::make_unique<ck::vision::FilesOperationPlanningService>(wake.callback());
    auto execution_service = std::make_unique<ck::vision::FilesOperationExecutionService>(wake.callback());
    auto journal_root = std::make_shared<const ck::files::NativeAbsolutePath>(
        documentation_path("/capture/journals"));
    auto volume_service = std::make_unique<ck::vision::FilesVolumeService>(wake.callback(), [] {
        std::vector<ck::vision::FilesVolumeRecord> records;
        records.push_back({"Workspace", "/capture", "simulatedfs", 20ull * 1024 * 1024 * 1024,
                           12ull * 1024 * 1024 * 1024, false, "documentation-workspace", {
                               ck::vision::FilesVolumeArchiveSupport::Unknown, false,
                               "documentation capability probe pending"}});
        records.push_back({"Archive", "/archive", "simulatedfs", 500ull * 1024 * 1024 * 1024,
                           214ull * 1024 * 1024 * 1024, true, "documentation-archive", {
                               ck::vision::FilesVolumeArchiveSupport::Unknown, false,
                               "documentation capability probe pending"}});
        return ck::files::Observation<std::vector<ck::vision::FilesVolumeRecord>>::available(std::move(records));
    });
    auto app = std::make_unique<ck::vision::FilesApp>(
        application,
        std::array<ck::vision::FilesPaneService *, 2>{left_service.get(), right_service.get()},
        wake,
        std::array<ck::files::NativeAbsolutePath, 2>{documentation_path("/capture/left"),
                                                     documentation_path("/capture/right")},
        [&application] { application.request_quit(); }, ck::vision::ThemeScheme::Classic,
        std::array<ck::vision::FilesCompletionService *, 2>{}, planning_service.get(), execution_service.get(),
        journal_root, ck::vision::FilesSettingsBinding{}, volume_service.get());
    const auto pump_until = [&](auto predicate) {
        for (int step = 0; step != 1000 && !predicate(); ++step)
        {
            application.step(clock.now_nanos());
            clock.advance(1'000'000);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        application.step(clock.now_nanos());
    };
    pump_until([&] { return !app->pending(0) && !app->pending(1); });
    // Select a regular file row (README.md) so Copy, Move, Trash, and Delete are all active
    // and ready in the main two-pane overview screenshot.
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Down, ckv::Modifier::None, ""}});
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Down, ckv::Modifier::None, ""}});
    application.step(clock.now_nanos());
    pump_until([&] { return !app->pending(0) && !app->pending(1); });
    if (app->status(0).find("unavailable") != std::string::npos)
        throw std::runtime_error("unexpected unavailable hint in Files capture: " + app->status(0));
    write_svg(directory, "ck-files", terminal.display());

    // Integrated recursive search dialog (Alt+F7).
    if (!app->open_search())
        throw std::runtime_error("could not open documentation Files search dialog");
    application.step(clock.now_nanos());
    terminal.inject_event(ckv::TextEvent{"*.json", false});
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Tab, ckv::Modifier::None, ""}});
    terminal.inject_event(ckv::TextEvent{"ck-files", false});
    application.step(clock.now_nanos());
    write_svg(directory, "ck-files-search", terminal.display());
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}});
    application.step(clock.now_nanos());

    if (!app->navigate(0, documentation_path("/")))
        throw std::runtime_error("could not navigate to the documentation Files root");
    pump_until([&] { return !app->pending(0); });
    if (!app->navigate_parent(0))
        throw std::runtime_error("could not request the documentation Files volume view");
    pump_until([&] { return !app->volume_info_pending(); });
    if (app->volume_info_pending() || !app->model(0).volume_cursor())
        throw std::runtime_error("documentation Files volume inventory did not finish");
    application.step(clock.now_nanos());
    write_svg(directory, "ck-files-volumes", terminal.display());
    const auto properties = application.commands().id_for("ck.files.properties");
    if (!properties || !application.execute_command(*properties) || !app->properties_pending())
        throw std::runtime_error("could not open documentation Files volume properties");
    application.step(clock.now_nanos());
    write_svg(directory, "ck-files-volume-properties", terminal.display());
    app.reset();
    left_service->join();
    right_service->join();
    planning_service->join();
    execution_service->join();
    volume_service->join();

    // Native fixture captures for real-filesystem parity tools:
    // Preview (F3), Embedded Editor (Ctrl+E), Compare & Sync, File Diff, and Archive Browser.
    struct TempDirGuard {
        std::filesystem::path root;
        ~TempDirGuard() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    };
    std::filesystem::path fixture_root = std::filesystem::temp_directory_path() / "ckw";
    if (const char *env_fixture = std::getenv("CKTOOLS_DOCGEN_FIXTURE_ROOT");
        env_fixture != nullptr && *env_fixture != '\0')
        fixture_root = env_fixture;
    {
        std::error_code ec;
        std::filesystem::remove_all(fixture_root, ec);
    }
    TempDirGuard guard{fixture_root};
    const auto left_dir = fixture_root / "left";
    const auto right_dir = fixture_root / "right";
    std::filesystem::create_directories(left_dir / "Documents");
    std::filesystem::create_directories(right_dir / "Archive");
    const auto write_text_file = [](const std::filesystem::path &path, std::string_view text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
        if (!out)
            throw std::runtime_error("could not write native fixture file: " + path.string());
    };
    write_text_file(
        left_dir / "Documents" / "notes.md",
        "# Project Notes\n\n- Verified copy/move preflight\n- Embedded editor & terminal\n");
    write_text_file(
        left_dir / "README.md",
        "# CK Utilities — ck-files\n\n"
        "Two-pane terminal file manager built on ckVision 2.1.\n\n"
        "## Highlights\n"
        "- Reviewed Copy (F5) and Move (F6) with conflict resolution\n"
        "- Integrated recursive search (Alt+F7) and directory tree (Alt+T)\n"
        "- Embedded syntax-aware editor (Ctrl+E) and terminal (Ctrl+O)\n"
        "- Native ZIP/TAR archive browsing, creation, and safe extraction\n");
    write_text_file(
        left_dir / "release-notes.json",
        "{\n"
        "  \"product\": \"CK Utilities\",\n"
        "  \"tool\": \"ck-files\",\n"
        "  \"version\": \"0.2.0\",\n"
        "  \"features\": [\n"
        "    \"reviewed-copy-move\",\n"
        "    \"recursive-search\",\n"
        "    \"embedded-editor\",\n"
        "    \"directory-sync\",\n"
        "    \"ordinary-archives\"\n"
        "  ],\n"
        "  \"platforms\": [\"macOS\", \"Linux\", \"Windows\"],\n"
        "  \"status\": \"ready\"\n"
        "}\n");
    write_text_file(
        right_dir / "README.md",
        "# CK Utilities — ck-files\n\n"
        "Two-pane terminal file manager built on ckVision 2.1.\n\n"
        "## Highlights\n"
        "- Reviewed Copy (F5) and Move (F6) with conflict resolution\n"
        "- Integrated recursive search (Alt+F7)\n"
        "- Bounded text and hexadecimal preview (F3)\n"
        "- Read-only volume and mount inspection\n");
    write_text_file(
        right_dir / "space-report.txt",
        "Workspace: 12.0 GiB free / 20.0 GiB total\nArchive:   214.0 GiB free / 500.0 GiB total\n");

    // Create a real ZIP archive in left/ so Browse Archive renders actual entries.
    const auto native_path = [](const std::filesystem::path &p) {
        auto obs = ck::files::NativeAbsolutePath::from_host(p);
        if (!obs.value())
            throw std::runtime_error("invalid native fixture path: " + p.string());
        return *obs.value();
    };
    const auto archive_path = native_path(left_dir / "release-bundle.zip");
    const std::vector<ck::vision::OrdinaryArchiveInputItem> archive_items = {
        {"README.md", native_path(left_dir / "README.md"), false},
        {"release-notes.json", native_path(left_dir / "release-notes.json"), false},
        {"Documents/notes.md", native_path(left_dir / "Documents" / "notes.md"), false},
    };
    const auto arc_res = ck::vision::create_ordinary_archive(
        archive_path, ck::vision::OrdinaryArchiveFormat::Zip, archive_items);
    if (!arc_res.ok)
        throw std::runtime_error("could not create documentation archive fixture: " + arc_res.error_message);

    const std::string sanitize_from = native_path(fixture_root).display_text();
    const std::string sanitize_to = "/workspace";

    auto make_native_backend = [&] {
        static const ck::files::AdapterScope docgen_native_scope;
        ck::vision::FilesPaneBackend backend;
        backend.filesystem = [] {
            return std::make_unique<ck::files::NativeFileSystem>(docgen_native_scope);
        };
        backend.workspace = [] {
            return ck::files::Observation<std::shared_ptr<ck::files::IndexFileFactory>>::available(
                std::make_shared<DocumentationIndexFactory>());
        };
        backend.storage_model = ck::files::StorageModel::LinuxStatx;
        backend.home = [right_p = native_path(right_dir)](ck::files::NativePathLimits, std::stop_token) {
            return ck::files::Observation<ck::files::NativeAbsolutePath>::available(right_p);
        };
        return backend;
    };

    auto capture_native_scene = [&](auto &&action) {
        ckv::ManualClock nclock;
        ckv::term::HeadlessTerminal nterm(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
        ckv::ui::Application napp_ui(nterm, nclock);
        ck::vision::FilesAppWake nwake(napp_ui);
        auto nleft = std::make_unique<ck::vision::FilesPaneService>(
            make_native_backend(), ck::vision::FilesPaneServiceLimits{}, nwake.callback());
        auto nright = std::make_unique<ck::vision::FilesPaneService>(
            make_native_backend(), ck::vision::FilesPaneServiceLimits{}, nwake.callback());
        auto nplan = std::make_unique<ck::vision::FilesOperationPlanningService>(nwake.callback());
        auto nexec = std::make_unique<ck::vision::FilesOperationExecutionService>(nwake.callback());
        std::filesystem::path journal_dir = fixture_root / "journals";
        if (const char *env_journal = std::getenv("CKTOOLS_DOCGEN_JOURNAL_ROOT");
            env_journal != nullptr && *env_journal != '\0')
            journal_dir = env_journal;
        auto njournal = std::make_shared<const ck::files::NativeAbsolutePath>(native_path(journal_dir));
        auto napp = std::make_unique<ck::vision::FilesApp>(
            napp_ui,
            std::array<ck::vision::FilesPaneService *, 2>{nleft.get(), nright.get()},
            nwake,
            std::array<ck::files::NativeAbsolutePath, 2>{native_path(left_dir), native_path(right_dir)},
            [&napp_ui] { napp_ui.request_quit(); }, ck::vision::ThemeScheme::Classic,
            std::array<ck::vision::FilesCompletionService *, 2>{}, nplan.get(), nexec.get(), njournal);
        const auto npump = [&](auto pred) {
            for (int step = 0; step != 1000 && !pred(); ++step)
            {
                napp_ui.step(nclock.now_nanos());
                nclock.advance(1'000'000);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            napp_ui.step(nclock.now_nanos());
        };
        npump([&] { return !napp->pending(0) && !napp->pending(1); });
        auto select_row_by_name = [&](std::size_t pane, std::string_view name) {
            if (napp->active_pane() != pane)
            {
                napp->switch_pane();
                napp_ui.step(nclock.now_nanos());
            }
            const auto *rows = napp->model(pane).visible_rows();
            if (!rows)
                throw std::runtime_error("no rows visible in native Files fixture");
            std::size_t target_idx = rows->size();
            for (std::size_t i = 0; i < rows->size(); ++i)
            {
                if ((*rows)[i].display_name == name)
                {
                    target_idx = i;
                    break;
                }
            }
            if (target_idx == rows->size())
                throw std::runtime_error("row not found in native Files fixture: " + std::string(name));
            for (std::size_t i = 0; i < target_idx; ++i)
            {
                nterm.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Down, ckv::Modifier::None, ""}});
                napp_ui.step(nclock.now_nanos());
            }
            npump([&] { return !napp->pending(0) && !napp->pending(1); });
        };
        action(napp_ui, nterm, *napp, npump, select_row_by_name);
        napp.reset();
        nleft->join();
        nright->join();
        nplan->join();
        nexec->join();
    };

    // 0a) Copy review dialog (README.md conflicts with right/README.md).
    capture_native_scene([&](auto &ui, auto &term, auto &fapp, auto &npump, auto &select_row) {
        select_row(0, "README.md");
        const auto copy_cmd = ui.commands().id_for("ck.files.copy");
        if (!copy_cmd || !ui.execute_command(*copy_cmd))
            throw std::runtime_error("could not start documentation Files copy review: " +
                                     fapp.availability("ck.files.copy").reason);
        npump([&] { return fapp.operation_review_pending(); });
        if (!fapp.operation_review_pending())
            throw std::runtime_error("documentation Files copy review did not open: " + fapp.status(0));
        write_svg(directory, "ck-files-copy-review", term.display(), sanitize_from, sanitize_to);
    });

    // 0b) Move review dialog (release-notes.json -> right/).
    capture_native_scene([&](auto &ui, auto &term, auto &fapp, auto &npump, auto &select_row) {
        select_row(0, "release-notes.json");
        const auto move_cmd = ui.commands().id_for("ck.files.move");
        if (!move_cmd || !ui.execute_command(*move_cmd))
            throw std::runtime_error("could not start documentation Files move review");
        npump([&] { return fapp.operation_review_pending(); });
        if (!fapp.operation_review_pending())
            throw std::runtime_error("documentation Files move review did not open: " + fapp.status(0));
        write_svg(directory, "ck-files-move-review", term.display(), sanitize_from, sanitize_to);
    });

    // 1) Bounded text preview (F3 on README.md).
    capture_native_scene([&](auto &ui, auto &term, auto &fapp, auto &npump, auto &select_row) {
        select_row(0, "README.md");
        const auto preview_cmd = ui.commands().id_for("ck.files.preview");
        if (!preview_cmd || !ui.execute_command(*preview_cmd))
            throw std::runtime_error("could not open documentation Files preview");
        npump([&] { return !fapp.pending(0) && fapp.preview_pending(); });
        write_svg(directory, "ck-files-preview", term.display(), sanitize_from, sanitize_to);
    });

    // 2) Embedded syntax-aware file editor (Ctrl+E on release-notes.json).
    capture_native_scene([&](auto &ui, auto &term, auto &fapp, auto &, auto &select_row) {
        select_row(0, "release-notes.json");
        if (!fapp.open_embedded_editor(false))
            throw std::runtime_error("could not open documentation Files embedded editor");
        ui.step(0);
        write_svg(directory, "ck-files-edit", term.display(), sanitize_from, sanitize_to);
    });

    // 3) Compare & reviewed directory synchronization preview.
    capture_native_scene([&](auto &ui, auto &term, auto &fapp, auto &, auto &) {
        if (!fapp.open_sync_directories())
            throw std::runtime_error("could not open documentation Files sync preview");
        ui.step(0);
        write_svg(directory, "ck-files-sync", term.display(), sanitize_from, sanitize_to);
    });

    // 4) File diff viewer (left/README.md vs right/README.md).
    capture_native_scene([&](auto &ui, auto &term, auto &fapp, auto &, auto &select_row) {
        select_row(1, "README.md");
        select_row(0, "README.md");
        if (!fapp.open_diff_files())
            throw std::runtime_error("could not open documentation Files diff viewer");
        ui.step(0);
        write_svg(directory, "ck-files-diff", term.display(), sanitize_from, sanitize_to);
    });

    // 5) Ordinary archive browser (release-bundle.zip).
    capture_native_scene([&](auto &ui, auto &term, auto &fapp, auto &, auto &select_row) {
        select_row(0, "release-bundle.zip");
        if (!fapp.open_archive_browse())
            throw std::runtime_error("could not open documentation Files archive browser");
        ui.step(0);
        write_svg(directory, "ck-files-archive", term.display(), sanitize_from, sanitize_to);
    });
}

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
    files.add_file(
        "/workspace/release-notes.json",
        std::string{R"({"product":"CK Utilities","framework":"ckVision )"} +
        std::string{ckv::version_string()} + R"(","tools":["ck-json-view","ck-find","ck-du","ck-files","ck-edit","ck-chat","ck-todo","ck-sysmon","ck-config"],"release":{"platforms":["macOS","Linux","Windows"],"packages":["tar.gz","deb","rpm","zip","homebrew"],"status":"ready"}})");
    ck::vision::JsonViewApp json_view(application, files);
    if (!json_view.load_file("/workspace/release-notes.json"))
        throw std::runtime_error("could not load JSON capture fixture");
    application.execute_command(json_view.level_command(3));
    application.step(0);
    write_svg(directory, "ck-json-view", terminal.display());
}

void capture_find(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ckv::MemoryFileSystem files;
    files.add_directory("/workspace");
    files.add_file("/workspace/docs/tools/ck-find.md",
                   "# ck-find\n\nInteractive file search with live progress, multiple Found Files windows,\n"
                   "exportable result lists, and integrated Text, JSON, and Hex viewers/editors.\n");
    files.add_file("/workspace/README.md", "# CK Utilities\n\nNative ckVision desktop utilities.\n");
    files.add_file("/workspace/src/vision/find/find_app.cpp", "// ck-find workspace implementation\n");
    DocumentationFindStore specifications;
    DocumentationFindExecutionService execution;
    ck::vision::FindApp find(application, specifications, execution, files, "/workspace");
    application.step(0);
    terminal.inject_event(ckv::TextEvent{"*find*", false});
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
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
    ck::config::OptionRegistry chat_registry("ck-chat");
    ck::chat::registerChatOptions(chat_registry);
    chat_registry.set("activeModelId", ck::config::OptionValue(std::string("guide-assistant")));
    chat_registry.set("activePromptId", ck::config::OptionValue(std::string("guide")));
    chat_registry.set("parseMarkdownLinks", ck::config::OptionValue(true));
    chat_registry.set("showAnalysis", ck::config::OptionValue(false));
    chat_registry.set("showThinking", ck::config::OptionValue(false));
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
                   "Focused terminal applications built on **ckVision 2.0**, with *keyboard-first*\n"
                   "menus, mouse support, and the [user guide](https://cklukas.github.io/ckUtilities/).\n"
                   "Start `ck-utilities` or launch any tool directly.\n\n"
                   "> Safe file workflows, shared shortcuts, and consistent themes across hosts.\n\n"
                   "## Suite tools\n\n"
                   "- [x] `ck-files` — Two-pane file manager with reviewed copy and move\n"
                   "- [x] `ck-todo` — Kanban boards with lanes, due dates, notes, and archives\n"
                   "- [x] `ck-sysmon` — CPU, memory, GPU, and Ollama model telemetry\n"
                   "- [x] `ck-du` — Asynchronous directory size browser\n"
                   "- [x] `ck-json-view` — Expandable JSON tree with incremental search\n"
                   "- [x] `ck-edit` — Markdown editor with tables, links, and reflow\n"
                   "- [x] `ck-find` — Guided file searches with filters and previews\n"
                   "- [x] `ck-chat` — Local GGUF model conversations and prompts\n"
                   "- [x] `ck-config` — Suite options and keyboard shortcut bindings\n\n"
                   "## Release packages\n\n"
                   "| Platform | Formats |\n"
                   "| --- | --- |\n"
                   "| macOS | `tar.gz`, Homebrew |\n"
                   "| Linux | `.deb`, `.rpm`, `tar.gz` |\n"
                   "| Windows | `.zip` |\n");
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
        capture_files(output_directory);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
