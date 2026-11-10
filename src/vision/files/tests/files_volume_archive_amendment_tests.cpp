// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_archive.hpp"
#include "native_filesystem.hpp"
#include "private_journal_scope.hpp"
#include "platform/journal_root_test_faults.hpp"
#include "platform/prepared_journal_root_bridge.hpp"
#include "archive_fault_allocator.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>
using namespace ck::files;using namespace ck::vision;
namespace {
struct Identity {dev_t dev=0;ino_t ino=0;bool known=false;};
using Fds=std::set<std::tuple<int,dev_t,ino_t,mode_t>>;
struct Control {
 std::atomic<bool> at_final=false,pending=false,release=false,published=false,reply_ready=false,observed_pending=false;
 Identity stage,journal,producer;int stage_closes=0,journal_closes=0,producer_closes=0,reused=-1;
 bool armed=false,fault_stage=false,fault_journal=false,producer_fault=false,oom=false,inject_done=false,cache_case=false;
 PreparedJournalRoot::Pointer root;Fds baseline;const char* mode=nullptr;
};
Control* control=nullptr;thread_local bool producer_role=false;
bool same(int fd,const Identity& id){struct stat s{};return id.known&&fstat(fd,&s)==0&&s.st_dev==id.dev&&s.st_ino==id.ino;}
Identity identify(int fd){struct stat s{};assert(fstat(fd,&s)==0);return {s.st_dev,s.st_ino,true};}
void wait(const std::atomic<bool>& flag,const char* label){for(unsigned i=0;i<10000;++i){if(flag.load(std::memory_order_acquire))return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}std::fprintf(stderr,"TIMEOUT %s\n",label);std::abort();}
Fds descriptors(){Fds out;DIR* d=opendir("/proc/self/fd");assert(d);const int own=dirfd(d);while(auto* e=readdir(d)){char* end=nullptr;long fd=strtol(e->d_name,&end,10);if(!end||*end||fd<0||fd==own)continue;struct stat s{};if(fstat(static_cast<int>(fd),&s)==0&&(S_ISREG(s.st_mode)||S_ISDIR(s.st_mode)))out.emplace(static_cast<int>(fd),s.st_dev,s.st_ino,s.st_mode&S_IFMT);}assert(closedir(d)==0);return out;}
void drained(){auto now=descriptors();for(const auto& fd:now)assert(control->baseline.contains(fd));assert(control->stage_closes==1&&control->journal_closes==1);}
void publication_pause()noexcept {
 if(!producer_role)return;
 ck::files::detail::PreparedJournalConsumerFailure first;
 // The same native fault seam also covers a journal's LOCAL latch. Only
 // suspend publication after the ACTUAL shared Root has reserved its slot.
 if(!ck::files::detail::read_prepared_journal_consumer_failure(*control->root,first)||!first.pending)return;
 control->pending.store(true,std::memory_order_release);while(!control->release.load(std::memory_order_acquire))std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
template<class T>T take(Observation<T> r){if(!r.value()){std::fprintf(stderr,"unexpected %d/%lld %s\n",int(r.state()),(long long)r.unavailable_detail()->error.code,r.unavailable_detail()->error.operation.c_str());std::abort();}return std::move(r).take_value();}
NativeName name(const char* s){return *NativeName::from_posix(s);}NativeAbsolutePath path(const std::string& s){return take(NativeAbsolutePath::from_posix(s));}
OperationJournalId id(){OperationJournalId i{};i.bytes[0]=std::byte{1};return i;}
std::vector<std::byte> bytes(const char* s){const auto* p=reinterpret_cast<const std::byte*>(s);return {p,p+strlen(s)};}
FilesVolumeRecord source(){return {"Native archive","/source","ext4",64,32,false,"source",{FilesVolumeArchiveSupport::LiveReadable,false,{}},{}};}
FilesVolumeArchiveDocument document(){FilesVolumeArchiveDocument d;d.source=source();d.entries={{".",FilesVolumeArchiveEntryKind::Directory,0,{},{},0},{"file",FilesVolumeArchiveEntryKind::RegularFile,1024*1024,{{0,bytes("data")},{1024*1024-4,bytes("tail")}}, {},0}};return d;}
std::vector<std::byte> read(const std::string& p){int fd=open(p.c_str(),O_RDONLY|O_CLOEXEC);assert(fd>=0);struct stat s{};assert(fstat(fd,&s)==0);std::vector<std::byte> b(s.st_size);assert(pread(fd,b.data(),b.size(),0)==static_cast<ssize_t>(b.size())&&close(fd)==0);return b;}
bool native(const NativeArchiveFailure& f){return f.domain==ErrorDomain::Posix&&f.code==ENOSPC&&std::strcmp(f.operation.data(),"journal_file.fsync")==0;}
void reuse_check(){if(control->reused>=0){assert(fcntl(control->reused,F_GETFD)>=0);}}
}
extern "C" int __real_ftruncate(int,off_t);
extern "C" int __wrap_ftruncate(int fd,off_t size){if(control&&control->armed&&!producer_role)control->stage=identify(fd);return __real_ftruncate(fd,size);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* b,size_t n,off_t at){if(control){if(producer_role)control->producer=identify(fd);else if(control->armed&&!same(fd,control->stage)&&!control->journal.known)control->journal=identify(fd);}return __real_pwrite(fd,b,n,at);}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){int r=__real_fsync(fd);if(control&&producer_role&&same(fd,control->producer)&&control->producer_fault){control->producer_fault=false;assert(r==0);errno=ENOSPC;return -1;}return r;}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd){bool stage=control&&control->armed&&same(fd,control->stage);bool journal=control&&control->armed&&same(fd,control->journal);bool producer=control&&same(fd,control->producer);if(stage)++control->stage_closes;if(journal)++control->journal_closes;if(producer)++control->producer_closes;int r=__real_close(fd);
 if(control&&!control->inject_done&&((stage&&control->fault_stage)||(journal&&control->fault_journal))){assert(r==0);control->inject_done=true;control->reused=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(control->reused==fd);if(control->oom)archive_test::deny_alloc=true;errno=EIO;return -1;}return r;}
namespace ck::vision::detail {
void archive_before_final_root_read(ck::files::PreparedJournalRoot& root)noexcept {
 if(!control||!control->cache_case)return;
 ck::files::detail::PreparedJournalConsumerFailure first;assert(!ck::files::detail::read_prepared_journal_consumer_failure(root,first));
 drained();control->at_final.store(true,std::memory_order_release);wait(control->pending,"publisher pending");
 if(control->oom){archive_test::deny_alloc=true;bool denied=false;try{void* p=::operator new(1);::operator delete(p);}catch(const std::bad_alloc&){denied=true;}assert(denied);}
}
void archive_final_root_read_observed(bool found,bool pending)noexcept {if(control&&control->cache_case&&found&&pending)control->observed_pending.store(true,std::memory_order_release);}
}
int main(int argc,char** argv){assert(argc==4&&getuid()!=0);const std::string base=argv[1],mode=argv[2];const bool old=std::atoi(argv[3])!=0;
 assert(mkdir((base+"/dest").c_str(),0700)==0&&mkdir((base+"/private").c_str(),0700)==0);int parent=open(base.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(parent>=0&&fsync(parent)==0&&close(parent)==0);
 Control c;control=&c;c.mode=argv[2];c.oom=mode.find("oom")!=std::string::npos;c.root=take(acquire_native_journal_root(path(base+"/private")));
 if(mode.starts_with("cache")){
  c.cache_case=true;producer_role=true;auto publisher=take(c.root->open_journal(name("publisher"),true));take(publisher->write_at(0,bytes("publisher")));producer_role=false;
  c.baseline=descriptors();c.armed=true;ck::files::detail::journal_root_test_publication_pause.store(publication_pause);
  FilesVolumeArchiveService service([&]{c.reply_ready.store(true,std::memory_order_release);});
  FilesVolumeArchiveRequest request{source(),path(base+"/dest"),"image.ckarchive",FilesVolumeArchiveConsistency::Live,1,[](auto){return Observation<FilesVolumeArchiveDocument>::available(document());},path(base+"/private"),id(),{}, {}};request.prepared_journal_root=c.root;
  auto accepted=service.request(std::move(request));assert(accepted.state==FilesVolumeArchiveSubmission::Accepted);wait(c.at_final,"final read boundary");
  std::thread producer([&]{producer_role=true;c.producer_fault=true;auto result=publisher->finish_io();assert(!result.value()&&result.unavailable_detail()->error.domain==ErrorDomain::Posix&&result.unavailable_detail()->error.code==ENOSPC);publisher.reset();c.published.store(true,std::memory_order_release);});
  wait(c.observed_pending,"authoritative read saw pending");
  if(old)wait(c.reply_ready,"baseline cached pending");
  else {std::this_thread::sleep_for(std::chrono::milliseconds(50));assert(!c.reply_ready.load(std::memory_order_acquire));}
  const bool returned_while_pending=c.reply_ready.load(std::memory_order_acquire);c.release.store(true,std::memory_order_release);producer.join();wait(c.reply_ready,"native cached reply");
  auto reply=service.take();assert(reply&&reply->failure_pod&&reply->state==FilesVolumeArchiveState::Indeterminate);service.request_stop();service.join();
  ck::files::detail::PreparedJournalConsumerFailure root;assert(ck::files::detail::read_prepared_journal_consumer_failure(*c.root,root)&&!root.pending&&root.domain==ErrorDomain::Posix&&root.code==ENOSPC);
  if(old){assert(returned_while_pending&&reply->failure_pod->domain==ErrorDomain::Adapter&&reply->failure_pod->code==static_cast<std::int64_t>(AdapterError::InvalidAccess));}
  else assert(!returned_while_pending&&native(*reply->failure_pod));
  const auto cached=*reply->failure_pod;for(unsigned i=0;i<10;++i){assert(!service.take());assert(cached.code==reply->failure_pod->code&&cached.operation==reply->failure_pod->operation);}
  drained();ck::files::detail::journal_root_test_publication_pause.store(nullptr);std::printf("CACHE old=%d oom=%d initial_clean=1 final_pending=1 returned_pending=%d cached=%d/%lld Root=Posix/ENOSPC consumer_closes=%d/%d\n",old,c.oom,returned_while_pending,int(cached.domain),(long long)cached.code,c.stage_closes,c.journal_closes);
 }else {
  NativeFileSystem fs;auto destination=std::shared_ptr<const DirectoryCursor>(take(fs.open_native_directory(path(base+"/dest"))));
  const bool pending=mode.find("pending")!=std::string::npos;std::unique_ptr<OperationJournalFile> publisher;
  if(pending){producer_role=true;publisher=take(c.root->open_journal(name("publisher"),true));take(publisher->write_at(0,bytes("p")));producer_role=false;}
  c.baseline=descriptors();NativeArchiveWriteRequest request{destination,c.root,name("archive"),name("hint"),name("journal"),id()};request.logical_size=1024*1024;request.extents={{0,bytes("data")},{1024*1024-4,bytes("tail")}};auto job=take(NativeArchiveWriteJob::begin(std::move(request)));c.armed=true;
  auto phase=mode.find("complete-journal")!=std::string::npos?NativeArchivePhase::CompleteJournal:mode.find("close-file")!=std::string::npos?NativeArchivePhase::CloseFile:NativeArchivePhase::Committed;
  for(unsigned i=0;i<1000&&job->progress().phase!=phase;++i){auto p=job->advance();assert(p.phase!=NativeArchivePhase::NeedsReconciliation);}assert(job->progress().phase==phase&&job->progress().published);
  bool fault=mode.find("fault")!=std::string::npos||c.oom||pending;c.fault_stage=fault&&phase!=NativeArchivePhase::CompleteJournal;c.fault_journal=fault&&phase==NativeArchivePhase::CompleteJournal;
  std::thread producer;
  if(pending){ck::files::detail::journal_root_test_publication_pause.store(publication_pause);producer=std::thread([&]{producer_role=true;c.producer_fault=true;auto r=publisher->finish_io();assert(!r.value());publisher.reset();c.published.store(true,std::memory_order_release);});wait(c.pending,"pending before finish");}
  const bool destructor=mode.find("dtor")!=std::string::npos;NativeArchiveProgress finished;
  if(destructor)job.reset();else finished=job->finish_io();
  if(pending){if(!destructor)assert(finished.phase==NativeArchivePhase::Drain&&!finished.drained);assert(c.stage_closes==1&&c.journal_closes==1);c.release.store(true,std::memory_order_release);producer.join();
   if(!destructor){finished=job->finish_io();assert(finished.phase==NativeArchivePhase::NeedsReconciliation&&finished.drained&&finished.failure&&native(*finished.failure));}
   ck::files::detail::journal_root_test_publication_pause.store(nullptr);}
  else if(!destructor){assert(finished.drained&&finished.published);if(!fault&&old)assert(finished.phase==NativeArchivePhase::Cancelled);else assert(finished.phase==NativeArchivePhase::NeedsReconciliation);}
  archive_test::deny_alloc=false;
  if(job){auto before=job->progress();const auto closes=c.stage_closes+c.journal_closes;for(unsigned i=0;i<10;++i){auto r=job->finish_io();assert(r.phase==before.phase&&r.drained==before.drained);}assert(c.stage_closes+c.journal_closes==closes);job.reset();}
  reuse_check();drained();
  c.armed=false;auto wire=read(base+"/private/journal");auto journal=take(decode_operation_journal(wire));assert(!journal.tail);const auto expected=phase==NativeArchivePhase::Committed?OperationJournalState::CommitIntended:OperationJournalState::DestinationCommitted;assert(journal.journal->state(0)==expected);
  auto image=read(base+"/dest/archive");assert(image.size()==1024*1024&&!memcmp(image.data(),"data",4)&&!memcmp(image.data()+image.size()-4,"tail",4));for(std::size_t i=4;i+4<image.size();++i)assert(image[i]==std::byte{});
  if(fault){ck::files::detail::PreparedJournalConsumerFailure first;assert(ck::files::detail::read_prepared_journal_consumer_failure(*c.root,first)&&!first.pending&&first.domain==ErrorDomain::Posix&&first.code==(pending?ENOSPC:EIO));}
  std::printf("FINISH old=%d mode=%s published=1 journal_incomplete=1 clean_cancel=%d stage/journal_closes=%d/%d Root_code=%d\n",old,mode.c_str(),!fault&&old&&!destructor,c.stage_closes,c.journal_closes,fault?(pending?ENOSPC:EIO):0);
 }
 c.armed=false;if(c.reused>=0){assert(fcntl(c.reused,F_GETFD)>=0);__real_close(c.reused);}c.root.reset();control=nullptr;
}
