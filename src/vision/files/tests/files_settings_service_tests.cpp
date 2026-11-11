// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_settings_service.hpp"
#include "files_app.hpp"
#include "directory_index_store.hpp"
#include "simulated_filesystem.hpp"
#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/testing/cktest.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace ck::vision;
using namespace ck::files;
using namespace std::chrono_literals;
template<class T>T take(Observation<T> o){if(!o.value())throw std::runtime_error("settings fixture observation unavailable");return std::move(o).take_value();}
FilesPaneLocation location(std::string text){return FilesPaneLocation(take(NativeAbsolutePath::from_posix(std::move(text))));}
struct Gate {
    std::mutex mutex;std::condition_variable cv;bool entered=false,released=false;
    void hold(){std::unique_lock lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return released;});}
    void wait(){std::unique_lock lock(mutex);if(!cv.wait_for(lock,5s,[&]{return entered;}))throw std::runtime_error("settings write did not enter gate");}
    void release(){std::lock_guard lock(mutex);released=true;cv.notify_all();}
};
// Actual Store and MemoryFileSystem CAS protocol; only the host I/O endpoint is
// fake. No native preservation, provider-safety or persistent-global-OOM claim.
class SettingsFS final:public ckv::FileSystem {
    mutable std::mutex mutex_;
    ckv::MemoryFileSystem files_;
    const std::thread::id ui_=std::this_thread::get_id();
    mutable std::thread::id worker_;
    void record() const {
        if(!enforce.load())return;
        std::lock_guard lock(trace_mutex);
        const auto current=std::this_thread::get_id();
        if(current==ui_)++ui_io;
        if(worker_==std::thread::id{})worker_=current;
        if(current!=worker_)++worker_changed;
    }
  public:
    mutable std::mutex trace_mutex;
    mutable std::atomic<unsigned> ui_io{0},worker_changed{0};
    std::atomic<bool> enforce{false},block{false},throw_after_publish{false},throw_before_publish{false};
    std::atomic<unsigned> writes{0},active{0},overlap{0};
    Gate gate,second_gate;
    std::atomic<bool> block_second{false};
    std::vector<std::string> attempted;
    std::vector<ckv::FileWriteExpectationKind> expectations;
    std::vector<ckv::FileEntry> list_directory(std::string_view path)const override {record();std::lock_guard lock(mutex_);return files_.list_directory(path);}
    bool exists(std::string_view path)const noexcept override {record();std::lock_guard lock(mutex_);return files_.exists(path);}
    bool is_directory(std::string_view path)const noexcept override {record();std::lock_guard lock(mutex_);return files_.is_directory(path);}
    bool create_directories(std::string_view path)override {record();std::lock_guard lock(mutex_);return files_.create_directories(path);}
    std::optional<ckv::FileReadResult> read_file(std::string_view path)const override {record();std::lock_guard lock(mutex_);return files_.read_file(path);}
    FilesSettingsStore::BoundedRead bounded_reader() {
        return [this](std::string_view path, std::size_t cap) {
            record();std::lock_guard lock(mutex_);return files_.read_file_bounded(path,cap);
        };
    }
    ckv::FileWriteResult write_file_atomic(std::string_view path,std::string_view bytes,ckv::FileWriteExpectation expected)override {
        record();if(active.fetch_add(1)!=0)++overlap;
        struct Done {std::atomic<unsigned>&active;~Done(){--active;}} done{active};
        const auto call=++writes;if(block.exchange(false))gate.hold();
        if(call==2 && block_second.load())second_gate.hold();
        if(throw_before_publish.exchange(false))throw std::bad_alloc();
        std::lock_guard lock(mutex_);attempted.emplace_back(bytes);expectations.push_back(expected.kind);
        auto result=files_.write_file_atomic(path,bytes,std::move(expected));
        if(throw_after_publish.exchange(false))throw std::bad_alloc();
        return result;
    }
    std::optional<ckv::FileReadResult> observed()const {std::lock_guard lock(mutex_);return files_.read_file("/settings/value");}
    void foreign_write(std::string bytes){std::lock_guard lock(mutex_);files_.add_file("/settings/value",std::move(bytes));}
    void print_threads()const {std::lock_guard lock(trace_mutex);std::fprintf(stderr,"settings threads: UI=%zu worker=%zu UI_IO=%u worker_changed=%u\n",std::hash<std::thread::id>{}(ui_),std::hash<std::thread::id>{}(worker_),ui_io.load(),worker_changed.load());}
};
struct Unblock {SettingsFS &fs;~Unblock(){fs.gate.release();fs.second_gate.release();}};
template<class Predicate>void await(Predicate p){const auto end=std::chrono::steady_clock::now()+5s;while(!p()){if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("settings condition timed out");std::this_thread::sleep_for(1ms);}}
FilesPersistentSettings snapshot(unsigned n){FilesPersistentSettings s;s.views[0].display_substring=std::to_string(n);s.bookmarks[1].push_back(location("/bookmark-"+std::to_string(n)));return s;}

class Bytes final:public IndexByteFile {
    std::vector<std::byte> bytes_;
  public:
    Observation<std::uint64_t> size()override{return Observation<std::uint64_t>::available(bytes_.size());}
    Observation<std::size_t> write_at(std::uint64_t at,std::span<const std::byte> bytes)override {
        if(at>1024*1024 || bytes.size()>1024*1024-at)return Observation<std::size_t>::unavailable(ObservationState::Failed,{ErrorDomain::Adapter,1,"settings_fixture.index_bound"});
        const auto first=static_cast<std::size_t>(at);bytes_.resize(std::max(bytes_.size(),first+bytes.size()));std::copy(bytes.begin(),bytes.end(),bytes_.begin()+static_cast<std::ptrdiff_t>(first));return Observation<std::size_t>::available(bytes.size());
    }
    Observation<std::size_t> read_at(std::uint64_t at,std::span<std::byte> out)override {
        if(at>=bytes_.size())return Observation<std::size_t>::available(0);
        const auto first=static_cast<std::size_t>(at);const auto n=std::min(out.size(),bytes_.size()-first);std::copy_n(bytes_.begin()+static_cast<std::ptrdiff_t>(first),n,out.begin());return Observation<std::size_t>::available(n);
    }
};
class MemoryWorkspace final:public IndexFileFactory {
  public:Observation<std::unique_ptr<IndexByteFile>> create_file()override{return Observation<std::unique_ptr<IndexByteFile>>::available(std::make_unique<Bytes>());}
};
#ifdef _WIN32
constexpr auto pane_storage_model = StorageModel::WindowsHandleAttributes;
#else
constexpr auto pane_storage_model = StorageModel::LinuxStatx;
#endif

NativeAbsolutePath pane_address(std::size_t pane){
#ifdef _WIN32
    return take(NativeAbsolutePath::from_windows(pane?u"V:\\settings-right":u"V:\\settings-left"));
#else
    return take(NativeAbsolutePath::from_posix(pane?"/settings-right":"/settings-left"));
#endif
}
class PaneFS final:public BasicFileSystem {
    ck::testing::SimulatedFileSystem files_;
  public:
    PaneFS(){for(std::size_t i=0;i<2;++i){auto dir=files_.make_node(ObjectKind::Directory);files_.bind(take(pane_address(i).host_path()),dir);
#ifdef _WIN32
        auto name=*NativeName::from_windows(u"row");
#else
        auto name=*NativeName::from_posix("row");
#endif
        files_.put(dir,std::move(name),files_.make_node(ObjectKind::RegularFile));}}
    Observation<std::unique_ptr<DirectoryCursor>> open_directory(const std::filesystem::path &path,DirectoryLinkPolicy policy)const override{return files_.open_directory(path,policy);}
    Observation<BasicMetadata> inspect_basic(const std::filesystem::path &path)const override{return files_.inspect_basic(path);}
};
struct AppFixture {
    SettingsFS files;
    FilesSettingsStore store{files,"/settings/value"};
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal{ckv::Size{120,34}};
    ckv::ui::Application application{terminal,clock};
    FilesAppWake wake{application};
    std::unique_ptr<FilesSettingsService> settings;
    std::array<std::unique_ptr<FilesPaneService>,2> panes;
    std::unique_ptr<FilesApp> app;
    unsigned quits=0;
    explicit AppFixture(FilesPersistentSettings initial={},bool persist=true){files.foreign_write(files_settings_text(initial));auto loaded=store.load(files.bounded_reader());files.enforce=true;settings=std::make_unique<FilesSettingsService>(store,wake.callback());
        for (auto &pane : panes) {
            FilesPaneBackend backend;
            backend.storage_model = pane_storage_model;
            backend.filesystem = [] { return std::make_unique<PaneFS>(); };
            backend.workspace = [] {
                return Observation<std::shared_ptr<IndexFileFactory>>::available(std::make_shared<MemoryWorkspace>());
            };
            pane = std::make_unique<FilesPaneService>(std::move(backend), FilesPaneServiceLimits{}, wake.callback());
        }
        app=std::make_unique<FilesApp>(application,std::array<FilesPaneService*,2>{panes[0].get(),panes[1].get()},wake,std::array<NativeAbsolutePath,2>{pane_address(0),pane_address(1)},[this]{++quits;},ThemeScheme::Classic,std::array<FilesCompletionService*,2>{},nullptr,nullptr,std::shared_ptr<const NativeAbsolutePath>{},FilesSettingsBinding{std::move(loaded.value),std::move(loaded.diagnostic),persist?settings.get():nullptr});
        loaded_panes();}
    ~AppFixture(){files.gate.release();files.second_gate.release();app.reset();for(auto &pane:panes)pane->join();settings->request_close();settings->join();application.step(0);}
    void loaded_panes(){await([&]{application.step(clock.now_nanos());clock.advance(1'000'000);return !app->pending(0)&&!app->pending(1);});}
};
} // namespace

CK_TEST(files_settings_service_blocked_save_keeps_real_app_bookmarks_refresh_and_input_responsive) {
    AppFixture f;f.files.block=true;CK_CHECK(f.app->add_bookmark(0));f.files.gate.wait();
    CK_CHECK(f.app->status(0).find("Saving settings")!=std::string::npos);
    CK_CHECK(f.settings->progress().saved_revision==0);
    CK_CHECK(f.app->refresh(1));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Tab,ckv::Modifier::None,{}}}));
    f.loaded_panes();CK_CHECK(f.app->active_pane()==1);
    CK_CHECK(f.app->add_bookmark(1));CK_CHECK(f.app->remove_bookmark(0));
    CK_CHECK(f.app->bookmark_count(0)==0 && f.app->bookmark_count(1)==1);
    auto view=f.app->view(0);view.show_hidden=true;CK_CHECK(f.app->change_view(0,std::move(view)));f.loaded_panes();
    CK_CHECK(f.files.writes==1 && f.files.ui_io==0);
    f.app->request_close();CK_CHECK(f.quits==1 && !f.settings->drained());
    f.files.gate.release();f.settings->join();
    const auto persisted=parse_files_settings(f.files.observed()->contents);
    CK_CHECK(persisted && persisted->bookmarks[0].empty() && persisted->bookmarks[1].size()==1 &&
             persisted->bookmarks[1].front()==FilesPaneLocation(pane_address(1)) && persisted->views[0].show_hidden);
    CK_CHECK(f.files.writes==2 && f.files.overlap==0 && f.files.worker_changed==0 && f.files.ui_io==0);
    f.files.print_threads();
}
CK_TEST(files_settings_service_coalesces_serial_CAS_and_drains_latest_accepted_snapshot) {
    SettingsFS fs;FilesSettingsStore store(fs,"/settings/value");CK_CHECK(store.load(fs.bounded_reader()).diagnostic.empty());fs.enforce=true;fs.block=true;
    FilesSettingsService worker(store);Unblock release{fs};
    const auto first=worker.submit(snapshot(1));CK_CHECK(first.state==FilesSettingsAdmission::Accepted);fs.gate.wait();
    const auto skipped=worker.submit(snapshot(2));const auto last=worker.submit(snapshot(3));
    CK_CHECK(skipped.revision==first.revision+1 && last.revision==skipped.revision+1);
    CK_CHECK(!worker.take() && worker.progress().finished_revision==0 && worker.progress().saved_revision==0);
    worker.request_close();CK_CHECK(!worker.drained());CK_CHECK(worker.submit(snapshot(4)).state==FilesSettingsAdmission::Closed);
    fs.gate.release();worker.join();const auto reply=worker.take();const auto retained=worker.latest_result();
    CK_CHECK(reply && retained && reply->revision==last.revision && retained->revision==last.revision && retained->state==FilesSettingsWriteState::Saved);
    CK_CHECK(!worker.take() && worker.latest_result().has_value());
    CK_CHECK(fs.writes==2 && fs.attempted.size()==2 && fs.attempted[0]==files_settings_text(snapshot(1)) && fs.attempted[1]==files_settings_text(snapshot(3)));
    CK_CHECK(fs.expectations[0]==ckv::FileWriteExpectationKind::MustNotExist && fs.expectations[1]==ckv::FileWriteExpectationKind::MatchFingerprint);
    CK_CHECK(fs.observed()->contents==files_settings_text(snapshot(3)) && fs.ui_io==0 && fs.overlap==0);fs.print_threads();
}
CK_TEST(files_settings_service_preserves_external_conflict_and_retained_error_after_UI_take) {
    SettingsFS fs;fs.foreign_write(files_settings_text(snapshot(0)));FilesSettingsStore store(fs,"/settings/value");CK_CHECK(store.load(fs.bounded_reader()).diagnostic.empty());fs.enforce=true;fs.block=true;
    FilesSettingsService worker(store);Unblock release{fs};const auto accepted=worker.submit(snapshot(1));fs.gate.wait();
    const auto foreign=files_settings_text(snapshot(9));fs.foreign_write(foreign);fs.gate.release();await([&]{return worker.progress().finished_revision==accepted.revision;});
    const auto reply=worker.take();CK_CHECK(reply && reply->revision==accepted.revision && reply->state==FilesSettingsWriteState::StoreError && reply->message().find("changed in another process")!=std::string_view::npos);
    worker.request_close();worker.join();const auto retained=worker.latest_result();
    CK_CHECK(retained && retained->revision==accepted.revision && retained->message()==reply->message());
    CK_CHECK(worker.progress().saved_revision==0 && fs.observed()->contents==foreign && fs.writes==1 && fs.ui_io==0);
}
CK_TEST(files_settings_service_publication_then_exception_is_unknown_and_never_blindly_replayed) {
    SettingsFS fs;FilesSettingsStore store(fs,"/settings/value");store.load(fs.bounded_reader());fs.enforce=true;fs.throw_after_publish=true;
    FilesSettingsService worker(store);const auto first=worker.submit(snapshot(1));await([&]{return worker.progress().finished_revision==first.revision;});
    const auto unknown=worker.take();CK_CHECK(unknown && unknown->revision==first.revision && unknown->state==FilesSettingsWriteState::NoMemory && unknown->outcome_unknown);
    CK_CHECK(fs.observed()->contents==files_settings_text(snapshot(1)) && fs.writes==1 && worker.progress().saved_revision==0);
    const auto next=worker.submit(snapshot(2));worker.request_close();worker.join();const auto conflict=worker.latest_result();
    CK_CHECK(conflict && conflict->revision==next.revision && conflict->state==FilesSettingsWriteState::StoreError && conflict->message().find("changed in another process")!=std::string_view::npos);
    CK_CHECK(fs.observed()->contents==files_settings_text(snapshot(1)) && fs.writes==2 && fs.ui_io==0);
}
CK_TEST(files_settings_service_failed_wake_keeps_completion_available_for_nonblocking_UI_poll) {
    SettingsFS fs;FilesSettingsStore store(fs,"/settings/value");store.load(fs.bounded_reader());fs.enforce=true;
    FilesSettingsService worker(store,[]{throw std::bad_alloc();});const auto accepted=worker.submit(snapshot(1));worker.request_close();worker.join();
    const auto reply=worker.take();CK_CHECK(reply && reply->revision==accepted.revision && reply->state==FilesSettingsWriteState::Saved);
    CK_CHECK(worker.progress().wake_failed && worker.drained() && worker.latest_result()->revision==accepted.revision && fs.ui_io==0);
}
CK_TEST(files_settings_service_checks_real_codec_exact_1MiB_boundary_before_queue_or_IO) {
    FilesPersistentSettings value;
    for(unsigned i=0;i<15;++i)value.bookmarks[0].push_back(location("/"+std::string(32767,'x')));
    value.bookmarks[0].push_back(location("/"));
    // The file header makes root-only records odd-sized. One component adds
    // three encoded bytes, making the remaining two-hex-bytes/unit budget even.
    value.bookmarks[0][0]=take(value.bookmarks[0][0].descend(*NativeName::from_posix("x"),{},FilesPaneLocation::hard_components));
    const auto before=files_settings_text(value).size();const auto extra=(FilesSettingsBounds::bytes-before)/2;
    value.bookmarks[0].back()=location("/"+std::string(extra,'y'));
    const auto exact=files_settings_text(value);CK_CHECK(exact.size()==FilesSettingsBounds::bytes && parse_files_settings(exact).has_value());
    SettingsFS fs;FilesSettingsStore store(fs,"/settings/value");store.load(fs.bounded_reader());fs.enforce=true;FilesSettingsService worker(store);
    const auto accepted=worker.submit(value);CK_CHECK(accepted.state==FilesSettingsAdmission::Accepted);
    auto overflow=value;overflow.bookmarks[0].back()=take(location("/"+std::string(extra-1,'y')).descend(*NativeName::from_posix("z"),{},FilesPaneLocation::hard_components));
    const auto too_large=files_settings_text(overflow);CK_CHECK(too_large.size()==FilesSettingsBounds::bytes+1 && !parse_files_settings(too_large));
    CK_CHECK(worker.submit(overflow).state==FilesSettingsAdmission::Limit);
    worker.request_close();worker.join();CK_CHECK(fs.writes==1 && fs.observed()->contents==exact && fs.ui_io==0);
}
CK_TEST(files_settings_service_rejects_null_bookmarks_and_limits_without_truncating_previous_work) {
    SettingsFS fs;FilesSettingsStore store(fs,"/settings/value");store.load(fs.bounded_reader());fs.block=true;FilesSettingsService worker(store);Unblock release{fs};const auto accepted=worker.submit(snapshot(1));fs.gate.wait();
    ck::files::PaneView view;std::array<std::shared_ptr<const FilesPaneLocation>,1> nulls{};
    FilesSettingsSnapshotView invalid;invalid.views={&view,&view};invalid.bookmarks[0]=nulls;
    CK_CHECK(worker.submit(invalid).state==FilesSettingsAdmission::Invalid);
    auto excessive=snapshot(2);excessive.bookmarks[0].assign(257,location("/bookmark"));
    CK_CHECK(worker.submit(excessive).state==FilesSettingsAdmission::Limit);
    excessive=snapshot(3);excessive.views[0].display_substring.assign(4097,'x');CK_CHECK(worker.submit(excessive).state==FilesSettingsAdmission::Limit);
    CK_CHECK(worker.progress().accepted_revision==accepted.revision);fs.gate.release();worker.request_close();worker.join();CK_CHECK(fs.writes==1 && fs.observed()->contents==files_settings_text(snapshot(1)));
}

CK_TEST(files_settings_service_UI_ignores_stale_failed_revision_while_latest_snapshot_is_pending) {
    AppFixture f;f.files.throw_before_publish=true;f.files.block=true;f.files.block_second=true;
    CK_CHECK(f.app->add_bookmark(0));f.files.gate.wait();CK_CHECK(f.app->add_bookmark(1));
    f.files.gate.release();f.files.second_gate.wait();
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.app->status(0).find("Saving settings")!=std::string::npos);
    CK_CHECK(f.settings->progress().saved_revision==0 && f.files.writes==2);
    f.files.second_gate.release();await([&]{f.application.step(f.clock.now_nanos());f.clock.advance(1'000'000);return f.settings->progress().saved_revision==2 && f.app->status(0).find("Saving settings")==std::string::npos;});
    CK_CHECK(f.app->status(0).find("Saving settings")==std::string::npos);
    const auto value=parse_files_settings(f.files.observed()->contents);
    CK_CHECK(value && value->bookmarks[0].size()==1 && value->bookmarks[1].size()==1 && f.files.ui_io==0);
}
CK_TEST(files_settings_service_UI_bookmark_limit_rejects_insertion_without_truncation) {
    FilesPersistentSettings initial;
    for(unsigned i=0;i<256;++i)initial.bookmarks[0].push_back(location("/existing-"+std::to_string(i)));
    AppFixture f(initial);CK_CHECK(!f.app->add_bookmark(0));
    CK_CHECK(f.app->bookmark_count(0)==256 && f.files.writes==0);
    CK_CHECK(f.app->status(0).find("Settings limit exceeded")!=std::string::npos);
    CK_CHECK(f.settings->progress().accepted_revision==0);
}

CK_TEST(files_settings_service_absent_binding_is_deliberate_memory_only_mode_without_sync_fallback) {
    AppFixture f({},false);CK_CHECK(f.app->add_bookmark(0));
    auto view=f.app->view(0);view.show_hidden=true;CK_CHECK(f.app->change_view(0,std::move(view)));f.loaded_panes();
    CK_CHECK(f.files.writes==0 && f.files.ui_io==0 && f.settings->progress().accepted_revision==0);
    CK_CHECK(f.app->status(0).find("Saving settings")==std::string::npos);
    CK_CHECK(f.app->status(0).find("no settings worker")==std::string::npos);
}
