// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include "files_compare_sync_service.hpp"
#include "files_embedded_editor.hpp"
#include "files_native_io_helpers.hpp"
#include "files_ordinary_archive.hpp"
#include "files_remote_provider.hpp"
#include "files_search_service.hpp"
#include "files_terminal_session.hpp"
#include "files_volume_archive_restore.hpp"

#include "native_filesystem.hpp"
#include "sha256.hpp"

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

int g_failures = 0;

void check(bool cond, const char *expr, int line) {
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << expr << "\n";
        ++g_failures;
    }
}

#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)

class TestClock final : public ckv::Clock {
  public:
    std::int64_t now_nanos() const noexcept override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

struct TempWorkspace final {
    std::filesystem::path root;
    ck::files::NativeAbsolutePath native_root;

    explicit TempWorkspace(const std::string &tag)
        : root(std::filesystem::path(CKTOOLS_TEST_TEMP_ROOT) /
               ("parity_" + tag + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
          native_root([&] {
              std::filesystem::create_directories(root);
              auto obs = ck::files::NativeAbsolutePath::from_host(root);
              if (!obs.value())
                  throw std::runtime_error("failed creating native temp root");
              return *obs.value();
          }()) {}

    ~TempWorkspace() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    ck::files::NativeAbsolutePath child(const std::string &rel) const {
        auto obs = ck::files::NativeAbsolutePath::from_host(root / rel);
        if (!obs.value())
            throw std::runtime_error("invalid child path");
        return *obs.value();
    }

    void mkdir(const std::string &rel) const {
        std::filesystem::create_directories(root / rel);
    }

    void write_bytes(const std::string &rel, std::span<const std::byte> data) const {
        const auto p = root / rel;
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path());
        std::string err;
        auto np = ck::files::NativeAbsolutePath::from_host(p);
        if (!np.value() ||
            !ck::vision::detail::write_native_file_staged(
                *np.value(), data, true, ".ck-test-stage-", err, {}))
            throw std::runtime_error("write_bytes failed: " + err);
    }

    void write_text(const std::string &rel, std::string_view text) const {
        write_bytes(rel, std::span<const std::byte>(
                             reinterpret_cast<const std::byte *>(text.data()), text.size()));
    }

    std::string read_text(const std::string &rel) const {
        std::vector<std::byte> buf;
        std::uint64_t sz = 0;
        bool trunc = false;
        std::string err;
        if (!ck::vision::detail::read_native_file_bounded(
                child(rel), 64 * 1024 * 1024, buf, sz, trunc, err, {}))
            throw std::runtime_error("read_text failed: " + err);
        return std::string(reinterpret_cast<const char *>(buf.data()), buf.size());
    }
};

// ---- CP-07 Tests ----
void test_cp07_search_and_directory_tree() {
    TempWorkspace ws("cp07");
    ws.write_text("alpha.txt", "hello world\nneedle line here\nbye\n");
    ws.write_text("sub/beta.md", "markdown document with NEEDLE inside\n");
    ws.write_text("sub/.hidden.txt", "hidden needle\n");
    std::vector<std::byte> bin{std::byte{'n'}, std::byte{'e'}, std::byte{'e'},
                               std::byte{'d'}, std::byte{'l'}, std::byte{'e'},
                               std::byte{0}, std::byte{0xFF}};
    ws.write_bytes("sub/binary.bin", bin);

#if !defined(_WIN32)
    // Symlink loop pointing to parent directory must NOT be traversed!
    std::error_code ec;
    std::filesystem::create_directory_symlink(ws.root, ws.root / "sub/loop_link", ec);
#endif

    // 1. Glob + content search (case-insensitive, skip binary)
    ck::vision::FilesSearchQuery q;
    q.root = ws.native_root;
    q.name_pattern = "*.*";
    q.name_mode = ck::vision::FilesSearchNameMode::Glob;
    q.content_query = "needle";
    q.case_sensitive = false;
    q.skip_binary_content = true;
    q.include_hidden = false;
    const auto r1 = ck::vision::run_files_search(q);
    CHECK(r1.error_message.empty());
    CHECK(!r1.cancelled);
    CHECK(r1.hits.size() == 2); // alpha.txt and beta.md (binary.bin skipped, .hidden.txt excluded)

    // 2. Search-within-results refinement
    ck::vision::FilesSearchQuery refine;
    refine.name_pattern = "*.md";
    refine.name_mode = ck::vision::FilesSearchNameMode::Glob;
    const auto r2 = ck::vision::filter_files_search_results(r1, refine);
    CHECK(r2.hits.size() == 1);
    CHECK(r2.hits[0].entry_name && r2.hits[0].entry_name->display_name() == "beta.md");

    // 3. Regex mode + invalid regex fails closed
    ck::vision::FilesSearchQuery rq;
    rq.root = ws.native_root;
    rq.name_pattern = "^alpha\\.txt$";
    rq.name_mode = ck::vision::FilesSearchNameMode::Regex;
    const auto r3 = ck::vision::run_files_search(rq);
    CHECK(r3.hits.size() == 1);

    rq.name_pattern = "[unclosed";
    const auto r_bad = ck::vision::run_files_search(rq);
    CHECK(!r_bad.error_message.empty());

    // 4. Async FilesSearchService worker
    std::atomic_bool woken{false};
    ck::vision::FilesSearchService svc([&] { woken.store(true); });
    CHECK(svc.start(q) != 0);
    std::optional<ck::vision::FilesSearchResult> async_res;
    for (int i = 0; i < 200 && !async_res; ++i) {
        async_res = svc.take();
        if (!async_res)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(async_res.has_value());
    CHECK(async_res->hits.size() == 2);
    svc.request_stop();
    svc.join();

    // 5. Directory tree via ck_du_core in-process
    const auto tree = ck::vision::build_files_directory_tree(ws.native_root, 128, 8);
    CHECK(tree.error_message.empty());
    CHECK(tree.nodes.size() >= 2);
}

// ---- CP-08 Tests ----
void test_cp08_compare_diff_and_sync() {
    TempWorkspace ws("cp08");
    ws.write_text("left/same.txt", "identical content\n");
    ws.write_text("right/same.txt", "identical content\n");
    ws.write_text("left/only_left.txt", "from left\n");
    ws.write_text("right/only_right.txt", "from right\n");
    // Equal-size different content!
    ws.write_text("left/divergent.txt", "AAAA1111");
    ws.write_text("right/divergent.txt", "BBBB2222");

    // Equalize timestamps on divergent.txt to force exact equal-size/equal-time conflict
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(ws.root / "left/divergent.txt", now);
    std::filesystem::last_write_time(ws.root / "right/divergent.txt", now);

    // 1. Thorough content compare + TwoWayCopyOnly
    ck::vision::FilesSyncOptions opts;
    opts.compare_mode = ck::vision::FilesCompareMode::ThoroughContent;
    opts.direction = ck::vision::FilesSyncDirection::TwoWayCopyOnly;
    opts.allow_one_way_deletions = true; // Must STILL never delete in TwoWayCopyOnly!
    opts.timestamp_quantum_ns = 2'000'000'000ULL;

    const auto plan = ck::vision::build_files_sync_plan(ws.child("left"), ws.child("right"), opts);
    CHECK(plan.error_message.empty());
    CHECK(plan.identical_count == 1);
    CHECK(plan.conflict_count == 1); // divergent.txt
    CHECK(plan.action_count == 2);   // only_left.txt -> right, only_right.txt -> left

    const auto exec = ck::vision::execute_files_sync_plan(plan);
    CHECK(exec.completed == 2);
    CHECK(exec.conflicts_preserved == 1);
    CHECK(exec.failed == 0);
    // Verify unique files copied both ways and divergent conflict was NOT clobbered!
    CHECK(ws.read_text("right/only_left.txt") == "from left\n");
    CHECK(ws.read_text("left/only_right.txt") == "from right\n");
    CHECK(ws.read_text("left/divergent.txt") == "AAAA1111");
    CHECK(ws.read_text("right/divergent.txt") == "BBBB2222");

    // 2. Text diff
    const auto td = ck::vision::compute_text_diff("line1\nline2\nline3\n", "line1\nLINE2\nline3\nline4\n");
    CHECK(!td.identical);
    CHECK(!td.binary);
    CHECK(!td.text_lines.empty());

    // 3. Binary diff via native files
    std::vector<std::byte> b1{std::byte{0x00}, std::byte{0x10}, std::byte{0x20}};
    std::vector<std::byte> b2{std::byte{0x00}, std::byte{0x99}, std::byte{0x20}, std::byte{0xAA}};
    ws.write_bytes("b1.bin", b1);
    ws.write_bytes("b2.bin", b2);
    const auto bd = ck::vision::diff_native_files(ws.child("b1.bin"), ws.child("b2.bin"));
    CHECK(bd.binary);
    CHECK(!bd.identical);
    CHECK(bd.binary_diffs.size() == 2);
    CHECK(bd.binary_diffs[0].offset == 1);
    CHECK(bd.binary_diffs[1].offset == 3);
}

// ---- CP-09 Tests ----
void test_cp09_ordinary_archives() {
    TempWorkspace ws("cp09");
    ws.write_text("src/hello.txt", "Hello from ordinary archive!\nRepeat repeat repeat repeat repeat.\n");
    ws.write_text("src/nested/deep.txt", "Nested file payload\n");
    const std::string long_name =
        "very_long_directory_name_exceeding_one_hundred_bytes_for_pax_header_validation/"
        "and_a_very_long_filename_component_0123456789_abcdef.txt";

    // Security path & link validation
    CHECK(!ck::vision::validate_ordinary_archive_entry_path("../escape.txt"));
    CHECK(!ck::vision::validate_ordinary_archive_entry_path("dir/../../escape.txt"));
    CHECK(!ck::vision::validate_ordinary_archive_entry_path("/etc/passwd"));
    CHECK(!ck::vision::validate_ordinary_archive_entry_path("C:\\Windows\\evil.dll"));
    CHECK(ck::vision::validate_ordinary_archive_entry_path("safe/sub/file.txt"));
    CHECK(!ck::vision::validate_ordinary_archive_link_target("a/link", "../../etc/passwd"));
    CHECK(!ck::vision::validate_ordinary_archive_link_target("a/link", "/etc/passwd"));
    CHECK(ck::vision::validate_ordinary_archive_link_target("a/b/link", "../sibling.txt"));

    // CKARV01 must never be misclassified as ZIP/TAR
    const std::array<std::byte, 8> ckarv_magic{
        std::byte{'C'}, std::byte{'K'}, std::byte{'A'}, std::byte{'R'},
        std::byte{'V'}, std::byte{'0'}, std::byte{'1'}, std::byte{0}};
    CHECK(ck::vision::detect_ordinary_archive_format("backup.ckarchive", ckarv_magic) ==
          ck::vision::OrdinaryArchiveFormat::VolumeArchiveCkArv01);

    std::vector<ck::vision::OrdinaryArchiveInputItem> items;
    items.push_back({"hello.txt", ws.child("src/hello.txt"), false});
    items.push_back({"nested/deep.txt", ws.child("src/nested/deep.txt"), false});
    items.push_back({long_name, ws.child("src/nested/deep.txt"), false});

    // 1. ZIP roundtrip + modify (add & delete)
    const auto zip_path = ws.child("bundle.zip");
    const auto zc = ck::vision::create_ordinary_archive(
        zip_path, ck::vision::OrdinaryArchiveFormat::Zip, items, true);
    CHECK(zc.ok);
    CHECK(zc.entries_written == 3);

    const auto zlist = ck::vision::list_ordinary_archive(zip_path);
    CHECK(zlist.error_message.empty());
    CHECK(zlist.format == ck::vision::OrdinaryArchiveFormat::Zip);
    CHECK(zlist.entries.size() == 3);

    const auto zread = ck::vision::read_ordinary_archive_entry(zip_path, 0);
    CHECK(zread.has_value());
    CHECK(std::string(reinterpret_cast<const char *>(zread->data()), zread->size()) ==
          "Hello from ordinary archive!\nRepeat repeat repeat repeat repeat.\n");

    ws.mkdir("extract_zip");
    const auto zext = ck::vision::extract_ordinary_archive(zip_path, ws.child("extract_zip"));
    CHECK(zext.error_message.empty());
    CHECK(zext.extracted_files == 3);
    CHECK(ws.read_text("extract_zip/hello.txt") ==
          "Hello from ordinary archive!\nRepeat repeat repeat repeat repeat.\n");
    CHECK(ws.read_text("extract_zip/" + long_name) == "Nested file payload\n");

    // Re-extract without overwrite skips conflicts cleanly
    const auto zext2 = ck::vision::extract_ordinary_archive(zip_path, ws.child("extract_zip"), {}, false);
    CHECK(zext2.skipped_conflicts == 3);

    // Modify ZIP: remove entry 0, add new entry
    ws.write_text("src/added.txt", "Added later\n");
    std::vector<ck::vision::OrdinaryArchiveInputItem> add_items{{"added.txt", ws.child("src/added.txt"), false}};
    std::vector<std::size_t> rem_indices{0};
    const auto zmod = ck::vision::modify_ordinary_archive(zip_path, add_items, rem_indices);
    CHECK(zmod.ok);
    const auto zlist2 = ck::vision::list_ordinary_archive(zip_path);
    CHECK(zlist2.entries.size() == 3);

    // 2. TAR roundtrip (including >100-byte PAX long filename!)
    const auto tar_path = ws.child("bundle.tar");
    const auto tc = ck::vision::create_ordinary_archive(
        tar_path, ck::vision::OrdinaryArchiveFormat::Tar, items);
    CHECK(tc.ok);
    const auto tlist = ck::vision::list_ordinary_archive(tar_path);
    CHECK(tlist.error_message.empty());
    CHECK(tlist.entries.size() == 3);
    CHECK(tlist.entries[2].path == long_name);

    ws.mkdir("extract_tar");
    const auto text = ck::vision::extract_ordinary_archive(tar_path, ws.child("extract_tar"));
    CHECK(text.error_message.empty());
    CHECK(text.extracted_files == 3);
    CHECK(ws.read_text("extract_tar/" + long_name) == "Nested file payload\n");

#if defined(CKTOOLS_HAS_ZLIB)
    // 3. TAR.GZ roundtrip
    const auto tgz_path = ws.child("bundle.tar.gz");
    const auto gc = ck::vision::create_ordinary_archive(
        tgz_path, ck::vision::OrdinaryArchiveFormat::TarGz, items);
    CHECK(gc.ok);
    const auto glist = ck::vision::list_ordinary_archive(tgz_path);
    CHECK(glist.error_message.empty());
    CHECK(glist.entries.size() == 3);
#endif

    // 4. Malformed archive fails closed (including malformed PAX record with rec_len <= sp+1)
    ws.write_text("corrupt.zip", "PK\x03\x04truncated garbage");
    const auto bad = ck::vision::list_ordinary_archive(ws.child("corrupt.zip"));
    CHECK(!bad.error_message.empty());

    // Malformed TAR with PAX record "1 a=b\n" (rec_len < space offset) must not underflow/crash!
    {
        std::vector<std::byte> bad_tar(1024, std::byte{0});
        const char *nm = "PaxHeader/bad";
        std::memcpy(bad_tar.data(), nm, std::strlen(nm));
        std::memcpy(bad_tar.data() + 100, "0000644\0", 8);
        std::memcpy(bad_tar.data() + 108, "0000000\0", 8);
        std::memcpy(bad_tar.data() + 116, "0000000\0", 8);
        std::memcpy(bad_tar.data() + 124, "00000000006\0", 12); // size = 6
        std::memcpy(bad_tar.data() + 136, "00000000000\0", 12);
        std::memset(bad_tar.data() + 148, ' ', 8);
        bad_tar[156] = std::byte{'x'};
        std::memcpy(bad_tar.data() + 257, "ustar\0", 6);
        std::memcpy(bad_tar.data() + 263, "00", 2);
        unsigned int csum = 0;
        for (std::size_t i = 0; i < 512; ++i)
            csum += std::to_integer<unsigned char>(bad_tar[i]);
        char cbuf[16]{};
        std::snprintf(cbuf, sizeof(cbuf), "%06o", csum);
        std::memcpy(bad_tar.data() + 148, cbuf, 6);
        bad_tar[154] = std::byte{0};
        bad_tar[155] = std::byte{' '};
        const char *pax_bad = "1 a=b\n";
        std::memcpy(bad_tar.data() + 512, pax_bad, 6);
        ws.write_bytes("bad_pax.tar", bad_tar);
        const auto bp = ck::vision::list_ordinary_archive(ws.child("bad_pax.tar"));
        CHECK(bp.entries.empty());
    }
}

// ---- CP-10 Tests ----
void test_cp10_editor_and_viewer() {
    TempWorkspace ws("cp10");
    ws.write_text("note.md", "# Title\r\nLine two\r\n");

    // 1. Open preserves CRLF style and captures snapshot
    const auto opened = ck::vision::open_files_editor_document(ws.child("note.md"));
    CHECK(opened.ok);
    CHECK(opened.snapshot.newline_style == ck::vision::FilesNewlineStyle::CrLf);
    CHECK(opened.text == "# Title\nLine two\n");

    // 2. Normal save succeeds and preserves CRLF on disk
    const auto saved = ck::vision::save_files_editor_document(
        opened.snapshot, "# Title\nLine two\nLine three\n");
    CHECK(saved.status == ck::vision::FilesSaveStatus::Saved);
    CHECK(ws.read_text("note.md") == "# Title\r\nLine two\r\nLine three\r\n");

    // 3. External modification conflict detection
    ws.write_text("note.md", "EXTERNAL CHANGE\r\n");
    const auto conflict = ck::vision::save_files_editor_document(
        saved.updated_snapshot, "my stale edit\n", false);
    CHECK(conflict.status == ck::vision::FilesSaveStatus::ExternalConflict);
    CHECK(ws.read_text("note.md") == "EXTERNAL CHANGE\r\n");

    // Explicit force overwrite succeeds
    const auto forced = ck::vision::save_files_editor_document(
        saved.updated_snapshot, "forced edit\n", true);
    CHECK(forced.status == ck::vision::FilesSaveStatus::Saved);

    // 4. Save As refuses overwrite when allow_replace == false
    const auto exists_refusal = ck::vision::save_as_files_editor_document(
        ws.child("note.md"), "other", ck::vision::FilesNewlineStyle::Lf, false);
    CHECK(exists_refusal.status == ck::vision::FilesSaveStatus::TargetExists);

    // 5. Syntax profile registry has standard profiles + ck-edit Markdown
    auto reg = ck::vision::make_files_syntax_registry();
    CHECK(reg->find("markdown") != nullptr);
    CHECK(reg->find("json") != nullptr);

    // 6. Streaming viewer (Text, Hex, JSON) + stream search
    ws.write_text("data.json", "{\"alpha\":1,\"target\":\"FOUND_ME\",\"omega\":2}");
    const auto v_json = ck::vision::read_files_viewer_page(
        ws.child("data.json"), ck::vision::FilesViewerMode::Json, 0, 4096);
    CHECK(v_json.ok);
    CHECK(v_json.lines.size() >= 3);

    const auto v_hex = ck::vision::read_files_viewer_page(
        ws.child("data.json"), ck::vision::FilesViewerMode::Hex, 0, 4096);
    CHECK(v_hex.ok);
    CHECK(!v_hex.lines.empty());
    CHECK(v_hex.lines[0].find("00000000") != std::string::npos);

    const auto found_off = ck::vision::search_files_viewer_stream(
        ws.child("data.json"), "found_me", 0, false);
    CHECK(found_off.has_value());
    CHECK(*found_off > 0);
}

// ---- CP-11 Tests ----
void test_cp11_terminal_and_execution() {
    TempWorkspace ws("cp11");

    // 1. DirectArgv placeholder expansion treats hostile filenames as pure data (no shell injection!)
    ck::vision::FilesUserMenuEntry entry;
    entry.label = "Inspect";
    entry.mode = ck::vision::FilesCommandMode::DirectArgv;
#if defined(_WIN32)
    entry.executable = "C:\\Windows\\System32\\cmd.exe";
#else
    entry.executable = "/bin/echo";
#endif
    entry.argument_templates = {"--", "%f", "%F"};

    ck::vision::FilesCommandPlaceholderContext ctx;
    ctx.active_directory = ws.native_root.display_text();
    ctx.current_name = "evil; rm -rf / $(touch pwned) `id`";
    ctx.marked_names = {"file one.txt", "file;two.txt"};

    const auto expanded = ck::vision::expand_files_user_menu_entry(entry, ctx);
    CHECK(expanded.valid);
    CHECK(expanded.launch.arguments.size() == 4);
    CHECK(expanded.launch.arguments[1] == "evil; rm -rf / $(touch pwned) `id`");
    CHECK(expanded.launch.arguments[2] == "file one.txt");
    CHECK(expanded.launch.arguments[3] == "file;two.txt");

    // 1b. ExplicitShell mode MUST shell-quote hostile filenames so they remain data (no injection!)
    ck::vision::FilesUserMenuEntry shell_entry;
    shell_entry.label = "Shell inspect";
    shell_entry.mode = ck::vision::FilesCommandMode::ExplicitShell;
    shell_entry.shell_template = "echo %f %F";
    const auto shell_exp = ck::vision::expand_files_user_menu_entry(shell_entry, ctx);
    CHECK(shell_exp.valid);
    // The hostile filename must be quoted, not raw unquoted shell syntax!
#if defined(_WIN32)
    CHECK(shell_exp.review_preview.find("\"evil; rm -rf / $(touch pwned) `id`\"") != std::string::npos);
#else
    CHECK(shell_exp.review_preview.find("'evil; rm -rf / $(touch pwned) `id`'") != std::string::npos);
    CHECK(shell_exp.review_preview.find("'file one.txt' 'file;two.txt'") != std::string::npos);
#endif
    // Control-char / newline injection in ExplicitShell mode must fail closed
    auto bad_ctx = ctx;
    bad_ctx.current_name = "good\nrm -rf /";
    CHECK(!ck::vision::expand_files_user_menu_entry(shell_entry, bad_ctx).valid);

    // 2. Direct command parser handles quotes and rejects NUL / unterminated quotes
#if defined(_WIN32)
    const std::string direct_exe = "C:\\Windows\\System32\\cmd.exe";
#else
    const std::string direct_exe = "/bin/echo";
#endif
    const auto parsed = ck::vision::parse_files_direct_command(
        direct_exe + " \"hello world\" 'a;b'", ctx.active_directory);
    CHECK(parsed.valid);
    CHECK(parsed.launch.arguments.size() == 2);
    CHECK(parsed.launch.arguments[0] == "hello world");
    CHECK(parsed.launch.arguments[1] == "a;b");

    CHECK(!ck::vision::parse_files_direct_command(direct_exe + " \"unterminated", ctx.active_directory).valid);

    // 3. Real bounded process execution via ckVision NativeProcessRunner
    TestClock clock;
#if !defined(_WIN32)
    const auto run_cmd = ck::vision::parse_files_direct_command("/bin/echo cp11_ok", ctx.active_directory);
    const auto run_res = ck::vision::execute_files_command(run_cmd, clock);
    CHECK(run_res.successful());
    CHECK(run_res.stdout_capture.bytes.find("cp11_ok") != std::string::npos);
#endif

    // 4. Interactive terminal launch spec & history
    const auto tspec = ck::vision::build_files_interactive_terminal_spec(ctx.active_directory);
    CHECK(tspec.exit_policy == ckv::core::TerminalExitPolicy::TerminateAfterGrace);
    CHECK(ckv::core::validate_process_launch(tspec) == ckv::core::ProcessLaunchValidation::Valid);

    ck::vision::FilesCommandHistory hist(2);
    hist.record("cmd1");
    hist.record("cmd2");
    hist.record("cmd1"); // deduplicates and moves to end
    CHECK(hist.entries().size() == 2);
    CHECK(hist.entries()[0] == "cmd2");
    CHECK(hist.entries()[1] == "cmd1");
}

// ---- CP-12 Tests ----
class MockRemoteTransport final : public ck::vision::FilesRemoteTransport {
  public:
    bool reject_host_key = false;
    bool fail_remove_uncertain = false;
    int remove_calls = 0;
    int upload_calls = 0;

    ck::vision::FilesProviderCapabilities capabilities() const override {
        return ck::vision::provider_capabilities_for(ck::vision::FilesProviderKind::Sftp);
    }

    ck::vision::FilesRemoteListResult list(const ck::vision::FilesRemoteUri &,
                                           ck::vision::FilesHostKeyPolicy,
                                           std::stop_token) override {
        if (reject_host_key)
            return {ck::vision::FilesRemoteStatus::HostKeyRejected, {}, "Host key verification failed"};
        return {ck::vision::FilesRemoteStatus::Ok, {{"remote.txt", false, false, 42, "-rw-r--r--", "Nov 27"}}, {}};
    }

    ck::vision::FilesRemoteTransferResult download(const ck::vision::FilesRemoteUri &,
                                                   const ck::files::NativeAbsolutePath &,
                                                   ck::vision::FilesHostKeyPolicy,
                                                   std::stop_token) override {
        return {ck::vision::FilesRemoteStatus::Ok, 42, false, {}};
    }

    ck::vision::FilesRemoteTransferResult upload(const ck::files::NativeAbsolutePath &,
                                                 const ck::vision::FilesRemoteUri &,
                                                 ck::vision::FilesHostKeyPolicy,
                                                 std::stop_token) override {
        ++upload_calls;
        return {ck::vision::FilesRemoteStatus::Ok, 10, false, {}};
    }

    ck::vision::FilesRemoteTransferResult remove_remote(const ck::vision::FilesRemoteUri &,
                                                        ck::vision::FilesHostKeyPolicy,
                                                        std::stop_token) override {
        ++remove_calls;
        if (fail_remove_uncertain)
            return {ck::vision::FilesRemoteStatus::UncertainCommit, 0, true, "Connection dropped mid-rm"};
        return {ck::vision::FilesRemoteStatus::Ok, 0, false, {}};
    }
};

void test_cp12_remote_provider() {
    // 1. URI parsing & mandatory secret redaction
    const auto sftp = ck::vision::parse_files_remote_uri("sftp://alice:TopSecret123@example.org:2222/srv/files");
    CHECK(sftp.valid);
    CHECK(sftp.kind == ck::vision::FilesProviderKind::Sftp);
    CHECK(sftp.username == "alice");
    CHECK(sftp.has_secret);
    CHECK(sftp.host == "example.org");
    CHECK(sftp.port == 2222);
    CHECK(sftp.path == "/srv/files");
    const std::string redacted = sftp.redacted_display();
    CHECK(redacted.find("TopSecret123") == std::string::npos);
    CHECK(redacted.find("alice:***@example.org:2222/srv/files") != std::string::npos);

    const std::string diag = ck::vision::redact_remote_secrets("failed connecting to sftp://bob:hunter2@host/a");
    CHECK(diag.find("hunter2") == std::string::npos);
    CHECK(diag.find("bob:***@host/a") != std::string::npos);

    // Password containing '@' must be fully redacted without leaking the suffix!
    const std::string diag_at = ck::vision::redact_remote_secrets("err sftp://bob:p@ssword@host/a");
    CHECK(diag_at.find("p@ssword") == std::string::npos);
    CHECK(diag_at.find("ssword") == std::string::npos);
    CHECK(diag_at.find("bob:***@host/a") != std::string::npos);

    // 2. SMB & UNC parsing + provider capability distinction
    const auto smb = ck::vision::parse_files_remote_uri("smb://nas.local/backups/2026");
    CHECK(smb.valid);
    CHECK(smb.share == "backups");
    CHECK(smb.path == "/2026");

    const auto unc = ck::vision::parse_files_remote_uri("\\\\fileserver\\public\\docs\\readme.txt");
    CHECK(unc.valid);
    CHECK(unc.kind == ck::vision::FilesProviderKind::SmbMountedShare);
    CHECK(unc.host == "fileserver");
    CHECK(unc.share == "public");
    CHECK(ck::vision::provider_capabilities_for(ck::vision::FilesProviderKind::SmbMountedShare).mounted_share);
    CHECK(!ck::vision::provider_capabilities_for(ck::vision::FilesProviderKind::SmbProtocolClient).mounted_share);

    // Invalid URIs rejected (including non-numeric trailing junk in port)
    CHECK(!ck::vision::parse_files_remote_uri("smb://nas.local/").valid);
    CHECK(!ck::vision::parse_files_remote_uri("ftp://example.org/pub").valid);
    CHECK(!ck::vision::parse_files_remote_uri("sftp://host/bad\npath").valid);
    CHECK(!ck::vision::parse_files_remote_uri("sftp://host:22evil/path").valid);
    CHECK(!ck::vision::parse_files_remote_uri("sftp://host:0/path").valid);

    // 3. Host-key rejection & uncertain-commit fail-closed across reconnect
    auto mock = std::make_unique<MockRemoteTransport>();
    MockRemoteTransport *raw = mock.get();
    ck::vision::FilesRemoteSession session(std::move(mock), ck::vision::FilesHostKeyPolicy::StrictVerify);

    raw->reject_host_key = true;
    CHECK(session.list(sftp).status == ck::vision::FilesRemoteStatus::HostKeyRejected);
    raw->reject_host_key = false;
    CHECK(session.list(sftp).status == ck::vision::FilesRemoteStatus::Ok);

    raw->fail_remove_uncertain = true;
    const auto rm1 = session.remove_remote(sftp);
    CHECK(rm1.status == ck::vision::FilesRemoteStatus::UncertainCommit);
    CHECK(session.has_uncertain_commit());
    CHECK(raw->remove_calls == 1);

    // Blind retry MUST be blocked without calling transport!
    raw->fail_remove_uncertain = false;
    const auto rm2 = session.remove_remote(sftp);
    CHECK(rm2.status == ck::vision::FilesRemoteStatus::UncertainCommit);
    CHECK(raw->remove_calls == 1);

    // After explicit review acknowledgement, operation is permitted again
    session.acknowledge_uncertain_commit();
    CHECK(!session.has_uncertain_commit());
    const auto rm3 = session.remove_remote(sftp);
    CHECK(rm3.status == ck::vision::FilesRemoteStatus::Ok);
    CHECK(raw->remove_calls == 2);
}

// ---- CP-13 Tests ----
void append_le_u8(std::vector<std::byte> &out, std::uint8_t v) { out.push_back(std::byte{v}); }
void append_le_u32(std::vector<std::byte> &out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::byte{static_cast<unsigned char>((v >> (i * 8)) & 0xFF)});
}
void append_le_u64(std::vector<std::byte> &out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(std::byte{static_cast<unsigned char>((v >> (i * 8)) & 0xFF)});
}
void append_ckarv_str(std::vector<std::byte> &out, std::string_view s) {
    append_le_u64(out, s.size());
    for (char c : s) out.push_back(std::byte{static_cast<unsigned char>(c)});
}

std::vector<std::byte> build_test_ckarv01(bool hostile_path = false) {
    std::vector<std::byte> manifest;
    append_ckarv_str(manifest, "vol-test-id");
    append_ckarv_str(manifest, "TestVolume");
    append_ckarv_str(manifest, "/mnt/test");
    append_ckarv_str(manifest, "ext4");
    append_le_u8(manifest, 1); // Snapshot
    append_le_u8(manifest, static_cast<std::uint8_t>(ck::files::NameEncoding::PosixBytes));
    for (int i = 0; i < 6; ++i) append_le_u8(manifest, 0);

    // Entry 0: directory "docs"
    append_le_u8(manifest, static_cast<std::uint8_t>(ck::vision::FilesVolumeArchiveEntryKind::Directory));
    for (int i = 0; i < 7; ++i) append_le_u8(manifest, 0);
    append_le_u64(manifest, 0);
    append_le_u64(manifest, 0);
    append_ckarv_str(manifest, hostile_path ? "../evil_dir" : "docs");
    append_ckarv_str(manifest, "");
    append_le_u64(manifest, 0);

    // Entry 1: sparse file "docs/sparse.bin" (logical size = 131072 + 12 + 4096 = 135180,
    // 1 extent of 12 bytes at source_offset 131072, trailing zero hole of 4096 bytes)
    const std::string payload = "SPARSE_DATA!";
    const std::uint64_t logical_sz = 131072ULL + payload.size() + 4096ULL;
    append_le_u8(manifest, static_cast<std::uint8_t>(ck::vision::FilesVolumeArchiveEntryKind::RegularFile));
    for (int i = 0; i < 7; ++i) append_le_u8(manifest, 0);
    append_le_u64(manifest, logical_sz);
    append_le_u64(manifest, 0);
    append_ckarv_str(manifest, "docs/sparse.bin");
    append_ckarv_str(manifest, "");
    append_le_u64(manifest, 1); // 1 extent
    append_le_u64(manifest, 131072ULL); // source_offset
    append_le_u64(manifest, 0ULL);      // archive_offset
    append_le_u64(manifest, payload.size());

    // 1 loss record
    append_le_u64(manifest, 1);
    append_le_u64(manifest, 1);
    append_le_u8(manifest, static_cast<std::uint8_t>(ck::vision::FilesVolumeArchiveLossKind::Metadata));
    append_le_u8(manifest, 0);
    for (int i = 0; i < 6; ++i) append_le_u8(manifest, 0);
    append_ckarv_str(manifest, "docs/sparse.bin");
    append_ckarv_str(manifest, "xattr omitted");

    std::vector<std::byte> image;
    const char magic[8] = {'C', 'K', 'A', 'R', 'V', '0', '1', '\0'};
    for (char c : magic) image.push_back(std::byte{static_cast<unsigned char>(c)});
    append_le_u32(image, 2); // version
    append_le_u32(image, 1); // flags = snapshot
    append_le_u64(image, manifest.size());
    append_le_u64(image, 56ULL + manifest.size()); // data_offset
    append_le_u64(image, payload.size());          // data_size
    append_le_u64(image, 2);                       // entry_count
    append_le_u64(image, 0);                       // reserved
    image.insert(image.end(), manifest.begin(), manifest.end());
    for (char c : payload) image.push_back(std::byte{static_cast<unsigned char>(c)});
    return image;
}

void test_cp13_volume_archive_restore() {
    TempWorkspace ws("cp13");
    const auto img = build_test_ckarv01(false);
    ws.write_bytes("volume.ckarchive", img);

    // 1. Inspect valid CKARV01
    const auto info = ck::vision::inspect_volume_archive_file(ws.child("volume.ckarchive"));
    CHECK(info.valid);
    CHECK(info.format_version == 2);
    CHECK(info.directory_count == 1);
    CHECK(info.regular_file_count == 1);
    CHECK(info.sparse_file_count == 1);
    CHECK(info.losses.size() == 1);

    // 2. Destination safety guard blocks raw devices & live roots
    auto root_slash = ck::files::NativeAbsolutePath::from_posix("/");
    CHECK(root_slash.value() && !ck::vision::validate_volume_archive_restore_destination(*root_slash.value()));
    auto dev_sda = ck::files::NativeAbsolutePath::from_posix("/dev/sda");
    CHECK(dev_sda.value() && !ck::vision::validate_volume_archive_restore_destination(*dev_sda.value()));

    // 3. Verified restore preserves sparse holes & exact bytes
    ws.mkdir("restored");
    const auto plan = ck::vision::plan_volume_archive_restore(
        ws.child("volume.ckarchive"), ws.child("restored"));
    CHECK(plan.valid);
    const auto res = ck::vision::restore_volume_archive(plan, false);
    CHECK(res.ok);
    CHECK(res.restored_directories == 1);
    CHECK(res.restored_files == 1);
    CHECK(res.restored_sparse_files == 1);
    CHECK(res.verified_bytes == 131072ULL + 12ULL + 4096ULL);

    const std::string restored_bytes = ws.read_text("restored/docs/sparse.bin");
    CHECK(restored_bytes.size() == 131072ULL + 12ULL + 4096ULL);
    CHECK(restored_bytes[0] == '\0');
    CHECK(restored_bytes.substr(131072, 12) == "SPARSE_DATA!");
    CHECK(restored_bytes.back() == '\0');

    // 4. Hostile path in CKARV01 fails closed
    ws.write_bytes("hostile.ckarchive", build_test_ckarv01(true));
    const auto bad_info = ck::vision::inspect_volume_archive_file(ws.child("hostile.ckarchive"));
    CHECK(!bad_info.valid);

    // 5. Staged overwrite preserves existing file if restore fails mid-read
    ws.write_text("restored/docs/sparse.bin", "EXISTING_VALUABLE_DATA");
    auto truncated_img = build_test_ckarv01(false);
    // Corrupt archive by truncating data payload AFTER planning (simulating I/O error mid-restore)
    ws.write_bytes("will_fail.ckarchive", truncated_img);
    const auto fail_plan = ck::vision::plan_volume_archive_restore(
        ws.child("will_fail.ckarchive"), ws.child("restored"));
    CHECK(fail_plan.valid);
    truncated_img.resize(truncated_img.size() - 6); // Truncate data payload
    ws.write_bytes("will_fail.ckarchive", truncated_img);
    const auto fail_res = ck::vision::restore_volume_archive(fail_plan, true);
    CHECK(!fail_res.ok);
    // Original file MUST still be intact!
    CHECK(ws.read_text("restored/docs/sparse.bin") == "EXISTING_VALUABLE_DATA");
}

// ---- CP-14 Combined FilesApp Integration Test ----
void test_cp14_combined_files_app_workflow() {
    TempWorkspace ws("cp14");
    ws.write_text("left/alpha.txt", "hello from left\n");
    ws.write_text("right/alpha.txt", "hello from right\n");

    // Private index cache MUST live on an ownership-enforcing filesystem (CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT).
    const std::filesystem::path private_root_base =
        std::string_view(CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT).empty()
            ? ws.root
            : std::filesystem::path(CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const std::filesystem::path cache_dir =
        private_root_base /
        ("parity_cp14_cache_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(cache_dir);
    struct CacheCleanup final {
        std::filesystem::path dir;
        ~CacheCleanup() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } cache_cleanup{cache_dir};
    std::filesystem::permissions(
        cache_dir,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
    auto cache_native = ck::files::NativeAbsolutePath::from_host(cache_dir);
    CHECK(cache_native.value() != nullptr);

    ckv::term::HeadlessTerminal terminal({120, 36});
    TestClock clock;
    ckv::ui::Application app(terminal, clock);
    ck::vision::FilesAppWake wake(app);
    ck::vision::FilesPaneService left_svc(
        ck::vision::native_files_pane_backend(*cache_native.value()), {}, wake.callback());
    ck::vision::FilesPaneService right_svc(
        ck::vision::native_files_pane_backend(*cache_native.value()), {}, wake.callback());

    {
        ck::vision::FilesApp files_app(
            app, {&left_svc, &right_svc}, wake,
            {ws.child("left"), ws.child("right")},
            [&] { app.request_quit(); });

        // Pump until both panes finish initial read
        for (int i = 0; i < 200 && (files_app.pending(0) || files_app.pending(1)); ++i) {
            app.step(clock.now_nanos() + 5'000'000);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(!files_app.pending(0));
        CHECK(!files_app.pending(1));

        // Verify all new commands are registered & available
        CHECK(files_app.availability("ck.files.search").enabled);
        CHECK(files_app.availability("ck.files.directory_tree").enabled);
        CHECK(files_app.availability("ck.files.sync_directories").enabled);
        CHECK(files_app.availability("ck.files.diff_files").enabled);
        CHECK(files_app.availability("ck.files.edit").enabled);
        CHECK(files_app.availability("ck.files.terminal").enabled);
        CHECK(files_app.availability("ck.files.connect_remote").enabled);

        // Open embedded editors (existing + new), edit, verify save & dirty-close protection!
        CHECK(files_app.open_embedded_editor(false));
        CHECK(files_app.open_embedded_editor(true));
        CHECK(files_app.embedded_editor_count() == 2);
        CHECK(!files_app.has_dirty_embedded_editors());
        CHECK(files_app.edit_embedded_editor_text_for_test(0, "updated from embedded editor\n"));
        CHECK(files_app.has_dirty_embedded_editors());
        // Save embedded editor 0 and verify disk bytes + clean state
        CHECK(files_app.save_embedded_editor(0));
        CHECK(!files_app.has_dirty_embedded_editors());
        CHECK(ws.read_text("left/alpha.txt") == "updated from embedded editor\n");
        // Pump the async refresh triggered by save_embedded_editor
        for (int i = 0; i < 200 && files_app.pending(0); ++i) {
            app.step(clock.now_nanos() + 5'000'000);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(!files_app.pending(0));

        // Open directory tree
        CHECK(files_app.open_directory_tree());
        CHECK(files_app.parity_dialog_pending());

        // Open embedded interactive terminal (ckVision PTY/ConPTY)
        CHECK(files_app.open_embedded_terminal());
        CHECK(files_app.embedded_terminal_open());
        app.step(clock.now_nanos() + 10'000'000);

        // Close parity report, pump deferred window detach, dirty editor 1, then verify request_close() prompts!
        files_app.request_close(); // schedules parity_report_window close
        app.step(clock.now_nanos() + 15'000'000);
        CHECK(!files_app.parity_dialog_pending());
        CHECK(files_app.edit_embedded_editor_text_for_test(1, "unsaved draft\n"));
        CHECK(files_app.has_dirty_embedded_editors());
        files_app.request_close();
        CHECK(files_app.close_confirmation_pending());
    }

    left_svc.request_stop();
    right_svc.request_stop();
    left_svc.join();
    right_svc.join();
}

} // namespace

int main() {
    try {
        test_cp07_search_and_directory_tree();
        test_cp08_compare_diff_and_sync();
        test_cp09_ordinary_archives();
        test_cp10_editor_and_viewer();
        test_cp11_terminal_and_execution();
        test_cp12_remote_provider();
        test_cp13_volume_archive_restore();
        test_cp14_combined_files_app_workflow();
    } catch (const std::exception &ex) {
        std::cerr << "UNHANDLED EXCEPTION: " << ex.what() << "\n";
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << "FAILED: " << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "ALL CP-07..CP-14 PARITY TESTS PASSED (8/8 suites).\n";
    return 0;
}
