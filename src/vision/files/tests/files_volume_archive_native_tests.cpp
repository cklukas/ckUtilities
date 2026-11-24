// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_archive.hpp"
#include "files_volume_source.hpp"
#include "native_filesystem.hpp"
#include "private_journal_scope.hpp"
#include "sha256.hpp"
#include "archive_fault_allocator.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <new>
#include <semaphore>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>
using namespace ck::files;using namespace ck::vision;
namespace {
using archive_test::deny_alloc;
const char* mode="roundtrip";bool armed=false,known=false,write_seen=false,close_fault=false,flush_fault=false,linked=false;
dev_t stage_dev=0;ino_t stage_ino=0;int closed=0,reused=-1;std::stop_source* cancel_source=nullptr;
std::string base_path;bool stage(int fd){struct stat s{};return known&&fstat(fd,&s)==0&&s.st_dev==stage_dev&&s.st_ino==stage_ino;}
bool is(const char* text){return !std::strcmp(mode,text);}
[[noreturn]] void interrupt_archive_process() {
    std::fprintf(stderr, "Interrupting archive at %s\n", mode);
    assert(std::fflush(stderr) == 0);
    assert(kill(getpid(), SIGKILL) == 0);
    std::abort();
}
std::binary_semaphore progress_entered(0), progress_continue(0);
bool verify_seen = false;
bool destination_moved = false;
bool replace_on_journal_read = false;
bool recovery_root_replaced = false;
ino_t expected_journal_inode = 0;
dev_t expected_journal_device = 0;
struct CaptureMutation {
    int file;
    int directory;
    dev_t device;
    ino_t inode;
    timespec changed_modified;
    bool directory_change;
    bool injected = false;
};
CaptureMutation* capture_mutation = nullptr;
struct AttributeFailure {
    dev_t device;
    ino_t inode;
    unsigned hits = 0;
};
AttributeFailure *attribute_failure = nullptr;
constexpr char fixture_attribute[] = "user.ckfiles-fixture";

bool fail_attribute_read(const char *path) {
    struct stat metadata{};
    if (!attribute_failure || stat(path, &metadata) != 0 ||
        metadata.st_dev != attribute_failure->device || metadata.st_ino != attribute_failure->inode)
        return false;
    ++attribute_failure->hits;
    errno = EIO;
    return true;
}
void replace_destination() {
    assert(!destination_moved);
    const auto original = base_path + "/dest";
    const auto moved = base_path + "/moved";
    assert(rename(original.c_str(), moved.c_str()) == 0);
    assert(mkdir(original.c_str(), 0700) == 0);
    destination_moved = true;
}
}
extern "C" ssize_t __real_listxattr(const char *, char *, std::size_t);
extern "C" ssize_t __real_getxattr(const char *, const char *, void *, std::size_t);
extern "C" ssize_t __wrap_listxattr(const char *path, char *list, std::size_t size) {
    if (is("source-attribute-catalog-io-error") && fail_attribute_read(path))
        return -1;
    return __real_listxattr(path, list, size);
}
extern "C" ssize_t __wrap_getxattr(const char *path, const char *name, void *value, std::size_t size) {
    if (is("source-attribute-value-io-error") && value && size != 0 && std::strcmp(name, fixture_attribute) == 0 &&
        fail_attribute_read(path))
        return -1;
    return __real_getxattr(path, name, value, size);
}
extern "C" ssize_t __real_pread(int, void *, size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void *, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void *data, size_t count, off_t offset) {
    if (is("service") && !verify_seen && stage(fd)) {
        verify_seen = true;
        progress_entered.release();
        assert(progress_continue.try_acquire_for(std::chrono::seconds(10)));
    }
    if (replace_on_journal_read) {
        struct stat opened{};
        assert(fstat(fd, &opened) == 0);
        assert(opened.st_ino == expected_journal_inode && opened.st_dev == expected_journal_device);
        const auto path = base_path + "/private";
        assert(rename(path.c_str(), (base_path + "/private-original").c_str()) == 0);
        assert(mkdir(path.c_str(), 0700) == 0);
        replace_on_journal_read = false;
        recovery_root_replaced = true;
    }
    const auto read = __real_pread(fd, data, count, offset);
    if (read > 0 && capture_mutation && !capture_mutation->injected) {
        auto& mutation = *capture_mutation;
        struct stat opened{};
        assert(fstat(fd, &opened) == 0);
        if (opened.st_dev == mutation.device && opened.st_ino == mutation.inode) {
            if (mutation.directory_change)
                assert(mkdirat(mutation.directory, "added-during-capture", 0700) == 0);
            else
                assert(__real_pwrite(mutation.file, "EDIT", 4, 0) == 4);
            const timespec timestamps[] = {{0, UTIME_OMIT}, mutation.changed_modified};
            assert(futimens(mutation.directory_change ? mutation.directory : mutation.file, timestamps) == 0);
            mutation.injected = true;
        }
    }
    return read;
}
extern "C" int __real_ftruncate(int,off_t);
extern "C" int __wrap_ftruncate(int fd,off_t n){if(armed){struct stat s{};assert(fstat(fd,&s)==0&&S_ISREG(s.st_mode)&&s.st_nlink==0);known=true;stage_dev=s.st_dev;stage_ino=s.st_ino;}
 return __real_ftruncate(fd,n);}
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t at){
 const auto r=__real_pwrite(fd,data,n,at);
 if(armed&&stage(fd)&&!write_seen){write_seen=true;
  if (is("crash-write") && r > 0)
      interrupt_archive_process();
  if (is("service") || is("service-cancel")) {
      progress_entered.release();
      assert(progress_continue.try_acquire_for(std::chrono::seconds(10)));
  }
  if (is("destination-moved-during-write")) replace_destination();
  if(is("hint-replaced")){const auto hint=base_path+"/dest/.image.ckarchive.cktmp-1";const auto saved=base_path+"/dest/saved";assert(rename(hint.c_str(),saved.c_str())==0);int x=open(hint.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(x>=0&&write(x,"replacement",11)==11&&close(x)==0);}
  if(is("cancel-in-write")||is("cancel-close")||is("cancel-close-oom")){assert(cancel_source);cancel_source->request_stop();}
  if(is("upstream-oom-close"))deny_alloc=true;
 }
 return r;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){
 if(armed&&stage(fd)&&is("first-enospc-close-oom")&&!flush_fault){flush_fault=true;errno=ENOSPC;return -1;}
 if(armed&&stage(fd)&&linked&&is("post-link-flush")&&!flush_fault){flush_fault=true;errno=EIO;return -1;}
 return __real_fsync(fd);
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd){const bool target=armed&&stage(fd);if(target)++closed;const int r=__real_close(fd);
 if (target && linked && is("crash-after-commit") && r == 0)
     interrupt_archive_process();
 if(target&&!close_fault&&(is("close-oom")||is("first-enospc-close-oom")||is("upstream-oom-close")||is("cancel-close")||is("cancel-close-oom"))){
  assert(r==0);close_fault=true;reused=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(reused==fd);
  if(!is("cancel-close"))deny_alloc=true;
  errno=EIO;return -1;}
 return r;}
extern "C" int __real_linkat(int,const char*,int,const char*,int);
extern "C" int __wrap_linkat(int a,const char* b,int c,const char* d,int e){
 const bool archive_link = armed && std::strcmp(d, "image.ckarchive") == 0;
 if (archive_link && is("crash-before-link"))
     interrupt_archive_process();
 if (armed && is("destination-moved-at-publish") && !destination_moved && !std::strcmp(d, "image.ckarchive"))
     replace_destination();
 const int r=__real_linkat(a,b,c,d,e);if(armed&&r==0){linked=true;
  if (archive_link && is("crash-after-link"))
      interrupt_archive_process();
  if(is("cancel-after-publish")){assert(cancel_source);cancel_source->request_stop();}}return r;}
namespace {
template<class T>T take(Observation<T> r){if(!r.value()){std::fprintf(stderr,"unavailable %d/%lld %s\n",int(r.state()),(long long)r.unavailable_detail()->error.code,r.unavailable_detail()->error.operation.c_str());std::abort();}return std::move(r).take_value();}
NativeName name(const char* s){return *NativeName::from_posix(s);}NativeAbsolutePath address(const std::string& s){return take(NativeAbsolutePath::from_posix(s));}
OperationJournalId id(){OperationJournalId x{};x.bytes[0]=std::byte{1};return x;}
std::vector<std::byte> bytes(const char* s){const auto* p=reinterpret_cast<const std::byte*>(s);return {p,p+strlen(s)};}
FilesVolumeRecord source(){return {"Fixture volume","/fixture","ext4",64,32,false,"archive-native-fixture",{FilesVolumeArchiveSupport::LiveReadable,false,{}},{}};}
FilesVolumeArchiveDocument document(){FilesVolumeArchiveDocument d;d.source=source();d.entries={{".",FilesVolumeArchiveEntryKind::Directory,0,{},{},0},{"sparse.bin",FilesVolumeArchiveEntryKind::RegularFile,2*1024*1024,{{0,bytes("DATA")},{2*1024*1024-4,bytes("TAIL")}}, {},0}};return d;}
std::string journal(){return ".ck-volume-archive-01000000000000000000000000000000";}
std::vector<std::byte> read_file(const std::string& path){int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);assert(fd>=0);struct stat s{};assert(fstat(fd,&s)==0);std::vector<std::byte> data(static_cast<std::size_t>(s.st_size));assert(pread(fd,data.data(),data.size(),0)==static_cast<ssize_t>(data.size()));assert(close(fd)==0);return data;}
std::uint64_t little(const std::vector<std::byte>& b,std::size_t at,unsigned n){assert(at+n<=b.size());std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return v;}
void verify_archive(const std::string& path,OperationJournalState expected_state=OperationJournalState::Complete,
                    const std::string& journal_leaf=journal()){auto b=read_file(path);assert(b.size()>56&&!memcmp(b.data(),"CKARV01\0",8)&&little(b,8,4)==2);const auto data=little(b,24,8);assert(data==56+little(b,16,8)&&little(b,32,8)==2*1024*1024&&little(b,40,8)==2&&b.size()==data+2*1024*1024);
 assert(!memcmp(b.data()+data,"DATA",4)&&!memcmp(b.data()+b.size()-4,"TAIL",4));for(std::size_t i=data+4;i+4<b.size();++i)assert(b[i]==std::byte{});
 struct stat s{};assert(stat(path.c_str(),&s)==0&&s.st_blocks*512<s.st_size/4);
 auto journal_bytes=read_file(base_path+"/private/"+journal_leaf);auto decoded=take(decode_operation_journal(journal_bytes));assert(!decoded.tail);
 auto recovery=take(inspect_volume_archive_recovery(*decoded.journal));assert(recovery.evidence.staging_namespace==StagingNamespace::Anonymous&&!recovery.evidence.staging_component&&!recovery.evidence.staging_encoding&&recovery.evidence.staging_native_path.empty());
 Sha256 hash;hash.update(b);assert(recovery.evidence.staging_digest&&*recovery.evidence.staging_digest==hash.digest()&&recovery.evidence.archive_logical_size==b.size());
 assert(decoded.journal->state(0)==expected_state);
}
}
namespace {
void live_capture_blocks_failed_attribute_observation() {
    const auto path = base_path + "/source";
    const auto file_path = path + "/payload";
    const auto scratch = base_path + "/scratch";
    const auto destination = base_path + "/dest";
    assert(mkdir(path.c_str(), 0700) == 0 && mkdir(scratch.c_str(), 0700) == 0 &&
           mkdir(destination.c_str(), 0700) == 0);
    const int file = open(file_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(file >= 0 && write(file, "DATA", 4) == 4);
    assert(fsetxattr(file, fixture_attribute, "meta", 4, XATTR_CREATE) == 0);
    struct stat metadata{};
    assert(fstat(file, &metadata) == 0 && close(file) == 0);
    AttributeFailure injection{metadata.st_dev, metadata.st_ino};

    NativeFileSystem filesystem;
    const auto root = take(filesystem.open_native_directory(address(path)));
    const auto root_metadata = take(root->inspect_basic());
    assert(root_metadata.identity.value() && root_metadata.mount_id.value());
    auto selected = source();
    selected.mount_point = path;
    selected.archive.root = {*root_metadata.identity.value(), *root_metadata.mount_id.value()};
    const auto review_capture = [&](const FilesVolumeArchiveSourceCapture &capture) {
        return take(FilesVolumeArchiveReview::create(capture.document, address(destination), "image.ckarchive",
            FilesVolumeArchiveDestinationState::Ready, capture.payload.scratch != nullptr));
    };
    {
        const auto control = take(capture_native_volume_archive_source(selected, address(scratch)));
        const auto review = review_capture(control);
        assert(review->summary().hard_losses == 0);
        assert(std::ranges::any_of(review->losses(), [](const FilesVolumeArchiveLoss &loss) {
            return loss.path == "payload" && loss.diagnostic == "extended attributes are not encoded";
        }));
        const std::vector<bool> all(review->losses().size(), true);
        assert(review->approve(FilesVolumeArchiveReviewDecision::ContinueWithListedLosses, all).value());
    }
    {
        attribute_failure = &injection;
        auto result = capture_native_volume_archive_source(selected, address(scratch));
        attribute_failure = nullptr;
        assert(injection.hits > 0);
        const auto failed = take(std::move(result));
        const auto review = review_capture(failed);
        assert(review->summary().hard_losses > 0);
        assert(std::ranges::any_of(review->losses(), [](const FilesVolumeArchiveLoss &loss) {
            return loss.path == "payload" && loss.kind == FilesVolumeArchiveLossKind::Metadata &&
                   loss.hard_failure && loss.diagnostic.find("extended-attribute") != std::string::npos;
        }));
        const std::vector<bool> all(review->losses().size(), true);
        assert(!review->approve(FilesVolumeArchiveReviewDecision::ContinueWithListedLosses, all).value());
    }
    assert(std::filesystem::is_empty(scratch) && std::filesystem::is_empty(destination));
    assert(read_file(file_path) == bytes("DATA"));
    char attribute[4]{};
    assert(getxattr(file_path.c_str(), fixture_attribute, attribute, sizeof(attribute)) ==
               static_cast<ssize_t>(sizeof(attribute)) &&
           std::memcmp(attribute, "meta", sizeof(attribute)) == 0);
    std::printf("%s PASS after %u injected native failures\n", mode, injection.hits);
}

void live_capture_refuses_observed_change(bool directory_change) {
    const auto path = base_path + "/source";
    assert(mkdir(path.c_str(), 0700) == 0);
    const int directory = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(directory >= 0);
    const int file = openat(directory, "payload", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(file >= 0 && __real_pwrite(file, "DATA", 4, 0) == 4);

    NativeFileSystem filesystem;
    auto root = take(filesystem.open_native_directory(address(path)));
    const auto metadata = take(root->inspect_basic());
    auto selected = source();
    selected.mount_point = path;
    assert(metadata.identity.value() && metadata.mount_id.value());
    selected.archive.root = {*metadata.identity.value(), *metadata.mount_id.value()};
    root.reset();
    assert(capture_native_volume_archive_source(selected, address(path)).value());

    struct stat file_stat{}, changed_stat{};
    assert(fstat(file, &file_stat) == 0);
    assert(fstat(directory_change ? directory : file, &changed_stat) == 0);
    auto modified = changed_stat.st_mtim;
    modified.tv_sec -= 10;
    CaptureMutation mutation{file, directory, file_stat.st_dev, file_stat.st_ino, modified, directory_change};
    capture_mutation = &mutation;
    const auto changed = capture_native_volume_archive_source(selected, address(path));
    capture_mutation = nullptr;
    assert(mutation.injected);
    assert(fstat(file, &file_stat) == 0 && file_stat.st_size == 4);
    assert(close(file) == 0 && close(directory) == 0);
    assert(!changed.value() && changed.state() == ObservationState::Changed);
    assert(changed.unavailable_detail()->error.operation ==
           (directory_change ? "volume_archive.source.directory_changed" : "volume_archive.source.entry_changed"));
}

void verify_unpublished_archive() {
    for (const auto *component : {"/dest/image.ckarchive", "/moved/image.ckarchive"}) {
        struct stat image{};
        assert(lstat((base_path + component).c_str(), &image) != 0 && errno == ENOENT);
    }
    const auto scan = take(discover_volume_archive_recovery(address(base_path + "/private")));
    assert(scan.journals.size() == 1 && scan.journals.front().report && scan.requires_reconciliation);
    const auto &report = *scan.journals.front().report;
    assert(report.journal_id != OperationJournalId{} && report.evidence.transaction_id != 0 &&
           !report.operation.has_untrusted_tail && report.operation.items.size() == 1 &&
           report.operation.items.front().state == OperationJournalState::Verified);
}

void scan_replaced_recovery_root() {
    struct stat original{};
    assert(stat((base_path + "/private/" + journal()).c_str(), &original) == 0);
    expected_journal_inode = original.st_ino;
    expected_journal_device = original.st_dev;
    replace_on_journal_read = true;
    const auto scan = discover_volume_archive_recovery(address(base_path + "/private"));
    assert(recovery_root_replaced && !scan.value() && scan.state() == ObservationState::Changed);
    assert(scan.unavailable_detail()->error.operation == "volume_archive.recovery_root_changed");
    struct stat retained{};
    assert(stat((base_path + "/private-original/" + journal()).c_str(), &retained) == 0);
    assert(retained.st_ino == original.st_ino && retained.st_size == original.st_size);
    assert(lstat((base_path + "/private/" + journal()).c_str(), &retained) == -1 && errno == ENOENT);
}

void reviewed_write_refuses_changed_admission() {
    FilesVolumeArchiveService worker;
    auto planned = worker.plan({
        source(), address(base_path + "/dest"), "image.ckarchive", FilesVolumeArchiveConsistency::Live,
        [](std::stop_token) {
            return Observation<FilesVolumeArchiveSourceCapture>::available(
                {document(), [](std::stop_token) { return Observation<bool>::available(!write_seen); }});
        }});
    assert(planned.state == FilesVolumeArchivePlanningSubmission::Accepted);
    std::optional<FilesVolumeArchivePlanningReply> review;
    for (unsigned i = 0; i < 5000 && !review; ++i) {
        review = worker.take_plan();
        if (!review) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(review && review->state == FilesVolumeArchivePlanningState::Ready && review->review);
    const bool late_journal = is("recovery-journal-appeared-during-review");
    const auto existing = base_path + "/private/" + journal();
    if (late_journal) {
        const int file = open(existing.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        assert(file >= 0 && write(file, "partial", 7) == 7 && close(file) == 0);
    }
    auto approved = take(review->review->approve(FilesVolumeArchiveReviewDecision::Proceed));
    auto started = worker.execute_review(planned.ticket, std::move(approved), address(base_path + "/private"));
    assert(started.state == FilesVolumeArchiveSubmission::Accepted);
    std::optional<FilesVolumeArchiveReply> result;
    for (unsigned i = 0; i < 5000 && !result; ++i) {
        result = worker.take();
        if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.request_stop();
    worker.join();
    assert(result && result->state == FilesVolumeArchiveState::Failed && result->error &&
           result->error->state == ObservationState::Changed);
    if (late_journal) {
        assert(result->error->error.operation == "volume_archive.recovery_admission_required");
        assert(!write_seen && closed == 0 && read_file(existing) == bytes("partial"));
        const auto scan = take(discover_volume_archive_recovery(address(base_path + "/private")));
        assert(scan.journals.size() == 1 && scan.requires_reconciliation);
        struct stat missing{};
        assert(lstat((base_path + "/dest/image.ckarchive").c_str(), &missing) == -1 && errno == ENOENT);
    } else {
        assert(result->error->error.operation == "volume_archive.source.mount_changed");
        assert(write_seen && closed == 1);
        verify_unpublished_archive();
    }

}

bool archive_crash_case() {
    return is("crash-write") || is("crash-before-link") ||
           is("crash-after-link") || is("crash-after-commit");
}

auto await_archive_reply(auto receive) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        if (auto reply = receive())
            return std::move(*reply);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    std::fprintf(stderr, "Timed out waiting for archive worker in %s\n", mode);
    std::abort();
}

FilesVolumeArchiveReply execute_crash_fixture_archive(const char *archive_name, bool interrupt) {
    const auto source_path = address(base_path + "/source");
    const auto scratch = address(base_path + "/scratch");
    NativeFileSystem filesystem;
    const auto directory = take(filesystem.open_native_directory(source_path));
    const auto metadata = take(directory->inspect_basic());
    assert(metadata.identity.value() && metadata.mount_id.value());
    auto selected = source();
    selected.mount_point = base_path + "/source";
    selected.archive.root = {*metadata.identity.value(), *metadata.mount_id.value()};
    FilesVolumeArchiveService worker;
    const auto planned = worker.plan({
        selected, address(base_path + "/dest"), archive_name, FilesVolumeArchiveConsistency::Live,
        [selected, scratch](std::stop_token stop) {
            return capture_native_volume_archive_source(selected, scratch, FilesVolumeArchiveConsistency::Live, stop);
        }});
    assert(planned.state == FilesVolumeArchivePlanningSubmission::Accepted);
    const auto reply = await_archive_reply([&] { return worker.take_plan(); });
    assert(reply.state == FilesVolumeArchivePlanningState::Ready && reply.review);
    const auto losses = reply.review->losses().size();
    auto approval = take(reply.review->approve(losses ? FilesVolumeArchiveReviewDecision::ContinueWithListedLosses
                                                    : FilesVolumeArchiveReviewDecision::Proceed,
                                             std::vector<bool>(losses, true)));
    armed = interrupt;
    const auto submitted = worker.execute_review(planned.ticket, std::move(approval), address(base_path + "/private"));
    assert(submitted.state == FilesVolumeArchiveSubmission::Accepted);
    return await_archive_reply([&] { return worker.take(); });
}

void create_crash_fixture_source() {
    assert(mkdir((base_path + "/source").c_str(), 0700) == 0);
    assert(mkdir((base_path + "/scratch").c_str(), 0700) == 0);
    const int content = open((base_path + "/source/sparse.bin").c_str(),
                             O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    assert(content >= 0 && pwrite(content, "DATA", 4, 0) == 4 &&
           pwrite(content, "TAIL", 4, 2 * 1024 * 1024 - 4) == 4 && fsync(content) == 0 && close(content) == 0);
}

void inspect_after_archive_crash() {
    using State = OperationJournalState;
    using Action = OperationRecoveryAction;
    const auto state = is("crash-write") ? State::Prepared
                     : is("crash-after-commit") ? State::DestinationCommitted : State::CommitIntended;
    const auto action = state == State::Prepared ? Action::InspectUnpublishedStaging
                      : state == State::DestinationCommitted ? Action::VerifyDestination
                      : Action::InspectAmbiguousPublication;
    const bool published = is("crash-after-link") || is("crash-after-commit");
    const auto source_path = base_path + "/source/sparse.bin";
    const auto destination = base_path + "/dest";
    const auto output = destination + "/image.ckarchive";
    const auto private_path = base_path + "/private";
    const auto journal_file = std::filesystem::directory_iterator(private_path);
    assert(journal_file != std::filesystem::directory_iterator{});
    const auto journal_path = journal_file->path();
    struct stat journal_before{}, archive_before{};
    assert(stat(journal_path.c_str(), &journal_before) == 0);
    if (published)
        assert(stat(output.c_str(), &archive_before) == 0);
    const auto initial_journal = read_file(journal_path.string());
    const auto original = read_file(source_path);
    std::vector<std::byte> expected(2 * 1024 * 1024);
    std::memcpy(expected.data(), "DATA", 4);
    std::memcpy(expected.data() + expected.size() - 4, "TAIL", 4);
    assert(original == expected);
    assert(std::filesystem::is_empty(base_path + "/scratch"));
    auto directory_size = [](const std::string &path) {
        return std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator{});
    };
    assert(directory_size(private_path) == 1 && directory_size(destination) == (published ? 1 : 0));
    const auto output_before = published ? read_file(output) : std::vector<std::byte>{};

    FilesVolumeArchiveRecoveryService recovery;
    assert(recovery.request(address(private_path)) == FilesVolumeArchiveRecoverySubmission::Accepted);
    const auto result = await_archive_reply([&] { return recovery.take(); });
    recovery.request_stop();
    recovery.join();
    assert(result.state == FilesVolumeArchiveRecoveryState::Ready && result.scan &&
           result.scan->requires_reconciliation && result.scan->journals.size() == 1);
    const auto &journal_item = result.scan->journals.front();
    assert(journal_item.native_name == name(journal_path.filename().c_str()));
    assert(journal_item.report && !journal_item.error);
    const auto &report = *journal_item.report;
    assert(report.evidence.archive_name == "image.ckarchive" &&
           report.evidence.staging_namespace == StagingNamespace::Anonymous &&
           !report.evidence.staging_component &&
           report.evidence.staging_digest.has_value() == (state != State::Prepared));
    assert(report.operation.requires_reconciliation && !report.operation.has_untrusted_tail &&
           report.operation.items.size() == 1);
    const auto &item = report.operation.items.front();
    assert(item.state == state && item.action == action && item.requires_reconciliation);
    if (published)
        verify_archive(output, state, journal_path.filename().string());

    // A different archive name avoids a publication conflict masking the
    // startup recovery gate on a subsequent reviewed Copy.
    const auto retry = execute_crash_fixture_archive("retry.ckarchive", false);
    assert(retry.state == FilesVolumeArchiveState::Failed && !retry.result && retry.error &&
           retry.error->error.operation == "volume_archive.recovery_admission_required");
    struct stat journal_after{}, archive_after{};
    assert(stat(journal_path.c_str(), &journal_after) == 0 &&
           journal_before.st_dev == journal_after.st_dev && journal_before.st_ino == journal_after.st_ino);
    assert(read_file(journal_path.string()) == initial_journal && directory_size(private_path) == 1);
    assert(directory_size(destination) == (published ? 1 : 0));
    if (published) {
        assert(stat(output.c_str(), &archive_after) == 0 &&
               archive_before.st_dev == archive_after.st_dev && archive_before.st_ino == archive_after.st_ino);
        assert(read_file(output) == output_before);
    }
    assert(read_file(source_path) == original && std::filesystem::is_empty(base_path + "/scratch"));
    std::printf("Restarted archive %s PASS published=%d state=%d\n", mode, published, static_cast<int>(state));
}
}

int main(int argc,char** argv){assert((argc==3||argc==4)&&getuid()!=0);mode=argv[2];base_path=argv[1];
 if (argc == 4) {
    assert(archive_crash_case() && std::strcmp(argv[3], "inspect") == 0);
    inspect_after_archive_crash();
    return 0;
 }
 if (is("source-edited-during-capture") || is("source-directory-changed-during-capture")) {
    live_capture_refuses_observed_change(is("source-directory-changed-during-capture"));
    return 0;
 }
 if (is("source-attribute-catalog-io-error") || is("source-attribute-value-io-error")) {
    live_capture_blocks_failed_attribute_observation();
    return 0;
 }
 assert(mkdir((base_path+"/dest").c_str(),0700)==0&&mkdir((base_path+"/private").c_str(),0700)==0);
 // Caller durably provisions the private Root namespace before acquisition.
 int parent=open(base_path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(parent>=0&&fsync(parent)==0&&close(parent)==0);
 if (archive_crash_case()) {
    create_crash_fixture_source();
    const auto result = execute_crash_fixture_archive("image.ckarchive", true);
    std::fprintf(stderr, "Writer returned %d before crash boundary%s%s\n", static_cast<int>(result.state),
                 result.error ? " at " : "", result.error ? result.error->error.operation.c_str() : "");
    return 2;
 }
 NativeFileSystem fs;auto dest=std::shared_ptr<const DirectoryCursor>(take(fs.open_native_directory(address(base_path+"/dest"))));auto root=take(acquire_native_journal_root(address(base_path+"/private")));
 if(is("no-owner")){FilesVolumeArchiveWriteRequest request{document(),base_path+"/nonexistent","image.ckarchive",1,{},{},{},{},{},{},{}};unsigned callbacks=0;request.append_journal=[&](auto){++callbacks;return Observation<bool>::available(true);};auto result=write_volume_archive(request);assert(!result.value()&&result.state()==ObservationState::Unsupported&&callbacks==0);dest.reset();take(root->finish_io());return 0;}
 if(is("occupied")){int fd=open((base_path+"/dest/image.ckarchive").c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0&&write(fd,"original",8)==8&&close(fd)==0);}
 if(is("hint-replaced")){int fd=open((base_path+"/dest/.image.ckarchive.cktmp-1").c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0&&write(fd,"original",8)==8&&close(fd)==0);}
 std::stop_source stop;cancel_source=&stop;armed=true;
 if (is("source-mount-changed-during-write") || is("recovery-journal-appeared-during-review")) {
  take(root->finish_io());
  reviewed_write_refuses_changed_admission();
  return 0;
 }
 const bool service_mode=is("service")||is("service-cancel")||is("service-legacy-root")||is("service-oom");
 if(service_mode){if(is("service-oom"))mode="upstream-oom-close";
  FilesVolumeArchiveService worker;FilesVolumeArchiveRequest request{source(),address(base_path+"/dest"),"image.ckarchive",FilesVolumeArchiveConsistency::Live,1,[](auto){return Observation<FilesVolumeArchiveDocument>::available(document());},address(base_path+"/private"),id(),{},[](const auto&,const auto&,bool)->Observation<std::unique_ptr<OperationJournalFile>>{std::abort();},{}};
  if(!is("service-legacy-root"))request.prepared_journal_root=root;
  auto submitted=worker.request(std::move(request));assert(submitted.state==FilesVolumeArchiveSubmission::Accepted);
  if (is("service") || is("service-cancel")) {
      using Phase = FilesVolumeArchivePhase;
      assert(progress_entered.try_acquire_for(std::chrono::seconds(10)));
      assert(!worker.progress({}));
      const auto writing = worker.progress(submitted.ticket);
      assert(writing && writing->phase == Phase::Writing && writing->completed_bytes == 0);
      if (is("service-cancel")) {
          assert(worker.cancel(submitted.ticket));
          const auto cancelled = worker.progress(submitted.ticket);
          assert(cancelled && cancelled->phase == Phase::CancelRequested && !worker.cancel(submitted.ticket));
      } else {
          progress_continue.release();
          assert(progress_entered.try_acquire_for(std::chrono::seconds(10)));
          const auto verifying = worker.progress(submitted.ticket);
          assert(verifying && verifying->phase == Phase::Verifying && verifying->completed_bytes == 0);
          // Only the read-back includes the sparse gap of the fixture's file.
          assert(verifying->total_bytes == writing->total_bytes + (2 * 1024 * 1024 - 8));
      }
      progress_continue.release();
  }
  std::optional<FilesVolumeArchiveReply> reply;for(unsigned i=0;i<30000&&!reply;++i){reply=worker.take();if(!reply)std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(reply);
  assert(!worker.progress(submitted.ticket));
  worker.request_stop();worker.join();
  if(is("upstream-oom-close")){assert(reply->state==FilesVolumeArchiveState::WorkerFault&&reply->failure_pod&&reply->failure_pod->domain==ErrorDomain::Posix&&reply->failure_pod->code==EIO&&close_fault);}
  else if (is("service-cancel")) {
      assert(reply->state == FilesVolumeArchiveState::Cancelled && !reply->result);
      struct stat image{};
      assert(lstat((base_path + "/dest/image.ckarchive").c_str(), &image) != 0 && errno == ENOENT);
  }
  else {assert(reply->state==FilesVolumeArchiveState::Complete&&reply->result&&reply->result->publication_confirmed);}
 }else{
  FilesVolumeArchiveWriteRequest request{document(),base_path+"/dest","image.ckarchive",1,{},{},{},{},{},{},{}};request.destination_owner=dest;request.prepared_journal_root=root;request.native_journal_name=name(journal().c_str());request.native_journal_id=id();
  try{auto result=write_volume_archive(request,stop.get_token());
   if(is("occupied"))assert(!result.value()&&result.state()==ObservationState::Changed);
   else if (is("destination-moved-during-write")) {
    assert(destination_moved && !result.value() && result.state() == ObservationState::Changed);
    assert(result.unavailable_detail()->error.operation == "volume_archive.review.destination_changed");
    verify_unpublished_archive();
   }
   else if (is("destination-moved-at-publish")) {
    assert(destination_moved && result.value() && result.value()->publication == FilesVolumeArchivePublication::Durable &&
           result.value()->publication_confirmed && !result.value()->published_path);
    struct stat image{};
    assert(lstat((base_path + "/dest/image.ckarchive").c_str(), &image) != 0 && errno == ENOENT);
   }
   else if(is("cancel-in-write"))assert(!result.value()&&result.unavailable_detail()->error.code==static_cast<std::int64_t>(AdapterError::InterruptedRead));
   else if(is("cancel-close"))assert(!result.value()&&result.unavailable_detail()->error.domain==ErrorDomain::Posix&&result.unavailable_detail()->error.code==EIO);
   else if(is("first-enospc-close-oom"))assert(!result.value()&&result.unavailable_detail()->error.domain==ErrorDomain::Posix&&result.unavailable_detail()->error.code==ENOSPC);
   else if(is("cancel-close-oom")||is("upstream-oom-close"))assert(!result.value()&&result.unavailable_detail()->error.domain==ErrorDomain::Posix&&result.unavailable_detail()->error.code==EIO);
   else if(is("close-oom"))assert(result.value()&&result.value()->publication==FilesVolumeArchivePublication::PublishedDurabilityUnknown&&result.value()->publication_confirmed);
   else if(is("post-link-flush"))assert(result.value()&&result.value()->publication==FilesVolumeArchivePublication::PublishedDurabilityUnknown&&!result.value()->publication_confirmed&&result.value()->digests_verified);
   else {assert(result.value()&&result.value()->publication==FilesVolumeArchivePublication::Durable&&result.value()->publication_confirmed);}
  }catch(const std::bad_alloc&){assert(is("close-oom")||is("first-enospc-close-oom")||is("upstream-oom-close")||is("cancel-close-oom"));}
 }
 // This ENOSPC path can format entirely in small-string storage. A separate
 // allocator control proves denial remains active without demanding that the
 // production drain allocate merely to exercise the fault switch.
 if(is("first-enospc-close-oom")&&archive_test::denied_allocations.load()==0){bool rejected=false;
  try{void* allocation=::operator new(1);::operator delete(allocation);}catch(const std::bad_alloc&){rejected=true;}assert(rejected);}
 deny_alloc=false;armed=false;assert(closed==1);if(is("close-oom")||is("first-enospc-close-oom")||is("upstream-oom-close")||is("cancel-close-oom"))assert(archive_test::denied_allocations.load()>0);if(reused>=0){assert(fcntl(reused,F_GETFD)>=0);__real_close(reused);}
 if(is("close-oom")||is("first-enospc-close-oom")||is("upstream-oom-close")||is("cancel-close")||is("cancel-close-oom")){
  detail::PreparedJournalConsumerFailure failure;assert(detail::read_prepared_journal_consumer_failure(*root,failure)&&!failure.pending&&failure.domain==ErrorDomain::Posix&&failure.code==(is("first-enospc-close-oom")?ENOSPC:EIO));}
 if(is("roundtrip")||is("service")||is("service-legacy-root")||is("cancel-after-publish")||is("hint-replaced"))verify_archive(base_path+"/dest/image.ckarchive");
 if(is("destination-moved-at-publish"))verify_archive(base_path+"/moved/image.ckarchive");
 if(is("post-link-flush"))verify_archive(base_path+"/dest/image.ckarchive",OperationJournalState::CommitIntended);
 if(is("occupied"))assert(read_file(base_path+"/dest/image.ckarchive")==bytes("original"));
 if(is("hint-replaced")){assert(read_file(base_path+"/dest/saved")==bytes("original")&&read_file(base_path+"/dest/.image.ckarchive.cktmp-1")==bytes("replacement"));}
 else {struct stat s{};assert(lstat((base_path+"/dest/.image.ckarchive.cktmp-1").c_str(),&s)!=0&&errno==ENOENT);}
 dest.reset();auto finished=root->finish_io();
 if (is("recovery-root-replaced")) {
    assert(finished.value() && *finished.value());
    scan_replaced_recovery_root();
 }
 if(is("roundtrip")||is("service")||is("service-legacy-root")||is("cancel-after-publish")||is("hint-replaced")||is("cancel-in-write"))assert(finished.value());
 std::printf("adapter mode=%s PASS original-creator-close=%d no-reopen/no-path-cleanup\n",mode,closed);
}
